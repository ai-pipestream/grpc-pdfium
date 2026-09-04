#include "proxy_service_impl.h"

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
  for (int attempt = 0; attempt < 2; ++attempt) {
    WorkerPool::Lease lease = pool_->Acquire();
    grpc::ClientContext worker_ctx;
    grpc::Status status = lease.stub->Probe(&worker_ctx, *request, response);
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
  return ForwardStreaming(
      pool_, *request, writer,
      [](pdfv1::PdfBackendService::Stub* stub, grpc::ClientContext* ctx,
         const pdfv1::ParseRequest& req) { return stub->Parse(ctx, req); });
}

grpc::Status ProxyServiceImpl::Render(grpc::ServerContext* /*context*/,
                                      const pdfv1::RenderRequest* request,
                                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  return ForwardStreaming(
      pool_, *request, writer,
      [](pdfv1::PdfBackendService::Stub* stub, grpc::ClientContext* ctx,
         const pdfv1::RenderRequest& req) { return stub->Render(ctx, req); });
}

}  // namespace grpc_pdfium
