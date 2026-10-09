#pragma once

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "worker_role.h"

namespace grpc_pdfium {

// The engine-backed PdfBackendService implementation, served by each worker
// process. Engine calls are serialized behind one process-wide mutex because
// PDFium is process-global and not thread-safe; concurrency comes from the
// worker-process pool in front, never from threads inside one process.
//
// A worker started for one role refuses the other role's RPCs with
// FAILED_PRECONDITION (see worker_role.h): the front never sends them, and
// the refusal keeps a text worker from ever rendering should that change.
class PdfBackendServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
  explicit PdfBackendServiceImpl(WorkerRole role = WorkerRole::kAny)
      : role_(role) {}

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

  grpc::Status GetServiceInfo(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ServiceInfoRequest* request,
      ai::protomolt::parse::pdf::v1::ServiceInfoResponse* response) override;

 private:
  const WorkerRole role_;
};

}  // namespace grpc_pdfium
