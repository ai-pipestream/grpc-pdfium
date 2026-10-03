#include "utf8.h"

#include <cstddef>

namespace grpc_pdfium {

namespace {

// Length of the well-formed UTF-8 sequence at s[i] (the Unicode standard's
// table 3-7: no overlong forms, no surrogates, nothing above U+10FFFF), or
// 0 when the bytes there do not form one.
size_t SequenceLength(std::string_view s, size_t i) {
  const auto byte = [&s](size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char lead = byte(i);
  if (lead < 0x80) return 1;
  size_t length = 0;
  unsigned char low = 0x80;
  unsigned char high = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead == 0xE0) {
    length = 3;
    low = 0xA0;
  } else if (lead == 0xED) {
    length = 3;
    high = 0x9F;
  } else if (lead >= 0xE1 && lead <= 0xEF) {
    length = 3;
  } else if (lead == 0xF0) {
    length = 4;
    low = 0x90;
  } else if (lead >= 0xF1 && lead <= 0xF3) {
    length = 4;
  } else if (lead == 0xF4) {
    length = 4;
    high = 0x8F;
  } else {
    return 0;
  }
  if (i + length > s.size()) return 0;
  if (byte(i + 1) < low || byte(i + 1) > high) return 0;
  for (size_t k = 2; k < length; ++k) {
    if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) return 0;
  }
  return length;
}

}  // namespace

std::string ValidUtf8(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (size_t i = 0; i < bytes.size();) {
    if (const size_t length = SequenceLength(bytes, i); length > 0) {
      out.append(bytes.substr(i, length));
      i += length;
      continue;
    }
    const auto latin1 = static_cast<unsigned char>(bytes[i++]);
    out.push_back(static_cast<char>(0xC0 | (latin1 >> 6)));
    out.push_back(static_cast<char>(0x80 | (latin1 & 0x3F)));
  }
  return out;
}

}  // namespace grpc_pdfium
