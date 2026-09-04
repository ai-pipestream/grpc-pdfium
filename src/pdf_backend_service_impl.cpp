#include "pdf_backend_service_impl.h"

#include <mutex>

#include "pdfium_engine.h"
#include "service_info.h"

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

// PDFium is process-global and not thread-safe: one engine call at a time,
// per process. The worker pool gives each process one request at a time
// anyway; this mutex makes the rule hold even when a caller dials a worker
// directly.
std::mutex& EngineMutex() {
  static std::mutex m;
  return m;
}

}  // namespace

grpc::Status PdfBackendServiceImpl::Probe(grpc::ServerContext* /*context*/,
                                          const pdfv1::ProbeRequest* request,
                                          pdfv1::ProbeResponse* response) {
  std::lock_guard<std::mutex> lock(EngineMutex());
  PdfiumEngine::InitProcess();
  PdfiumEngine::Probe(request->document(), response->mutable_capabilities());
  return grpc::Status::OK;
}

grpc::Status PdfBackendServiceImpl::Parse(
    grpc::ServerContext* /*context*/, const pdfv1::ParseRequest* request,
    grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  std::lock_guard<std::mutex> lock(EngineMutex());
  PdfiumEngine::InitProcess();
  PdfiumEngine::Parse(*request, [writer](const pdfv1::ParseResponse& msg) {
    return writer->Write(msg);
  });
  return grpc::Status::OK;
}

grpc::Status PdfBackendServiceImpl::Render(
    grpc::ServerContext* /*context*/, const pdfv1::RenderRequest* request,
    grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  std::lock_guard<std::mutex> lock(EngineMutex());
  PdfiumEngine::InitProcess();
  std::string error;
  bool ok = PdfiumEngine::Render(
      *request,
      [writer](const pdfv1::RenderResponse& msg) { return writer->Write(msg); },
      &error);
  if (!ok && !error.empty()) {
    grpc::StatusCode code = error.rfind("dpi ", 0) == 0
                                ? grpc::StatusCode::INVALID_ARGUMENT
                                : grpc::StatusCode::FAILED_PRECONDITION;
    return grpc::Status(code, error);
  }
  return grpc::Status::OK;
}

grpc::Status PdfBackendServiceImpl::GetServiceInfo(
    grpc::ServerContext* /*context*/,
    const pdfv1::ServiceInfoRequest* /*request*/,
    pdfv1::ServiceInfoResponse* response) {
  FillServiceInfo(response);
  return grpc::Status::OK;
}

}  // namespace grpc_pdfium
