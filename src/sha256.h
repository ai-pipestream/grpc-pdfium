#pragma once

#include <string>

namespace grpc_pdfium {

// Lowercase hex SHA-256 of data, the content address of the PdfDocument
// handshake. Implemented on the boringssl the gRPC build already carries.
std::string Sha256Hex(const std::string& data);

}  // namespace grpc_pdfium
