//
//
// Copyright 2015 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#ifndef GRPC_SRC_CORE_LIB_TRANSPORT_TRANSPORT_H
#define GRPC_SRC_CORE_LIB_TRANSPORT_TRANSPORT_H

#include <grpc/support/port_platform.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <functional>
#include <string>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

#include <grpc/impl/connectivity_state.h>
#include <grpc/slice.h>
#include <grpc/status.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>

#include "src/core/lib/channel/context.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gprpp/debug_location.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/iomgr/call_combiner.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/iomgr_fwd.h"
#include "src/core/lib/iomgr/polling_entity.h"
#include "src/core/lib/promise/arena_promise.h"
#include "src/core/lib/promise/context.h"
#include "src/core/lib/promise/detail/status.h"
#include "src/core/lib/promise/latch.h"
#include "src/core/lib/promise/party.h"
#include "src/core/lib/promise/pipe.h"
#include "src/core/lib/promise/race.h"
#include "src/core/lib/promise/status_flag.h"
#include "src/core/lib/resource_quota/arena.h"
#include "src/core/lib/slice/slice_buffer.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/metadata_batch.h"
#include "src/core/lib/transport/transport_fwd.h"

// Minimum and maximum protocol accepted versions.
#define GRPC_PROTOCOL_VERSION_MAX_MAJOR 2
#define GRPC_PROTOCOL_VERSION_MAX_MINOR 1
#define GRPC_PROTOCOL_VERSION_MIN_MAJOR 2
#define GRPC_PROTOCOL_VERSION_MIN_MINOR 1

#define GRPC_ARG_TRANSPORT "grpc.internal.transport"

/// Internal bit flag for grpc_begin_message's \a flags signaling the use of
/// compression for the message. (Does not apply for stream compression.)
#define GRPC_WRITE_INTERNAL_COMPRESS (0x80000000u)
/// Internal bit flag for determining whether the message was compressed and had
/// to be decompressed by the message_decompress filter. (Does not apply for
/// stream compression.)
#define GRPC_WRITE_INTERNAL_TEST_ONLY_WAS_COMPRESSED (0x40000000u)
/// Mask of all valid internal flags.
#define GRPC_WRITE_INTERNAL_USED_MASK \
  (GRPC_WRITE_INTERNAL_COMPRESS | GRPC_WRITE_INTERNAL_TEST_ONLY_WAS_COMPRESSED)

namespace grpc_core {

// Server metadata type
// TODO(ctiller): This should be a bespoke instance of MetadataMap<>
using ServerMetadata = grpc_metadata_batch;
using ServerMetadataHandle = Arena::PoolPtr<ServerMetadata>;

// Client initial metadata type
// TODO(ctiller): This should be a bespoke instance of MetadataMap<>
using ClientMetadata = grpc_metadata_batch;
using ClientMetadataHandle = Arena::PoolPtr<ClientMetadata>;

class Message {
 public:
  Message() = default;
  ~Message() = default;
  Message(SliceBuffer payload, uint32_t flags)
      : payload_(std::move(payload)), flags_(flags) {}
  Message(const Message&) = delete;
  Message& operator=(const Message&) = delete;

  uint32_t flags() const { return flags_; }
  uint32_t& mutable_flags() { return flags_; }
  SliceBuffer* payload() { return &payload_; }
  const SliceBuffer* payload() const { return &payload_; }

  std::string DebugString() const;

 private:
  SliceBuffer payload_;
  uint32_t flags_ = 0;
};

using MessageHandle = Arena::PoolPtr<Message>;

// Ok/not-ok check for trailing metadata, so that it can be used as result types
// for TrySeq.
inline bool IsStatusOk(const ServerMetadataHandle& m) {
  return m->get(GrpcStatusMetadata()).value_or(GRPC_STATUS_UNKNOWN) ==
         GRPC_STATUS_OK;
}

ServerMetadataHandle ServerMetadataFromStatus(
    const absl::Status& status, Arena* arena = GetContext<Arena>());

template <>
struct StatusCastImpl<ServerMetadataHandle, absl::Status> {
  static ServerMetadataHandle Cast(const absl::Status& m) {
    return ServerMetadataFromStatus(m);
  }
};

template <>
struct StatusCastImpl<ServerMetadataHandle, const absl::Status&> {
  static ServerMetadataHandle Cast(const absl::Status& m) {
    return ServerMetadataFromStatus(m);
  }
};

template <>
struct StatusCastImpl<ServerMetadataHandle, absl::Status&> {
  static ServerMetadataHandle Cast(const absl::Status& m) {
    return ServerMetadataFromStatus(m);
  }
};

// Anything that can be first cast to absl::Status can then be cast to
// ServerMetadataHandle.
template <typename T>
struct StatusCastImpl<
    ServerMetadataHandle, T,
    absl::void_t<decltype(StatusCast<absl::Status>(std::declval<T>()))>> {
  static ServerMetadataHandle Cast(const T& m) {
    return ServerMetadataFromStatus(StatusCast<absl::Status>(m));
  }
};

// Move only type that tracks call startup.
// Allows observation of when client_initial_metadata has been processed by the
// end of the local call stack.
// Interested observers can call Wait() to obtain a promise that will resolve
// when all local client_initial_metadata processing has completed.
// The result of this token is either true on successful completion, or false
// if the metadata was not sent.
// To set a successful completion, call Complete(true). For failure, call
// Complete(false).
// If Complete is not called, the destructor of a still held token will complete
// with failure.
// Transports should hold this token until client_initial_metadata has passed
// any flow control (eg MAX_CONCURRENT_STREAMS for http2).
class ClientInitialMetadataOutstandingToken {
 public:
  static ClientInitialMetadataOutstandingToken Empty() {
    return ClientInitialMetadataOutstandingToken();
  }
  static ClientInitialMetadataOutstandingToken New(
      Arena* arena = GetContext<Arena>()) {
    ClientInitialMetadataOutstandingToken token;
    token.latch_ = arena->New<Latch<bool>>();
    return token;
  }

  ClientInitialMetadataOutstandingToken(
      const ClientInitialMetadataOutstandingToken&) = delete;
  ClientInitialMetadataOutstandingToken& operator=(
      const ClientInitialMetadataOutstandingToken&) = delete;
  ClientInitialMetadataOutstandingToken(
      ClientInitialMetadataOutstandingToken&& other) noexcept
      : latch_(std::exchange(other.latch_, nullptr)) {}
  ClientInitialMetadataOutstandingToken& operator=(
      ClientInitialMetadataOutstandingToken&& other) noexcept {
    latch_ = std::exchange(other.latch_, nullptr);
    return *this;
  }
  ~ClientInitialMetadataOutstandingToken() {
    if (latch_ != nullptr) latch_->Set(false);
  }
  void Complete(bool success) { std::exchange(latch_, nullptr)->Set(success); }

  // Returns a promise that will resolve when this object (or its moved-from
  // ancestor) is dropped.
  auto Wait() { return latch_->Wait(); }

 private:
  ClientInitialMetadataOutstandingToken() = default;

  Latch<bool>* latch_ = nullptr;
};

using ClientInitialMetadataOutstandingTokenWaitType =
    decltype(std::declval<ClientInitialMetadataOutstandingToken>().Wait());

struct CallArgs {
  // Initial metadata from the client to the server.
  // During promise setup this can be manipulated by filters (and then
  // passed on to the next filter).
  ClientMetadataHandle client_initial_metadata;
  // Token indicating that client_initial_metadata is still being processed.
  // This should be moved around and only destroyed when the transport is
  // satisfied that the metadata has passed any flow control measures it has.
  ClientInitialMetadataOutstandingToken client_initial_metadata_outstanding;
  // Latch that will ultimately contain the polling entity for the call.
  // TODO(ctiller): remove once event engine lands
  Latch<grpc_polling_entity>* polling_entity;
  // Initial metadata from the server to the client.
  // Set once when it's available.
  // During promise setup filters can substitute their own latch for this
  // and consequently intercept the sent value and mutate/observe it.
  PipeSender<ServerMetadataHandle>* server_initial_metadata;
  // Messages travelling from the application to the transport.
  PipeReceiver<MessageHandle>* client_to_server_messages;
  // Messages travelling from the transport to the application.
  PipeSender<MessageHandle>* server_to_client_messages;
};

using NextPromiseFactory =
    std::function<ArenaPromise<ServerMetadataHandle>(CallArgs)>;

// The common middle part of a call - a reference is held by each of
// CallInitiator and CallHandler - which provide interfaces that are appropriate
// for each side of a call.
// The spine will ultimately host the pipes, filters, and context for one part
// of a call: ie top-half client channel, sub channel call, server call.
// TODO(ctiller): eventually drop this when we don't need to reference into
// legacy promise calls anymore
class CallSpineInterface {
 public:
  virtual ~CallSpineInterface() = default;
  virtual Pipe<ClientMetadataHandle>& client_initial_metadata() = 0;
  virtual Pipe<ServerMetadataHandle>& server_initial_metadata() = 0;
  virtual Pipe<MessageHandle>& client_to_server_messages() = 0;
  virtual Pipe<MessageHandle>& server_to_client_messages() = 0;
  virtual Pipe<ServerMetadataHandle>& server_trailing_metadata() = 0;
  virtual Latch<ServerMetadataHandle>& cancel_latch() = 0;
  virtual Party& party() = 0;
  virtual void IncrementRefCount() = 0;
  virtual void Unref() = 0;

  // Cancel the call with the given metadata.
  // Regarding the `MUST_USE_RESULT absl::nullopt_t`:
  // Most cancellation calls right now happen in pipe interceptors;
  // there `nullopt` indicates terminate processing of this pipe and close with
  // error.
  // It's convenient then to have the Cancel operation (setting the latch to
  // terminate the call) be the last thing that occurs in a pipe interceptor,
  // and this construction supports that (and has helped the author not write
  // some bugs).
  GRPC_MUST_USE_RESULT absl::nullopt_t Cancel(ServerMetadataHandle metadata) {
    GPR_DEBUG_ASSERT(Activity::current() == &party());
    auto& c = cancel_latch();
    if (c.is_set()) return absl::nullopt;
    c.Set(std::move(metadata));
    return absl::nullopt;
  }

  auto WaitForCancel() {
    GPR_DEBUG_ASSERT(Activity::current() == &party());
    return cancel_latch().Wait();
  }

  // Wrap a promise so that if it returns failure it automatically cancels
  // the rest of the call.
  // The resulting (returned) promise will resolve to Empty.
  template <typename Promise>
  auto CancelIfFails(Promise promise) {
    GPR_DEBUG_ASSERT(Activity::current() == &party());
    using P = promise_detail::PromiseLike<Promise>;
    using ResultType = typename P::Result;
    return Map(std::move(promise), [this](ResultType r) {
      if (!IsStatusOk(r)) {
        std::ignore = Cancel(StatusCast<ServerMetadataHandle>(r));
      }
      return r;
    });
  }

  // Spawn a promise that returns Empty{} and save some boilerplate handling
  // that detail.
  template <typename PromiseFactory>
  void SpawnInfallible(absl::string_view name, PromiseFactory promise_factory) {
    party().Spawn(name, std::move(promise_factory), [](Empty) {});
  }

  // Spawn a promise that returns some status-like type; if the status
  // represents failure automatically cancel the rest of the call.
  template <typename PromiseFactory>
  void SpawnGuarded(absl::string_view name, PromiseFactory promise_factory) {
    using FactoryType =
        promise_detail::OncePromiseFactory<void, PromiseFactory>;
    using PromiseType = typename FactoryType::Promise;
    using ResultType = typename PromiseType::Result;
    static_assert(
        std::is_same<bool,
                     decltype(IsStatusOk(std::declval<ResultType>()))>::value,
        "SpawnGuarded promise must return a status-like object");
    party().Spawn(name, std::move(promise_factory), [this](ResultType r) {
      if (!IsStatusOk(r)) {
        std::ignore = Cancel(StatusCast<ServerMetadataHandle>(std::move(r)));
      }
    });
  }
};

class CallSpine final : public CallSpineInterface {
 public:
  Pipe<ClientMetadataHandle>& client_initial_metadata() override {
    return client_initial_metadata_;
  }
  Pipe<ServerMetadataHandle>& server_initial_metadata() override {
    return server_initial_metadata_;
  }
  Pipe<MessageHandle>& client_to_server_messages() override {
    return client_to_server_messages_;
  }
  Pipe<MessageHandle>& server_to_client_messages() override {
    return server_to_client_messages_;
  }
  Pipe<ServerMetadataHandle>& server_trailing_metadata() override {
    return server_trailing_metadata_;
  }
  Latch<ServerMetadataHandle>& cancel_latch() override { return cancel_latch_; }
  Party& party() override { Crash("unimplemented"); }
  void IncrementRefCount() override { Crash("unimplemented"); }
  void Unref() override { Crash("unimplemented"); }

 private:
  // Initial metadata from client to server
  Pipe<ClientMetadataHandle> client_initial_metadata_;
  // Initial metadata from server to client
  Pipe<ServerMetadataHandle> server_initial_metadata_;
  // Messages travelling from the application to the transport.
  Pipe<MessageHandle> client_to_server_messages_;
  // Messages travelling from the transport to the application.
  Pipe<MessageHandle> server_to_client_messages_;
  // Trailing metadata from server to client
  Pipe<ServerMetadataHandle> server_trailing_metadata_;
  // Latch that can be set to terminate the call
  Latch<ServerMetadataHandle> cancel_latch_;
};

class CallInitiator {
 public:
  explicit CallInitiator(RefCountedPtr<CallSpine> spine)
      : spine_(std::move(spine)) {}

  auto PushClientInitialMetadata(ClientMetadataHandle md) {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return Map(spine_->client_initial_metadata().sender.Push(std::move(md)),
               [](bool ok) { return StatusFlag(ok); });
  }

  auto PullServerInitialMetadata() {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return Map(spine_->server_initial_metadata().receiver.Next(),
               [](NextResult<ClientMetadataHandle> md)
                   -> ValueOrFailure<ClientMetadataHandle> {
                 if (!md.has_value()) return Failure{};
                 return std::move(*md);
               });
  }

  auto PullServerTrailingMetadata() {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return Race(spine_->WaitForCancel(),
                Map(spine_->server_trailing_metadata().receiver.Next(),
                    [spine = spine_](NextResult<ServerMetadataHandle> md)
                        -> ServerMetadataHandle {
                      GPR_ASSERT(md.has_value());
                      return std::move(*md);
                    }));
  }

  auto PullMessage() {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return spine_->server_to_client_messages().receiver.Next();
  }

  auto PushMessage(MessageHandle message) {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return spine_->client_to_server_messages().sender.Push(std::move(message));
  }

  template <typename Promise>
  auto CancelIfFails(Promise promise) {
    return spine_->CancelIfFails(std::move(promise));
  }

  template <typename PromiseFactory>
  void SpawnGuarded(absl::string_view name, PromiseFactory promise_factory) {
    spine_->SpawnGuarded(name, std::move(promise_factory));
  }

  template <typename PromiseFactory>
  void SpawnInfallible(absl::string_view name, PromiseFactory promise_factory) {
    spine_->SpawnInfallible(name, std::move(promise_factory));
  }

  template <typename PromiseFactory>
  auto SpawnWaitable(absl::string_view name, PromiseFactory promise_factory) {
    return spine_->party().SpawnWaitable(name, std::move(promise_factory));
  }

 private:
  const RefCountedPtr<CallSpine> spine_;
};

class CallHandler {
 public:
  explicit CallHandler(RefCountedPtr<CallSpine> spine)
      : spine_(std::move(spine)) {}

  auto PullClientInitialMetadata() {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return Map(spine_->client_initial_metadata().receiver.Next(),
               [](NextResult<ClientMetadataHandle> md)
                   -> ValueOrFailure<ClientMetadataHandle> {
                 if (!md.has_value()) return Failure{};
                 return std::move(*md);
               });
  }

  auto PushServerInitialMetadata(ClientMetadataHandle md) {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return Map(spine_->server_initial_metadata().sender.Push(std::move(md)),
               [](bool ok) { return StatusFlag(ok); });
  }

  auto PushServerTrailingMetadata(ClientMetadataHandle md) {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return Map(spine_->server_initial_metadata().sender.Push(std::move(md)),
               [](bool ok) { return StatusFlag(ok); });
  }

  auto PullMessage() {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return spine_->client_to_server_messages().receiver.Next();
  }

  auto PushMessage(MessageHandle message) {
    GPR_DEBUG_ASSERT(Activity::current() == &spine_->party());
    return spine_->server_to_client_messages().sender.Push(std::move(message));
  }

  template <typename Promise>
  auto CancelIfFails(Promise promise) {
    return spine_->CancelIfFails(std::move(promise));
  }

  template <typename PromiseFactory>
  void SpawnGuarded(absl::string_view name, PromiseFactory promise_factory) {
    spine_->SpawnGuarded(name, std::move(promise_factory));
  }

  template <typename PromiseFactory>
  void SpawnInfallible(absl::string_view name, PromiseFactory promise_factory) {
    spine_->SpawnInfallible(name, std::move(promise_factory));
  }

  template <typename PromiseFactory>
  auto SpawnWaitable(absl::string_view name, PromiseFactory promise_factory) {
    return spine_->party().SpawnWaitable(name, std::move(promise_factory));
  }

 private:
  const RefCountedPtr<CallSpine> spine_;
};

template <typename CallHalf>
auto OutgoingMessages(CallHalf& h) {
  struct Wrapper {
    CallHalf& h;
    auto Next() { return h.PullMessage(); }
  };
  return Wrapper{h};
}

// Forward a call from `call_handler` to `call_initiator` (with initial metadata
// `client_initial_metadata`)
void ForwardCall(CallHandler call_handler, CallInitiator call_initiator,
                 ClientMetadataHandle client_initial_metadata);

}  // namespace grpc_core

// forward declarations

// grpc_stream doesn't actually exist. It's used as a typesafe
// opaque pointer for whatever data the transport wants to track
// for a stream.
typedef struct grpc_stream grpc_stream;

extern grpc_core::DebugOnlyTraceFlag grpc_trace_stream_refcount;

typedef struct grpc_stream_refcount {
  grpc_core::RefCount refs;
  grpc_closure destroy;
#ifndef NDEBUG
  const char* object_type;
#endif
} grpc_stream_refcount;

#ifndef NDEBUG
void grpc_stream_ref_init(grpc_stream_refcount* refcount, int initial_refs,
                          grpc_iomgr_cb_func cb, void* cb_arg,
                          const char* object_type);
#define GRPC_STREAM_REF_INIT(rc, ir, cb, cb_arg, objtype) \
  grpc_stream_ref_init(rc, ir, cb, cb_arg, objtype)
#else
void grpc_stream_ref_init(grpc_stream_refcount* refcount, int initial_refs,
                          grpc_iomgr_cb_func cb, void* cb_arg);
#define GRPC_STREAM_REF_INIT(rc, ir, cb, cb_arg, objtype) \
  do {                                                    \
    grpc_stream_ref_init(rc, ir, cb, cb_arg);             \
    (void)(objtype);                                      \
  } while (0)
#endif

#ifndef NDEBUG
inline void grpc_stream_ref(grpc_stream_refcount* refcount,
                            const char* reason) {
  if (grpc_trace_stream_refcount.enabled()) {
    gpr_log(GPR_DEBUG, "%s %p:%p REF %s", refcount->object_type, refcount,
            refcount->destroy.cb_arg, reason);
  }
  refcount->refs.RefNonZero(DEBUG_LOCATION, reason);
}
#else
inline void grpc_stream_ref(grpc_stream_refcount* refcount) {
  refcount->refs.RefNonZero();
}
#endif

void grpc_stream_destroy(grpc_stream_refcount* refcount);

#ifndef NDEBUG
inline void grpc_stream_unref(grpc_stream_refcount* refcount,
                              const char* reason) {
  if (grpc_trace_stream_refcount.enabled()) {
    gpr_log(GPR_DEBUG, "%s %p:%p UNREF %s", refcount->object_type, refcount,
            refcount->destroy.cb_arg, reason);
  }
  if (GPR_UNLIKELY(refcount->refs.Unref(DEBUG_LOCATION, reason))) {
    grpc_stream_destroy(refcount);
  }
}
#else
inline void grpc_stream_unref(grpc_stream_refcount* refcount) {
  if (GPR_UNLIKELY(refcount->refs.Unref())) {
    grpc_stream_destroy(refcount);
  }
}
#endif

// Wrap a buffer that is owned by some stream object into a slice that shares
// the same refcount
grpc_slice grpc_slice_from_stream_owned_buffer(grpc_stream_refcount* refcount,
                                               void* buffer, size_t length);

struct grpc_transport_one_way_stats {
  uint64_t framing_bytes = 0;
  uint64_t data_bytes = 0;
  uint64_t header_bytes = 0;
};

struct grpc_transport_stream_stats {
  grpc_transport_one_way_stats incoming;
  grpc_transport_one_way_stats outgoing;
  gpr_timespec latency = gpr_inf_future(GPR_TIMESPAN);
};

void grpc_transport_move_one_way_stats(grpc_transport_one_way_stats* from,
                                       grpc_transport_one_way_stats* to);

void grpc_transport_move_stats(grpc_transport_stream_stats* from,
                               grpc_transport_stream_stats* to);

// This struct (which is present in both grpc_transport_stream_op_batch
// and grpc_transport_op_batch) is a convenience to allow filters or
// transports to schedule a closure related to a particular batch without
// having to allocate memory.  The general pattern is to initialize the
// closure with the callback arg set to the batch and extra_arg set to
// whatever state is associated with the handler (e.g., the call element
// or the transport stream object).
//
// Note that this can only be used by the current handler of a given
// batch on the way down the stack (i.e., whichever filter or transport is
// currently handling the batch).  Once a filter or transport passes control
// of the batch to the next handler, it cannot depend on the contents of
// this struct anymore, because the next handler may reuse it.
struct grpc_handler_private_op_data {
  void* extra_arg = nullptr;
  grpc_closure closure;
  grpc_handler_private_op_data() { memset(&closure, 0, sizeof(closure)); }
};

typedef struct grpc_transport_stream_op_batch_payload
    grpc_transport_stream_op_batch_payload;

// Transport stream op: a set of operations to perform on a transport
// against a single stream
struct grpc_transport_stream_op_batch {
  grpc_transport_stream_op_batch()
      : send_initial_metadata(false),
        send_trailing_metadata(false),
        send_message(false),
        recv_initial_metadata(false),
        recv_message(false),
        recv_trailing_metadata(false),
        cancel_stream(false),
        is_traced(false) {}

  /// Should be scheduled when all of the non-recv operations in the batch
  /// are complete.

  /// The recv ops (recv_initial_metadata, recv_message, and
  /// recv_trailing_metadata) each have their own callbacks.  If a batch
  /// contains both recv ops and non-recv ops, on_complete should be
  /// scheduled as soon as the non-recv ops are complete, regardless of
  /// whether or not the recv ops are complete.  If a batch contains
  /// only recv ops, on_complete can be null.
  grpc_closure* on_complete = nullptr;

  /// Values for the stream op (fields set are determined by flags above)
  grpc_transport_stream_op_batch_payload* payload = nullptr;

  /// Send initial metadata to the peer, from the provided metadata batch.
  bool send_initial_metadata : 1;

  /// Send trailing metadata to the peer, from the provided metadata batch.
  bool send_trailing_metadata : 1;

  /// Send message data to the peer, from the provided byte stream.
  bool send_message : 1;

  /// Receive initial metadata from the stream, into provided metadata batch.
  bool recv_initial_metadata : 1;

  /// Receive message data from the stream, into provided byte stream.
  bool recv_message : 1;

  /// Receive trailing metadata from the stream, into provided metadata batch.
  ///
  bool recv_trailing_metadata : 1;

  /// Cancel this stream with the provided error
  bool cancel_stream : 1;

  /// Is this stream traced
  bool is_traced : 1;

  bool HasOp() const {
    return send_initial_metadata || send_trailing_metadata || send_message ||
           recv_initial_metadata || recv_message || recv_trailing_metadata ||
           cancel_stream;
  }

  //**************************************************************************
  // remaining fields are initialized and used at the discretion of the
  // current handler of the op

  grpc_handler_private_op_data handler_private;
};

struct grpc_transport_stream_op_batch_payload {
  explicit grpc_transport_stream_op_batch_payload(
      grpc_call_context_element* context)
      : context(context) {}
  struct {
    grpc_metadata_batch* send_initial_metadata = nullptr;
  } send_initial_metadata;

  struct {
    grpc_metadata_batch* send_trailing_metadata = nullptr;
    // Set by the transport to true if the stream successfully wrote the
    // trailing metadata. If this is not set but there was a send trailing
    // metadata op present, this can indicate that a server call can be marked
    // as  a cancellation (since the stream was write-closed before status could
    // be delivered).
    bool* sent = nullptr;
  } send_trailing_metadata;

  struct {
    // The transport (or a filter that decides to return a failure before
    // the op gets down to the transport) takes ownership.
    // The batch's on_complete will not be called until after the byte
    // stream is orphaned.
    grpc_core::SliceBuffer* send_message;
    uint32_t flags = 0;
    // Set by the transport if the stream has been closed for writes. If this
    // is set and send message op is present, we set the operation to be a
    // failure without sending a cancel OP down the stack. This is so that the
    // status of the call does not get overwritten by the Cancel OP, which would
    // be especially problematic if we had received a valid status from the
    // server.
    // For send_initial_metadata, it is fine for the status to be overwritten
    // because at that point, the client will not have received a status.
    // For send_trailing_metadata, we might overwrite the status if we have
    // non-zero metadata to send. This is fine because the API does not allow
    // the client to send trailing metadata.
    bool stream_write_closed = false;
  } send_message;

  struct {
    grpc_metadata_batch* recv_initial_metadata = nullptr;
    /// Should be enqueued when initial metadata is ready to be processed.
    grpc_closure* recv_initial_metadata_ready = nullptr;
    // If not NULL, will be set to true if trailing metadata is
    // immediately available. This may be a signal that we received a
    // Trailers-Only response. The retry filter checks this to know whether to
    // defer the decision to commit the call or not. The C++ callback API also
    // uses this to set the success flag of OnReadInitialMetadataDone()
    // callback.
    bool* trailing_metadata_available = nullptr;
  } recv_initial_metadata;

  struct {
    // Will be set by the transport to point to the byte stream containing a
    // received message. Will be nullopt if trailing metadata is received
    // instead of a message.
    absl::optional<grpc_core::SliceBuffer>* recv_message = nullptr;
    uint32_t* flags = nullptr;
    // Was this recv_message failed for reasons other than a clean end-of-stream
    bool* call_failed_before_recv_message = nullptr;
    /// Should be enqueued when one message is ready to be processed.
    grpc_closure* recv_message_ready = nullptr;
  } recv_message;

  struct {
    grpc_metadata_batch* recv_trailing_metadata = nullptr;
    grpc_transport_stream_stats* collect_stats = nullptr;
    /// Should be enqueued when trailing metadata is ready to be processed.
    grpc_closure* recv_trailing_metadata_ready = nullptr;
  } recv_trailing_metadata;

  /// Forcefully close this stream.
  /// The HTTP2 semantics should be:
  /// - server side: if cancel_error has
  /// grpc_core::StatusIntProperty::kRpcStatus, and trailing metadata has not
  /// been sent, send trailing metadata with status and message from
  /// cancel_error (use grpc_error_get_status) followed by a RST_STREAM with
  /// error=GRPC_CHTTP2_NO_ERROR to force a full close
  /// - at all other times: use grpc_error_get_status to get a status code, and
  ///   convert to a HTTP2 error code using
  ///   grpc_chttp2_grpc_status_to_http2_error. Send a RST_STREAM with this
  ///   error.
  struct {
    // Error contract: the transport that gets this op must cause cancel_error
    //                 to be unref'ed after processing it
    grpc_error_handle cancel_error;
    // If true the transport should endeavor to delay sending the cancellation
    // notification for some small amount of time, in order to foil certain
    // exploits.
    // This should be set for cancellations that result from malformed client
    // initial metadata.
    bool tarpit = false;
  } cancel_stream;

  // Indexes correspond to grpc_context_index enum values
  grpc_call_context_element* context;
};

/// Transport op: a set of operations to perform on a transport as a whole
typedef struct grpc_transport_op {
  /// Called when processing of this op is done.
  grpc_closure* on_consumed = nullptr;
  /// connectivity monitoring - set connectivity_state to NULL to unsubscribe
  grpc_core::OrphanablePtr<grpc_core::ConnectivityStateWatcherInterface>
      start_connectivity_watch;
  grpc_core::ConnectivityStateWatcherInterface* stop_connectivity_watch =
      nullptr;
  /// should the transport be disconnected
  /// Error contract: the transport that gets this op must cause
  ///                disconnect_with_error to be unref'ed after processing it
  grpc_error_handle disconnect_with_error;
  /// what should the goaway contain?
  /// Error contract: the transport that gets this op must cause
  ///                goaway_error to be unref'ed after processing it
  grpc_error_handle goaway_error;
  void (*set_accept_stream_fn)(void* user_data, grpc_core::Transport* transport,
                               const void* server_data) = nullptr;
  void (*set_registered_method_matcher_fn)(
      void* user_data, grpc_core::ServerMetadata* metadata) = nullptr;
  void* set_accept_stream_user_data = nullptr;
  void (*set_make_promise_fn)(void* user_data, grpc_core::Transport* transport,
                              const void* server_data) = nullptr;
  void* set_make_promise_user_data = nullptr;
  /// add this transport to a pollset
  grpc_pollset* bind_pollset = nullptr;
  /// add this transport to a pollset_set
  grpc_pollset_set* bind_pollset_set = nullptr;
  /// send a ping, if either on_initiate or on_ack is not NULL
  struct {
    /// Ping may be delayed by the transport, on_initiate callback will be
    /// called when the ping is actually being sent.
    grpc_closure* on_initiate = nullptr;
    /// Called when the ping ack is received
    grpc_closure* on_ack = nullptr;
  } send_ping;
  grpc_connectivity_state start_connectivity_watch_state = GRPC_CHANNEL_IDLE;
  // If true, will reset the channel's connection backoff.
  bool reset_connect_backoff = false;

  /// set the callback for accepting new streams;
  /// this is a permanent callback, unlike the other one-shot closures.
  /// If true, the callback is set to set_accept_stream_fn, with its
  /// user_data argument set to set_accept_stream_user_data.
  /// `set_registered_method_matcher_fn` is also set with its user_data argument
  /// set to set_accept_stream_user_data. The transport should invoke
  /// `set_registered_method_matcher_fn` after initial metadata is received but
  /// before recv_initial_metadata_ready callback is invoked. If the transport
  /// detects an error in the stream, invoking
  /// `set_registered_method_matcher_fn` can be skipped.
  bool set_accept_stream = false;

  /// set the callback for accepting new streams based upon promises;
  /// this is a permanent callback, unlike the other one-shot closures.
  /// If true, the callback is set to set_make_promise_fn, with its
  /// user_data argument set to set_make_promise_data
  bool set_make_promise = false;

  //**************************************************************************
  // remaining fields are initialized and used at the discretion of the
  // transport implementation

  grpc_handler_private_op_data handler_private;
} grpc_transport_op;

void grpc_transport_stream_op_batch_finish_with_failure(
    grpc_transport_stream_op_batch* batch, grpc_error_handle error,
    grpc_core::CallCombiner* call_combiner);
void grpc_transport_stream_op_batch_queue_finish_with_failure(
    grpc_transport_stream_op_batch* batch, grpc_error_handle error,
    grpc_core::CallCombinerClosureList* closures);
// Fail a batch from within the transport (i.e. without the activity lock/call
// combiner taken).
void grpc_transport_stream_op_batch_finish_with_failure_from_transport(
    grpc_transport_stream_op_batch* batch, grpc_error_handle error);

std::string grpc_transport_stream_op_batch_string(
    grpc_transport_stream_op_batch* op, bool truncate);
std::string grpc_transport_op_string(grpc_transport_op* op);

namespace grpc_core {

class FilterStackTransport {
 public:
  // Memory required for a single stream element - this is allocated by upper
  // layers and initialized by the transport
  virtual size_t SizeOfStream() const = 0;

  // Initialize transport data for a stream.
  // Returns 0 on success, any other (transport-defined) value for failure.
  // May assume that stream contains all-zeros.
  // Arguments:
  //   stream      - a pointer to uninitialized memory to initialize
  //   server_data - either NULL for a client initiated stream, or a pointer
  //                 supplied from the accept_stream callback function
  virtual void InitStream(grpc_stream* stream, grpc_stream_refcount* refcount,
                          const void* server_data, Arena* arena) = 0;

  // HACK: inproc does not handle stream op batch callbacks correctly (receive
  // ops are required to complete prior to on_complete triggering).
  // This flag is used to disable coalescing of batches in connected_channel for
  // that specific transport.
  // TODO(ctiller): This ought not be necessary once we have promises complete.
  virtual bool HackyDisableStreamOpBatchCoalescingInConnectedChannel()
      const = 0;

  virtual void PerformStreamOp(grpc_stream* stream,
                               grpc_transport_stream_op_batch* op) = 0;

  // Destroy transport data for a stream.
  // Requires: a recv_batch with final_state == GRPC_STREAM_CLOSED has been
  // received by the up-layer. Must not be called in the same call stack as
  // recv_frame.
  // Arguments:
  //   stream    - the grpc_stream to destroy (memory is still owned by the
  //               caller, but any child memory must be cleaned up)
  virtual void DestroyStream(grpc_stream* stream,
                             grpc_closure* then_schedule_closure) = 0;

 protected:
  ~FilterStackTransport() = default;
};

class ClientTransport {
 public:
  virtual void StartCall(CallHandler call_handler) = 0;

 protected:
  ~ClientTransport() = default;
};

class ServerTransport {
 public:
  // AcceptFunction takes initial metadata for a new call and returns a
  // CallInitiator object for it, for the transport to use to communicate with
  // the CallHandler object passed to the application.
  using AcceptFunction =
      absl::AnyInvocable<absl::StatusOr<CallInitiator>(ClientMetadata&) const>;

  // Called once slightly after transport setup to register the accept function.
  virtual void SetAcceptFunction(AcceptFunction accept_function) = 0;

 protected:
  ~ServerTransport() = default;
};

class Transport : public Orphanable {
 public:
  struct RawPointerChannelArgTag {};
  static absl::string_view ChannelArgName() { return GRPC_ARG_TRANSPORT; }

  virtual FilterStackTransport* filter_stack_transport() = 0;
  virtual ClientTransport* client_transport() = 0;
  virtual ServerTransport* server_transport() = 0;

  // name of this transport implementation
  virtual absl::string_view GetTransportName() const = 0;

  // implementation of grpc_transport_set_pollset
  virtual void SetPollset(grpc_stream* stream, grpc_pollset* pollset) = 0;

  // implementation of grpc_transport_set_pollset
  virtual void SetPollsetSet(grpc_stream* stream,
                             grpc_pollset_set* pollset_set) = 0;

  void SetPollingEntity(grpc_stream* stream,
                        grpc_polling_entity* pollset_or_pollset_set);

  // implementation of grpc_transport_perform_op
  virtual void PerformOp(grpc_transport_op* op) = 0;

  // implementation of grpc_transport_get_endpoint
  virtual grpc_endpoint* GetEndpoint() = 0;
};

}  // namespace grpc_core

// Allocate a grpc_transport_op, and preconfigure the on_complete closure to
// \a on_complete and then delete the returned transport op
grpc_transport_op* grpc_make_transport_op(grpc_closure* on_complete);
// Allocate a grpc_transport_stream_op_batch, and preconfigure the on_complete
// closure
// to \a on_complete and then delete the returned transport op
grpc_transport_stream_op_batch* grpc_make_transport_stream_op(
    grpc_closure* on_complete);

namespace grpc_core {
// This is the key to be used for loading/storing keepalive_throttling in the
// absl::Status object.
constexpr const char* kKeepaliveThrottlingKey =
    "grpc.internal.keepalive_throttling";
}  // namespace grpc_core

#endif  // GRPC_SRC_CORE_LIB_TRANSPORT_TRANSPORT_H
