#pragma once

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
// The front also owns the content-addressed document handshake
// (PdfDocument.sha256): it verifies hashes, answers cache hits and misses,
// and keeps the byte cache. Workers always receive full bytes and stay
// stateless, so a respawned worker never loses cached content.
class ProxyServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
  ProxyServiceImpl(WorkerPool* pool, ByteCache* cache)
      : pool_(pool), cache_(cache) {}

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
};

}  // namespace grpc_pdfium
