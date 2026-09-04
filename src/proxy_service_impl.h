#pragma once

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "worker_pool.h"

namespace grpc_pdfium {

// The front-door PdfBackendService: forwards every RPC to a leased worker
// process over its unix socket and streams the responses through. A worker
// failure before any response reached the client is retried once on a fresh
// worker; a failure mid-stream surfaces to the client as UNAVAILABLE.
class ProxyServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
  explicit ProxyServiceImpl(WorkerPool* pool) : pool_(pool) {}

  grpc::Status Probe(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ProbeRequest* request,
      ai::protomolt::parse::pdf::v1::ProbeResponse* response) override;

  grpc::Status Parse(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ParseRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::ParseResponse>*
          writer) override;

  grpc::Status Render(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::RenderRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::RenderResponse>*
          writer) override;

 private:
  WorkerPool* pool_;
};

}  // namespace grpc_pdfium
