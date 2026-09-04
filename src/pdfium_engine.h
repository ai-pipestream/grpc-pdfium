#pragma once

#include <functional>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"

namespace grpc_pdfium {

// The PDFium-backed tier 0 engine. PDFium keeps process-global state and is
// not thread-safe, so every function here must be called from one thread at
// a time; the worker process enforces that with a mutex around each RPC.
// The gRPC front never calls into this directly, it dials worker processes.
class PdfiumEngine {
 public:
  // Initializes the PDFium library once per process. Call before anything
  // else; safe to call again.
  static void InitProcess();

  // The backend identity string reported in BackendCapabilities.
  static const char* BackendName();

  // The engine identity string reported in BackendCapabilities.
  static const char* EngineVersion();

  // Loads the document and fills the per-document capability verdicts.
  static void Probe(const ai::protomolt::parse::pdf::v1::PdfDocument& document,
                    ai::protomolt::parse::pdf::v1::BackendCapabilities* caps);

  // Parses the tier 0 families, invoking emit for each stream message
  // (header first, then one PageChunk per page, then the trailer). Returns
  // false only when emit returned false (client gone).
  static bool Parse(
      const ai::protomolt::parse::pdf::v1::ParseRequest& request,
      const std::function<
          bool(const ai::protomolt::parse::pdf::v1::ParseResponse&)>& emit);

  // Renders the requested pages, invoking emit per raster. Fills a typed
  // gRPC-style error into *error_message and returns false when the
  // document cannot be rendered.
  static bool Render(
      const ai::protomolt::parse::pdf::v1::RenderRequest& request,
      const std::function<
          bool(const ai::protomolt::parse::pdf::v1::RenderResponse&)>& emit,
      std::string* error_message);
};

}  // namespace grpc_pdfium
