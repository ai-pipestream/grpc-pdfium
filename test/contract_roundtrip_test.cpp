// The M0 walking-skeleton gate: start the real service on a local port,
// dial it through the generated client stubs, and check that Probe and
// Parse round-trip typed data from the released contract.

#include <cstdio>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "pdf_backend_service_impl.h"

namespace pdfv1 = ai::pipestream::parse::pdf::v1;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  grpc_pdfium::PdfBackendServiceImpl service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  Check(server != nullptr, "server started");
  Check(port != 0, "server bound a port");

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = pdfv1::PdfBackendService::NewStub(channel);

  // Probe with PDF magic: identity plus the typed skeleton load status.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data("%PDF-1.7\n%skeleton");
    pdfv1::ProbeResponse response;
    grpc::Status status = stub->Probe(&ctx, request, &response);
    Check(status.ok(), "Probe RPC succeeded");
    Check(response.capabilities().backend_name() == "grpc-pdfium",
          "Probe reports the backend name");
    Check(response.capabilities().load_status() ==
              pdfv1::LOAD_STATUS_ENGINE_ERROR,
          "skeleton reports LOAD_STATUS_ENGINE_ERROR for PDF bytes");
    Check(!response.capabilities().engine_version().empty(),
          "Probe reports an engine version string");
  }

  // Probe without PDF magic: typed NOT_PDF.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data("not a pdf");
    pdfv1::ProbeResponse response;
    grpc::Status status = stub->Probe(&ctx, request, &response);
    Check(status.ok(), "Probe RPC succeeded for non-PDF bytes");
    Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_NOT_PDF,
          "non-PDF bytes report LOAD_STATUS_NOT_PDF");
  }

  // Parse: header arrives first and the stream ends after it on a failed
  // load, per the contract.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data("%PDF-1.7\n%skeleton");
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse message;
    Check(reader->Read(&message), "Parse stream produced a message");
    Check(message.has_header(), "first Parse message is the header");
    Check(message.header().capabilities().backend_name() == "grpc-pdfium",
          "Parse header echoes capabilities");
    Check(!reader->Read(&message), "Parse stream ended after the header");
    Check(reader->Finish().ok(), "Parse stream finished OK");
  }

  // Render: typed gRPC error until the engine is linked.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data("%PDF-1.7\n%skeleton");
    request.set_dpi(72.0);
    request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse message;
    Check(!reader->Read(&message), "Render stream is empty in the skeleton");
    Check(reader->Finish().error_code() == grpc::FAILED_PRECONDITION,
          "Render fails with FAILED_PRECONDITION in the skeleton");
  }

  server->Shutdown();
  if (failures == 0) {
    std::printf("contract_roundtrip: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "contract_roundtrip: %d check(s) failed\n", failures);
  return 1;
}
