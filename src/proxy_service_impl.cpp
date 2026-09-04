#include "proxy_service_impl.h"

#include <memory>
#include <string>

#include "pdfium_engine.h"
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

// The content-addressed handshake of PdfDocument.sha256, resolved once here
// for all three RPCs. Bytes on the wire with a hash are verified (a match is
// cached, a mismatch is a typed verdict); an empty-data request is a cache
// lookup and a hit fills the bytes in for the worker call, so workers never
// learn that the cache exists.
ResolveResult ResolveDocument(pdfv1::PdfDocument* document, ByteCache* cache,
                              std::string* detail) {
  if (!document->data().empty()) {
    if (!document->has_sha256()) return ResolveResult::kReady;
    const std::string actual = Sha256Hex(document->data());
    if (actual != document->sha256()) {
      *detail = "data does not hash to the supplied sha256 " +
                document->sha256();
      return ResolveResult::kHashMismatch;
    }
    cache->Put(actual,
               std::make_shared<const std::string>(document->data()));
    return ResolveResult::kReady;
  }
  if (!document->has_sha256()) return ResolveResult::kInvalid;
  std::shared_ptr<const std::string> bytes = cache->Get(document->sha256());
  if (bytes == nullptr) {
    *detail = "no cached bytes for sha256 " + document->sha256();
    return ResolveResult::kBytesRequired;
  }
  document->set_data(*bytes);
  return ResolveResult::kReady;
}

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

// Forwards one server-streaming RPC through a leased worker. Retries once
// on a fresh worker when the first attempt fails before any message was
// forwarded.
template <typename Request, typename Response, typename StartFn>
grpc::Status ForwardStreaming(WorkerPool* pool, const Request& request,
                              grpc::ServerWriter<Response>* writer,
                              const StartFn& start) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    WorkerPool::Lease lease = pool->Acquire();
    grpc::ClientContext worker_ctx;
    auto reader = start(lease.stub, &worker_ctx, request);
    Response message;
    bool forwarded_any = false;
    bool client_gone = false;
    while (reader->Read(&message)) {
      if (!writer->Write(message)) {
        client_gone = true;
        worker_ctx.TryCancel();
        break;
      }
      forwarded_any = true;
    }
    grpc::Status status = reader->Finish();
    bool failed = IsWorkerFailure(status) && !client_gone;
    pool->Release(lease, failed);
    if (client_gone) return grpc::Status::CANCELLED;
    if (!failed) return status;
    if (forwarded_any) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "worker failed mid-stream: " + status.error_message());
    }
    // Nothing was forwarded: safe to retry once on a fresh worker.
  }
  return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                      "worker failed twice before producing a response");
}

}  // namespace

grpc::Status ProxyServiceImpl::Probe(grpc::ServerContext* /*context*/,
                                     const pdfv1::ProbeRequest* request,
                                     pdfv1::ProbeResponse* response) {
  pdfv1::ProbeRequest resolved = *request;
  std::string detail;
  switch (ResolveDocument(resolved.mutable_document(), cache_, &detail)) {
    case ResolveResult::kInvalid:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "data is empty and sha256 is absent");
    case ResolveResult::kBytesRequired:
      FillVerdictCapabilities(response->mutable_capabilities(),
                              pdfv1::LOAD_STATUS_BYTES_REQUIRED, detail);
      return grpc::Status::OK;
    case ResolveResult::kHashMismatch:
      FillVerdictCapabilities(response->mutable_capabilities(),
                              pdfv1::LOAD_STATUS_HASH_MISMATCH, detail);
      return grpc::Status::OK;
    case ResolveResult::kReady:
      break;
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    WorkerPool::Lease lease = pool_->Acquire();
    grpc::ClientContext worker_ctx;
    grpc::Status status = lease.stub->Probe(&worker_ctx, resolved, response);
    bool failed = IsWorkerFailure(status);
    pool_->Release(lease, failed);
    if (!failed) return status;
  }
  return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                      "worker failed twice before producing a response");
}

grpc::Status ProxyServiceImpl::Parse(grpc::ServerContext* /*context*/,
                                     const pdfv1::ParseRequest* request,
                                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  pdfv1::ParseRequest resolved = *request;
  std::string detail;
  ResolveResult result =
      ResolveDocument(resolved.mutable_document(), cache_, &detail);
  if (result == ResolveResult::kInvalid) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "data is empty and sha256 is absent");
  }
  if (result != ResolveResult::kReady) {
    // Load-failure discipline: the header carries the verdict and the
    // stream ends after it.
    pdfv1::ParseResponse head;
    FillVerdictCapabilities(head.mutable_header()->mutable_capabilities(),
                            result == ResolveResult::kBytesRequired
                                ? pdfv1::LOAD_STATUS_BYTES_REQUIRED
                                : pdfv1::LOAD_STATUS_HASH_MISMATCH,
                            detail);
    if (!writer->Write(head)) return grpc::Status::CANCELLED;
    return grpc::Status::OK;
  }
  return ForwardStreaming(
      pool_, resolved, writer,
      [](pdfv1::PdfBackendService::Stub* stub, grpc::ClientContext* ctx,
         const pdfv1::ParseRequest& req) { return stub->Parse(ctx, req); });
}

grpc::Status ProxyServiceImpl::Render(grpc::ServerContext* /*context*/,
                                      const pdfv1::RenderRequest* request,
                                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  pdfv1::RenderRequest resolved = *request;
  std::string detail;
  ResolveResult result =
      ResolveDocument(resolved.mutable_document(), cache_, &detail);
  if (result == ResolveResult::kInvalid) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "data is empty and sha256 is absent");
  }
  if (result != ResolveResult::kReady) {
    // Load-failure discipline: exactly one message with the head set, then
    // the stream ends.
    pdfv1::RenderResponse head;
    auto* verdict = head.mutable_head();
    verdict->set_load_status(result == ResolveResult::kBytesRequired
                                 ? pdfv1::LOAD_STATUS_BYTES_REQUIRED
                                 : pdfv1::LOAD_STATUS_HASH_MISMATCH);
    verdict->set_load_detail(detail);
    if (!writer->Write(head)) return grpc::Status::CANCELLED;
    return grpc::Status::OK;
  }
  return ForwardStreaming(
      pool_, resolved, writer,
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
