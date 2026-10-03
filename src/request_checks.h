#pragma once

#include <climits>
#include <cmath>
#include <cstdint>
#include <string>

#include <grpcpp/support/status.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"

namespace grpc_pdfium {

// Argument checks the front and the workers both apply: the front rejects a
// malformed request before it costs a worker, and a worker dialed directly
// is just as safe.

// The highest Render DPI accepted. Far above any page model's input; the
// raster byte ceiling in the engine is what bounds memory, this keeps the
// pixel arithmetic in range.
inline constexpr double kMaxRenderDpi = 2400.0;

// A set PageRange must have end greater than begin (the contract's rule),
// and no document has more pages than an int can count (PDFium indexes
// pages with int), so a bound above INT_MAX is rejected rather than ever
// becoming a negative page index.
inline grpc::Status CheckPageRange(
    const ai::protomolt::parse::pdf::v1::PageRange& range) {
  if (range.end() <= range.begin()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "pages.end must be greater than pages.begin (got " +
                            std::to_string(range.begin()) + ", " +
                            std::to_string(range.end()) + ")");
  }
  if (range.end() > static_cast<uint32_t>(INT_MAX)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "pages.end " + std::to_string(range.end()) +
                            " is beyond any page index");
  }
  return grpc::Status::OK;
}

inline grpc::Status CheckParseRequest(
    const ai::protomolt::parse::pdf::v1::ParseRequest& request) {
  return request.has_pages() ? CheckPageRange(request.pages())
                             : grpc::Status::OK;
}

inline grpc::Status CheckRenderRequest(
    const ai::protomolt::parse::pdf::v1::RenderRequest& request) {
  if (!std::isfinite(request.dpi()) || request.dpi() <= 0.0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dpi must be positive");
  }
  if (request.dpi() > kMaxRenderDpi) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dpi " + std::to_string(request.dpi()) +
                            " is above the limit of " +
                            std::to_string(static_cast<int>(kMaxRenderDpi)));
  }
  return request.has_pages() ? CheckPageRange(request.pages())
                             : grpc::Status::OK;
}

}  // namespace grpc_pdfium
