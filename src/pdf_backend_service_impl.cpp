#include "pdf_backend_service_impl.h"

#include <string_view>

namespace grpc_pdfium {

namespace pdfv1 = ai::pipestream::parse::pdf::v1;

namespace {

constexpr std::string_view kBackendName = "grpc-pdfium";
constexpr std::string_view kEngineVersion = "pdfium (not linked yet)";
constexpr std::string_view kSkeletonDetail =
    "walking skeleton: the pdfium engine is not linked yet";

// Fills capabilities for the skeleton stage: identity, a typed load status,
// and no family verdicts (the contract keeps the family list empty when the
// document did not load).
void FillSkeletonCapabilities(const pdfv1::PdfDocument& document,
                              pdfv1::BackendCapabilities* caps) {
  caps->set_backend_name(std::string(kBackendName));
  caps->set_engine_version(std::string(kEngineVersion));
  const std::string& data = document.data();
  if (data.rfind("%PDF-", 0) != 0) {
    caps->set_load_status(pdfv1::LOAD_STATUS_NOT_PDF);
  } else {
    caps->set_load_status(pdfv1::LOAD_STATUS_ENGINE_ERROR);
    caps->set_load_detail(std::string(kSkeletonDetail));
  }
}

}  // namespace

grpc::Status PdfBackendServiceImpl::Probe(grpc::ServerContext* /*context*/,
                                          const pdfv1::ProbeRequest* request,
                                          pdfv1::ProbeResponse* response) {
  FillSkeletonCapabilities(request->document(), response->mutable_capabilities());
  return grpc::Status::OK;
}

grpc::Status PdfBackendServiceImpl::Parse(
    grpc::ServerContext* /*context*/, const pdfv1::ParseRequest* request,
    grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  pdfv1::ParseResponse header_msg;
  FillSkeletonCapabilities(request->document(),
                           header_msg.mutable_header()->mutable_capabilities());
  writer->Write(header_msg);
  // Load status is never OK at this stage, so the stream ends after the
  // header, exactly as the contract specifies for failed loads.
  return grpc::Status::OK;
}

grpc::Status PdfBackendServiceImpl::Render(
    grpc::ServerContext* /*context*/, const pdfv1::RenderRequest* /*request*/,
    grpc::ServerWriter<pdfv1::RenderResponse>* /*writer*/) {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                      std::string(kSkeletonDetail));
}

}  // namespace grpc_pdfium
