#include "sha256.h"

#include <cstdint>

#include <openssl/sha.h>

namespace grpc_pdfium {

std::string Sha256Hex(const std::string& data) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(SHA256_DIGEST_LENGTH * 2);
  for (uint8_t byte : digest) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

}  // namespace grpc_pdfium
