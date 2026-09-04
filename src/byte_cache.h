#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace grpc_pdfium {

// The server side of the content-addressed document handshake
// (PdfDocument.sha256): document bytes keyed by their lowercase hex SHA-256.
// The cache lives in the front process, which owns the client-facing wire;
// worker processes always receive full bytes over their unix sockets and
// never see the cache.
//
// Bounded two ways, both least-recently-used: a document count and a total
// byte ceiling. Thread safe; the front serves concurrent RPCs.
class ByteCache {
 public:
  // max_documents of 0 disables the cache; max_bytes is the ceiling on the
  // summed size of cached documents.
  ByteCache(size_t max_documents, size_t max_bytes);

  // The cached bytes for a hash, or nullptr on a miss. A hit counts as use
  // for eviction.
  std::shared_ptr<const std::string> Get(const std::string& sha256_hex);

  // Caches data under its (already verified) hash. A document larger than
  // the byte ceiling is never cached; re-putting a known hash only refreshes
  // its eviction position.
  void Put(const std::string& sha256_hex,
           std::shared_ptr<const std::string> data);

  // Number of cached documents. Tests and logging only.
  size_t Size() const;

 private:
  using LruList = std::list<std::string>;  // front is most recently used
  struct Entry {
    std::shared_ptr<const std::string> data;
    LruList::iterator lru_it;
  };

  mutable std::mutex mutex_;
  size_t max_documents_;
  size_t max_bytes_;
  size_t total_bytes_ = 0;
  LruList lru_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace grpc_pdfium
