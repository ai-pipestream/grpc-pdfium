#pragma once

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace grpc_pdfium {

// PdfBackendService implementation. Walking-skeleton stage: the wire
// contract is fully served and spec-conformant, but no PDF engine is linked
// yet, so every document reports LOAD_STATUS_ENGINE_ERROR (or NOT_PDF when
// the bytes lack the PDF magic) and Parse streams end after the header.
class PdfBackendServiceImpl final
    : public ai::pipestream::parse::pdf::v1::PdfBackendService::Service {
 public:
  grpc::Status Probe(
      grpc::ServerContext* context,
      const ai::pipestream::parse::pdf::v1::ProbeRequest* request,
      ai::pipestream::parse::pdf::v1::ProbeResponse* response) override;

  grpc::Status Parse(
      grpc::ServerContext* context,
      const ai::pipestream::parse::pdf::v1::ParseRequest* request,
      grpc::ServerWriter<ai::pipestream::parse::pdf::v1::ParseResponse>*
          writer) override;

  grpc::Status Render(
      grpc::ServerContext* context,
      const ai::pipestream::parse::pdf::v1::RenderRequest* request,
      grpc::ServerWriter<ai::pipestream::parse::pdf::v1::RenderResponse>*
          writer) override;
};

}  // namespace grpc_pdfium
