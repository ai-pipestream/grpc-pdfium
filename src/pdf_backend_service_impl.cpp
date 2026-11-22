#include "pdf_backend_service_impl.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "parse_stream_session.h"
#include "pdfium_engine.h"
#include "request_checks.h"
#include "service_info.h"
#include "sha256.h"

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

// PDFium is process-global and not thread-safe: one engine call at a time,
// per process. The worker pool gives each process one request at a time
// anyway; this mutex makes the rule hold even when a caller dials a worker
// directly.
std::mutex& EngineMutex() {
  static std::mutex m;
  return m;
}

// The largest upload a kAny service's ParseStream accepts: the size the
// unary RPCs accept in one message.
constexpr uint64_t kMaxWorkerStreamBytes = 512ull * 1024 * 1024;

grpc::Status WrongRole(WorkerRole role, const char* rpc) {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                      std::string("this is a ") + WorkerRoleName(role) +
                          " worker and does not serve " + rpc);
}

}  // namespace

grpc::Status PdfBackendServiceImpl::Probe(grpc::ServerContext* /*context*/,
                                          const pdfv1::ProbeRequest* request,
                                          pdfv1::ProbeResponse* response) {
  if (!ServesText(role_)) return WrongRole(role_, "Probe");
  std::lock_guard<std::mutex> lock(EngineMutex());
  PdfiumEngine::InitProcess();
  PdfiumEngine::Probe(request->document(), response->mutable_capabilities());
  return grpc::Status::OK;
}

grpc::Status PdfBackendServiceImpl::Parse(
    grpc::ServerContext* /*context*/, const pdfv1::ParseRequest* request,
    grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  if (!ServesText(role_)) return WrongRole(role_, "Parse");
  grpc::Status checked = CheckParseRequest(*request);
  if (!checked.ok()) return checked;
  std::lock_guard<std::mutex> lock(EngineMutex());
  PdfiumEngine::InitProcess();
  PdfiumEngine::Parse(*request, [writer](const pdfv1::ParseResponse& msg) {
    return writer->Write(msg);
  });
  return grpc::Status::OK;
}

grpc::Status PdfBackendServiceImpl::Render(
    grpc::ServerContext* /*context*/, const pdfv1::RenderRequest* request,
    grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  if (!ServesRender(role_)) return WrongRole(role_, "Render");
  grpc::Status checked = CheckRenderRequest(*request);
  if (!checked.ok()) return checked;
  std::lock_guard<std::mutex> lock(EngineMutex());
  PdfiumEngine::InitProcess();
  return PdfiumEngine::Render(
      *request,
      [writer](const pdfv1::RenderResponse& msg) { return writer->Write(msg); });
}

grpc::Status PdfBackendServiceImpl::ParseStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<pdfv1::ParseStreamResponse, pdfv1::ParseStreamRequest>*
        stream) {
  if (role_ != WorkerRole::kAny) return WrongRole(role_, "ParseStream");
  ParseStreamHandlers handlers;
  // The engine reads the bytes from the request, so each request gets its
  // own copy; one at a time, behind the engine mutex like every call.
  handlers.parse = [](const pdfv1::ParseRequest& request,
                      const std::shared_ptr<std::string>& bytes,
                      MessageSink<pdfv1::ParseResponse>* sink, RequestCancel*) {
    grpc::Status checked = CheckParseRequest(request);
    if (!checked.ok()) return checked;
    pdfv1::ParseRequest filled = request;
    filled.mutable_document()->set_data(*bytes);
    std::lock_guard<std::mutex> lock(EngineMutex());
    PdfiumEngine::InitProcess();
    PdfiumEngine::Parse(filled, [sink](const pdfv1::ParseResponse& msg) {
      return sink->Write(msg);
    });
    return grpc::Status::OK;
  };
  handlers.render = [](const pdfv1::RenderRequest& request,
                       const std::shared_ptr<std::string>& bytes,
                       MessageSink<pdfv1::RenderResponse>* sink, RequestCancel*) {
    grpc::Status checked = CheckRenderRequest(request);
    if (!checked.ok()) return checked;
    pdfv1::RenderRequest filled = request;
    filled.mutable_document()->set_data(*bytes);
    std::lock_guard<std::mutex> lock(EngineMutex());
    PdfiumEngine::InitProcess();
    return PdfiumEngine::Render(filled, [sink](const pdfv1::RenderResponse& msg) {
      return sink->Write(msg);
    });
  };
  handlers.verdict = [](pdfv1::BackendCapabilities* caps, pdfv1::LoadStatus status,
                        const std::string& detail) {
    caps->set_backend_name(PdfiumEngine::BackendName());
    caps->set_engine_version(PdfiumEngine::EngineVersion());
    caps->set_load_status(status);
    caps->set_load_detail(detail);
  };
  handlers.sha256_hex = Sha256Hex;
  ParseStreamLimits limits;
  limits.max_document_bytes = kMaxWorkerStreamBytes;
  limits.max_parallel_requests = 1;
  return ServeParseStream(context, stream, handlers, limits);
}

grpc::Status PdfBackendServiceImpl::GetServiceInfo(
    grpc::ServerContext* /*context*/,
    const pdfv1::ServiceInfoRequest* /*request*/,
    pdfv1::ServiceInfoResponse* response) {
  FillServiceInfo(response);
  return grpc::Status::OK;
}

}  // namespace grpc_pdfium
