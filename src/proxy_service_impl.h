#pragma once

#include <chrono>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "byte_cache.h"
#include "worker_pool.h"

namespace grpc_pdfium {

// The front-door PdfBackendService: forwards every RPC to a leased worker
// process over its unix socket and streams the responses through. A worker
// failure before any response reached the client is retried once on a fresh
// worker; a failure mid-stream surfaces to the client as UNAVAILABLE.
//
// Worker calls inherit the client's deadline and cancellation. A request
// waits for a free worker no longer than its client does, nor than
// queue_limit when that is set (then RESOURCE_EXHAUSTED); the pool's
// watchdog ends a worker call that stops making progress
// (DEADLINE_EXCEEDED), or cancels the client's call when the front is stuck
// writing to a client that stopped reading (the client sees CANCELLED).
//
// The front also owns the content-addressed document handshake
// (PdfDocument.sha256): it verifies hashes, answers cache hits and misses,
// and keeps the byte cache. Workers always receive full bytes and stay
// stateless, so a respawned worker never loses cached content.
class ProxyServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
  ProxyServiceImpl(WorkerPool* pool, ByteCache* cache,
                   std::chrono::seconds queue_limit)
      : pool_(pool), cache_(cache), queue_limit_(queue_limit) {}

  grpc::Status Probe(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ProbeRequest* request,
      ai::protomolt::parse::pdf::v1::ProbeResponse* response) override;

  grpc::Status Parse(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ParseRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::ParseResponse>*
          writer) override;

  grpc::Status Render(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::RenderRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::RenderResponse>*
          writer) override;

  // Answered by the front itself: the identity is the same for front and
  // workers, so no worker round-trip is needed (and no worker is leased).
  grpc::Status GetServiceInfo(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ServiceInfoRequest* request,
      ai::protomolt::parse::pdf::v1::ServiceInfoResponse* response) override;

 private:
  WorkerPool* pool_;
  ByteCache* cache_;
  std::chrono::seconds queue_limit_;
};

}  // namespace grpc_pdfium
