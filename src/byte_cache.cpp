#include "byte_cache.h"

namespace grpc_pdfium {

ByteCache::ByteCache(size_t max_documents, size_t max_bytes)
    : max_documents_(max_documents), max_bytes_(max_bytes) {}

std::shared_ptr<const std::string> ByteCache::Get(
    const std::string& sha256_hex) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(sha256_hex);
  if (it == entries_.end()) return nullptr;
  lru_.splice(lru_.begin(), lru_, it->second.lru_it);
  return it->second.data;
}

void ByteCache::Put(const std::string& sha256_hex,
                    std::shared_ptr<const std::string> data) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto existing = entries_.find(sha256_hex);
  if (existing != entries_.end()) {
    lru_.splice(lru_.begin(), lru_, existing->second.lru_it);
    return;
  }
  const size_t size = data->size();
  if (max_documents_ == 0 || size > max_bytes_) return;
  while (!lru_.empty() && (entries_.size() >= max_documents_ ||
                           total_bytes_ + size > max_bytes_)) {
    const std::string& victim = lru_.back();
    total_bytes_ -= entries_.at(victim).data->size();
    entries_.erase(victim);
    lru_.pop_back();
  }
  lru_.push_front(sha256_hex);
  entries_.emplace(sha256_hex, Entry{std::move(data), lru_.begin()});
  total_bytes_ += size;
}

size_t ByteCache::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

}  // namespace grpc_pdfium
