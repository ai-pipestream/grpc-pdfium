#pragma once

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace grpc_pdfium {

// The engine-backed PdfBackendService implementation, served by each worker
// process. Engine calls are serialized behind one process-wide mutex because
// PDFium is process-global and not thread-safe; concurrency comes from the
// worker-process pool in front, never from threads inside one process.
class PdfBackendServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
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
};

}  // namespace grpc_pdfium
