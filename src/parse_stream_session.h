#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace grpc_pdfium {

// Where one Parse or Render answer goes: the unary RPC's ServerWriter, or
// one request's slot on a ParseStream. Write returns false once nobody
// reads the answer any more (the client went away, or a Cancel named the
// request), and the work serving it stops.
template <typename Message>
class MessageSink {
 public:
  virtual ~MessageSink() = default;
  virtual bool Write(const Message& message) = 0;
};

// The unary RPC's sink.
template <typename Message>
class ServerWriterSink final : public MessageSink<Message> {
 public:
  explicit ServerWriterSink(grpc::ServerWriter<Message>* writer)
      : writer_(writer) {}
  bool Write(const Message& message) override { return writer_->Write(message); }

 private:
  grpc::ServerWriter<Message>* writer_;
};

// The cancellation of one ParseStream request, for the work serving it.
// A hook set while the work waits on something that does not write (a
// worker call computing a page) runs when the Cancel arrives, so the wait
// ends then and not at the next write. Thread safe.
class RequestCancel {
 public:
  bool cancelled() const;
  // Runs hook when the request is cancelled, at once when it already is.
  // The hook runs under this object's lock, so once ClearHook returns it
  // is not running and never runs again.
  void SetHook(std::function<void()> hook);
  void ClearHook();
  void Cancel();

 private:
  mutable std::mutex mutex_;
  bool cancelled_ = false;
  std::function<void()> hook_;
};

// What ParseStream needs from the service serving it. Each request arrives
// as the ParseRequest or RenderRequest the unary RPC would get, with the
// open's password, doc_id and the upload's sha256 in document() and data
// left empty: the stream's bytes come beside it, uploaded once.
struct ParseStreamHandlers {
  std::function<grpc::Status(
      const ai::protomolt::parse::pdf::v1::ParseRequest& request,
      const std::shared_ptr<std::string>& bytes,
      MessageSink<ai::protomolt::parse::pdf::v1::ParseResponse>* sink,
      RequestCancel* cancel)>
      parse;
  std::function<grpc::Status(
      const ai::protomolt::parse::pdf::v1::RenderRequest& request,
      const std::shared_ptr<std::string>& bytes,
      MessageSink<ai::protomolt::parse::pdf::v1::RenderResponse>* sink,
      RequestCancel* cancel)>
      render;
  // Fills the capabilities of a typed load verdict the stream answers
  // itself (LOAD_STATUS_HASH_MISMATCH).
  std::function<void(ai::protomolt::parse::pdf::v1::BackendCapabilities* caps,
                     ai::protomolt::parse::pdf::v1::LoadStatus status,
                     const std::string& detail)>
      verdict;
  // Lowercase hex SHA-256.
  std::function<std::string(const std::string& bytes)> sha256_hex;
  // Called once with the upload when it matched the open's sha256, so the
  // service can cache it for later calls addressed by hash. May be empty.
  std::function<void(const std::string& sha256,
                     const std::shared_ptr<std::string>& bytes)>
      verified;
};

struct ParseStreamLimits {
  // The largest upload one stream accepts; past it the stream fails
  // RESOURCE_EXHAUSTED.
  uint64_t max_document_bytes = 0;
  // How many of one stream's requests are served at once.
  unsigned max_parallel_requests = 1;
};

// Serves one ParseStream call to its end: assembles the chunked upload,
// answers the open as request 0 once the bytes are complete, serves
// ParsePages and RenderPages through the handlers (up to
// max_parallel_requests at once, none before the open's header), honors
// Cancel, and closes with ParseStreamDone. Protocol violations fail the
// stream with INVALID_ARGUMENT; the reading stays on the calling thread,
// so a Cancel is seen while requests run.
grpc::Status ServeParseStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<ai::protomolt::parse::pdf::v1::ParseStreamResponse,
                             ai::protomolt::parse::pdf::v1::ParseStreamRequest>*
        stream,
    const ParseStreamHandlers& handlers, const ParseStreamLimits& limits);

}  // namespace grpc_pdfium
