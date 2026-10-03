#pragma once

#include <string>
#include <string_view>

namespace grpc_pdfium {

// Bytes as valid UTF-8, for the contract's proto3 string fields. Protobuf
// rejects a message whose string field is not UTF-8, so one font name in a
// legacy encoding (a GBK or Shift-JIS /BaseFont, a font program's family
// name) would otherwise cost the client the whole Parse stream. Well-formed
// UTF-8 passes through unchanged; each byte of an ill-formed sequence is
// read as Latin-1, since PDF names and legacy byte strings are most often
// in a single-byte encoding. The same rule as grpc-poppler's, so both
// backends report the same name for the same bytes.
std::string ValidUtf8(std::string_view bytes);

}  // namespace grpc_pdfium
