#include "proxy_service_impl.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "pdfium_engine.h"
#include "request_checks.h"
#include "service_info.h"
#include "sha256.h"

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

// A worker failing with one of these did not produce a typed answer: the
// process crashed, was killed, or never came up. Anything else is the
// worker's own verdict and passes through.
bool IsWorkerFailure(const grpc::Status& status) {
  return status.error_code() == grpc::StatusCode::UNAVAILABLE ||
         status.error_code() == grpc::StatusCode::UNKNOWN ||
         status.error_code() == grpc::StatusCode::CANCELLED;
}

enum class ResolveResult { kReady, kBytesRequired, kHashMismatch, kInvalid };

// A request's document after the handshake.
struct Resolved {
  ResolveResult result = ResolveResult::kReady;
  // The verdict's message when result is a verdict.
  std::string detail;
  // The cached bytes of a request addressed by hash; null when the request
  // carries its own bytes.
  std::shared_ptr<const std::string> cached;
};

// The content-addressed handshake of PdfDocument.sha256, resolved once here
// for all three RPCs. Bytes on the wire with a hash are verified (a match is
// cached, a mismatch is a typed verdict); an empty-data request is a cache
// lookup, and a hit hands back the cached bytes for the worker call, so
// workers never learn that the cache exists.
Resolved ResolveDocument(const pdfv1::PdfDocument& document, ByteCache* cache) {
  Resolved resolved;
  if (!document.data().empty()) {
    if (!document.has_sha256()) return resolved;
    const std::string actual = Sha256Hex(document.data());
    if (actual != document.sha256()) {
      resolved.result = ResolveResult::kHashMismatch;
      resolved.detail =
          "data does not hash to the supplied sha256 " + document.sha256();
      return resolved;
    }
    cache->Put(actual, std::make_shared<const std::string>(document.data()));
    return resolved;
  }
  if (!document.has_sha256()) {
    resolved.result = ResolveResult::kInvalid;
    return resolved;
  }
  resolved.cached = cache->Get(document.sha256());
  if (resolved.cached == nullptr) {
    resolved.result = ResolveResult::kBytesRequired;
    resolved.detail = "no cached bytes for sha256 " + document.sha256();
  }
  return resolved;
}

// The request a worker receives. A request that carries its bytes goes
// through as it came, with no copy. A request addressed by hash gets the
// cached bytes filled into a copy, made only once a worker is leased, so a
// request still waiting for a worker holds no copy of the document.
template <typename Request>
class WorkerRequest {
 public:
  WorkerRequest(const Request& client, std::shared_ptr<const std::string> cached)
      : client_(client), cached_(std::move(cached)) {}

  const Request& Get() {
    if (cached_ == nullptr) return client_;
    if (!filled_.has_value()) {
      filled_.emplace(client_);
      filled_->mutable_document()->set_data(*cached_);
    }
    return *filled_;
  }

 private:
  const Request& client_;
  std::shared_ptr<const std::string> cached_;
  std::optional<Request> filled_;
};

// A handshake verdict that is not kReady becomes a typed load failure. On
// Probe and Parse the surface is BackendCapabilities, on Render the
// RenderResponse head.
void FillVerdictCapabilities(pdfv1::BackendCapabilities* caps,
                             pdfv1::LoadStatus status,
                             const std::string& detail) {
  caps->set_backend_name(PdfiumEngine::BackendName());
  caps->set_engine_version(PdfiumEngine::EngineVersion());
  caps->set_load_status(status);
  caps->set_load_detail(detail);
}

// One forwarded call's view of the pool: the role of worker it needs, the
// client call it serves, and how long it may wait for a free worker.
struct Forwarding {
  WorkerPool* pool;
  WorkerRole role;
  grpc::ServerContext* context;
  std::chrono::system_clock::time_point queue_deadline;
};

// A request waits for a free worker as long as its client does, and no
// longer than queue_limit when one is set.
Forwarding StartForwarding(WorkerPool* pool, WorkerRole role,
                           grpc::ServerContext* context,
                           std::chrono::seconds queue_limit) {
  std::chrono::system_clock::time_point deadline = context->deadline();
  if (queue_limit.count() > 0) {
    deadline = std::min(deadline, std::chrono::system_clock::now() + queue_limit);
  }
  return Forwarding{pool, role, context, deadline};
}

grpc::Status AcquireWorker(const Forwarding& fwd, WorkerPool::Lease* lease) {
  switch (fwd.pool->Acquire(fwd.role, fwd.queue_deadline, fwd.context, lease)) {
    case WorkerPool::AcquireResult::kLeased:
      return grpc::Status::OK;
    case WorkerPool::AcquireResult::kCancelled:
      return grpc::Status::CANCELLED;
    case WorkerPool::AcquireResult::kTimedOut:
      break;
  }
  return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                      "all " + std::to_string(fwd.pool->size(fwd.role)) + " " +
                          WorkerRoleName(fwd.role) +
                          " workers stayed busy for the whole wait");
}

// The worker call inherits the client's deadline and cancellation, so a
// client that gives up stops its worker call too.
std::unique_ptr<grpc::ClientContext> WorkerContext(const Forwarding& fwd) {
  return grpc::ClientContext::FromServerContext(*fwd.context);
}

// What one attempt on a worker comes to once the lease is released.
struct Settled {
  grpc::Status status;
  // The worker failed before producing anything: the call may go to a
  // fresh worker.
  bool retry = false;
};

// Releases the lease of one finished attempt and decides the answer. A call
// the client abandoned (it cancelled, its deadline passed, or it stopped
// reading) may have left its worker inside the engine, so a worker call
// that did not finish OK kills its worker: the slot frees now, not whenever
// the engine returns. A lease the watchdog cut is never retried, since the
// same request would stall again.
Settled SettleAttempt(const Forwarding& fwd, const WorkerPool::Lease& lease,
                      const grpc::Status& status, bool client_gone,
                      bool forwarded_any) {
  const bool abandoned =
      client_gone || fwd.context->IsCancelled() ||
      status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED;
  const bool failed = abandoned ? !status.ok() : IsWorkerFailure(status);
  const bool cut = fwd.pool->Release(lease, failed);
  if (abandoned) {
    return {client_gone || status.ok() ? grpc::Status::CANCELLED : status};
  }
  if (cut) {
    return {grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                         "the worker made no progress for " +
                             std::to_string(fwd.pool->stall_limit().count()) +
                             " s and was stopped")};
  }
  if (!failed) return {status};
  if (forwarded_any) {
    return {grpc::Status(grpc::StatusCode::UNAVAILABLE,
                         "worker failed mid-stream: " + status.error_message())};
  }
  return {grpc::Status(grpc::StatusCode::UNAVAILABLE,
                       "worker failed twice before producing a response"),
          true};
}

// Forwards one server-streaming RPC through a leased worker. Retries once
// on a fresh worker when the first attempt fails before any message was
// forwarded.
template <typename Request, typename Response, typename StartFn>
grpc::Status ForwardStreaming(const Forwarding& fwd,
                              WorkerRequest<Request>* request,
                              grpc::ServerWriter<Response>* writer,
                              const StartFn& start) {
  Settled settled;
  for (int attempt = 0; attempt < 2; ++attempt) {
    WorkerPool::Lease lease;
    grpc::Status waited = AcquireWorker(fwd, &lease);
    if (!waited.ok()) return waited;
    std::unique_ptr<grpc::ClientContext> worker_ctx = WorkerContext(fwd);
    auto reader = start(lease.stub, worker_ctx.get(), request->Get());
    Response message;
    bool forwarded_any = false;
    bool client_gone = false;
    while (reader->Read(&message)) {
      if (!writer->Write(message)) {
        client_gone = true;
        worker_ctx->TryCancel();
        break;
      }
      forwarded_any = true;
      fwd.pool->Touch(lease);
    }
    const grpc::Status status = reader->Finish();
    settled = SettleAttempt(fwd, lease, status, client_gone, forwarded_any);
    if (!settled.retry) break;
  }
  return settled.status;
}

}  // namespace

grpc::Status ProxyServiceImpl::Probe(grpc::ServerContext* context,
                                     const pdfv1::ProbeRequest* request,
                                     pdfv1::ProbeResponse* response) {
  Resolved resolved = ResolveDocument(request->document(), cache_);
  switch (resolved.result) {
    case ResolveResult::kInvalid:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "data is empty and sha256 is absent");
    case ResolveResult::kBytesRequired:
      FillVerdictCapabilities(response->mutable_capabilities(),
                              pdfv1::LOAD_STATUS_BYTES_REQUIRED,
                              resolved.detail);
      return grpc::Status::OK;
    case ResolveResult::kHashMismatch:
      FillVerdictCapabilities(response->mutable_capabilities(),
                              pdfv1::LOAD_STATUS_HASH_MISMATCH,
                              resolved.detail);
      return grpc::Status::OK;
    case ResolveResult::kReady:
      break;
  }
  WorkerRequest<pdfv1::ProbeRequest> forwarded(*request,
                                               std::move(resolved.cached));
  const Forwarding fwd =
      StartForwarding(pool_, WorkerRole::kText, context, queue_limit_);
  Settled settled;
  for (int attempt = 0; attempt < 2; ++attempt) {
    WorkerPool::Lease lease;
    grpc::Status waited = AcquireWorker(fwd, &lease);
    if (!waited.ok()) return waited;
    std::unique_ptr<grpc::ClientContext> worker_ctx = WorkerContext(fwd);
    response->Clear();
    const grpc::Status status =
        lease.stub->Probe(worker_ctx.get(), forwarded.Get(), response);
    settled = SettleAttempt(fwd, lease, status, false, false);
    if (!settled.retry) break;
  }
  return settled.status;
}

grpc::Status ProxyServiceImpl::Parse(grpc::ServerContext* context,
                                     const pdfv1::ParseRequest* request,
                                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  // A malformed request fails here, before it resolves bytes or leases a
  // worker.
  grpc::Status checked = CheckParseRequest(*request);
  if (!checked.ok()) return checked;
  Resolved resolved = ResolveDocument(request->document(), cache_);
  if (resolved.result == ResolveResult::kInvalid) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "data is empty and sha256 is absent");
  }
  if (resolved.result != ResolveResult::kReady) {
    // Load-failure discipline: the header carries the verdict and the
    // stream ends after it.
    pdfv1::ParseResponse head;
    FillVerdictCapabilities(head.mutable_header()->mutable_capabilities(),
                            resolved.result == ResolveResult::kBytesRequired
                                ? pdfv1::LOAD_STATUS_BYTES_REQUIRED
                                : pdfv1::LOAD_STATUS_HASH_MISMATCH,
                            resolved.detail);
    if (!writer->Write(head)) return grpc::Status::CANCELLED;
    return grpc::Status::OK;
  }
  WorkerRequest<pdfv1::ParseRequest> forwarded(*request,
                                               std::move(resolved.cached));
  // Text work never shares a process with rendering (worker_role.h).
  return ForwardStreaming(
      StartForwarding(pool_, WorkerRole::kText, context, queue_limit_),
      &forwarded, writer,
      [](pdfv1::PdfBackendService::Stub* stub, grpc::ClientContext* ctx,
         const pdfv1::ParseRequest& req) { return stub->Parse(ctx, req); });
}

grpc::Status ProxyServiceImpl::Render(grpc::ServerContext* context,
                                      const pdfv1::RenderRequest* request,
                                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  grpc::Status checked = CheckRenderRequest(*request);
  if (!checked.ok()) return checked;
  Resolved resolved = ResolveDocument(request->document(), cache_);
  if (resolved.result == ResolveResult::kInvalid) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "data is empty and sha256 is absent");
  }
  if (resolved.result != ResolveResult::kReady) {
    // Load-failure discipline: exactly one message with the head set, then
    // the stream ends.
    pdfv1::RenderResponse head;
    auto* verdict = head.mutable_head();
    verdict->set_load_status(resolved.result == ResolveResult::kBytesRequired
                                 ? pdfv1::LOAD_STATUS_BYTES_REQUIRED
                                 : pdfv1::LOAD_STATUS_HASH_MISMATCH);
    verdict->set_load_detail(resolved.detail);
    if (!writer->Write(head)) return grpc::Status::CANCELLED;
    return grpc::Status::OK;
  }
  WorkerRequest<pdfv1::RenderRequest> forwarded(*request,
                                                std::move(resolved.cached));
  return ForwardStreaming(
      StartForwarding(pool_, WorkerRole::kRender, context, queue_limit_),
      &forwarded, writer,
      [](pdfv1::PdfBackendService::Stub* stub, grpc::ClientContext* ctx,
         const pdfv1::RenderRequest& req) { return stub->Render(ctx, req); });
}

grpc::Status ProxyServiceImpl::GetServiceInfo(
    grpc::ServerContext* /*context*/,
    const pdfv1::ServiceInfoRequest* /*request*/,
    pdfv1::ServiceInfoResponse* response) {
  FillServiceInfo(response);
  return grpc::Status::OK;
}

}  // namespace grpc_pdfium
