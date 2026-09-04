#pragma once

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "pdfium_engine.h"

// The build version GetServiceInfo reports: Docker builds pass the image tag
// through the CMake cache, local builds fall back to git describe, then "dev"
// (see CMakeLists.txt). The define fallback here keeps the tests buildable
// from any target that forgot the flag.
#ifndef GRPC_PDFIUM_BUILD_VERSION
#define GRPC_PDFIUM_BUILD_VERSION "dev"
#endif

namespace grpc_pdfium {

// The document-independent identity GetServiceInfo reports. backend_name and
// engine_version are the exact strings BackendCapabilities carries for every
// document. The UiInfo block follows the family convention so the demo shell
// can mount a tab; there is no web UI yet and the description says so.
inline void FillServiceInfo(
    ai::protomolt::parse::pdf::v1::ServiceInfoResponse* response) {
  response->set_backend_name(PdfiumEngine::BackendName());
  response->set_engine_version(PdfiumEngine::EngineVersion());
  response->set_build_version(GRPC_PDFIUM_BUILD_VERSION);
  auto* ui = response->mutable_ui();
  ui->set_title("PDFium");
  ui->set_path("/ui/pdfium");
  ui->set_description("PdfBackendService over PDFium; no web UI yet");
}

}  // namespace grpc_pdfium
