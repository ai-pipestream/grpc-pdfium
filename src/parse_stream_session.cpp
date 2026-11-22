#include "parse_stream_session.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

bool RequestCancel::cancelled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cancelled_;
}

void RequestCancel::SetHook(std::function<void()> hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_) {
    hook();
    return;
  }
  hook_ = std::move(hook);
}

void RequestCancel::ClearHook() {
  std::lock_guard<std::mutex> lock(mutex_);
  hook_ = nullptr;
}

void RequestCancel::Cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_) return;
  cancelled_ = true;
  if (hook_) hook_();
  hook_ = nullptr;
}

namespace {

using Stream = grpc::ServerReaderWriter<pdfv1::ParseStreamResponse,
                                        pdfv1::ParseStreamRequest>;

// How a request's handler status reads on the wire.
pdfv1::RequestOutcome OutcomeOf(const grpc::Status& status) {
  switch (status.error_code()) {
    case grpc::StatusCode::OK:
      return pdfv1::REQUEST_OUTCOME_OK;
    case grpc::StatusCode::CANCELLED:
      return pdfv1::REQUEST_OUTCOME_CANCELLED;
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::OUT_OF_RANGE:
      return pdfv1::REQUEST_OUTCOME_INVALID_ARGUMENT;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return pdfv1::REQUEST_OUTCOME_RESOURCE_EXHAUSTED;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return pdfv1::REQUEST_OUTCOME_DEADLINE_EXCEEDED;
    default:
      return pdfv1::REQUEST_OUTCOME_ENGINE_ERROR;
  }
}

grpc::Status Invalid(const std::string& message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message);
}

// One request of the stream: the open's parse (id 0), a ParsePages or a
// RenderPages, as the unary request it is served as.
struct Job {
  uint32_t id = 0;
  std::optional<pdfv1::ParseRequest> parse;
  std::optional<pdfv1::RenderRequest> render;
  // The open asked for its header alone: the sink drops everything else.
  bool header_only = false;
  std::shared_ptr<RequestCancel> cancel = std::make_shared<RequestCancel>();
};

class Session {
 public:
  Session(grpc::ServerContext* context, Stream* stream,
          const ParseStreamHandlers& handlers, const ParseStreamLimits& limits)
      : context_(context),
        stream_(stream),
        handlers_(handlers),
        limits_(limits),
        bytes_(std::make_shared<std::string>()) {}

  grpc::Status Run() {
    const unsigned workers = limits_.max_parallel_requests > 0
                                 ? limits_.max_parallel_requests
                                 : 1;
    for (unsigned i = 0; i < workers; ++i) {
      workers_.emplace_back([this] { WorkLoop(); });
    }
    grpc::Status status = ReadLoop();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
      if (!status.ok() || context_->IsCancelled()) StopLocked();
    }
    changed_.notify_all();
    for (std::thread& worker : workers_) worker.join();
    for (const uint32_t id : dropped_) SendRequestDone(id, pdfv1::REQUEST_OUTCOME_CANCELLED, "");
    if (!status.ok()) return status;
    if (context_->IsCancelled()) return grpc::Status::CANCELLED;
    pdfv1::ParseStreamResponse done;
    auto* body = done.mutable_done();
    body->set_bytes_received(bytes_->size());
    body->set_sha256(digest_);
    body->set_cancelled(cancelled_all_);
    Send(done);
    return grpc::Status::OK;
  }

  // Writes one stream message; gRPC allows one writer at a time. After a
  // failed write the client is gone and nothing more is sent.
  bool Send(const pdfv1::ParseStreamResponse& message) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_failed_) return false;
    if (!stream_->Write(message)) write_failed_ = true;
    return !write_failed_;
  }

  bool write_failed() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return write_failed_;
  }

  // The open's header went out: the stream is open, and later requests may
  // be answered. A load verdict other than OK leaves them unserved.
  void HeaderSent(const pdfv1::BackendCapabilities& caps) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (caps.load_status() != pdfv1::LOAD_STATUS_OK) {
        load_failed_ = true;
        load_detail_ = pdfv1::LoadStatus_Name(caps.load_status());
        if (!caps.load_detail().empty()) load_detail_ += ": " + caps.load_detail();
      }
      opened_ = true;
    }
    changed_.notify_all();
  }

 private:
  template <typename Message>
  class RequestSink final : public MessageSink<Message> {
   public:
    RequestSink(Session* session, const Job& job) : session_(session), job_(job) {}

    bool Write(const Message& message) override {
      if (job_.cancel->cancelled()) return false;
      pdfv1::ParseStreamResponse out;
      out.set_request_id(job_.id);
      if constexpr (std::is_same_v<Message, pdfv1::ParseResponse>) {
        const bool header = message.has_header();
        // A header-only open answers with the header and nothing else; the
        // rest of the inventory parse is dropped, not an error.
        if (job_.header_only && !header) return true;
        *out.mutable_parse() = message;
        const bool sent = session_->Send(out);
        if (job_.id == 0 && header) session_->HeaderSent(message.header().capabilities());
        return sent;
      } else {
        *out.mutable_render() = message;
        return session_->Send(out);
      }
    }

   private:
    Session* session_;
    const Job& job_;
  };

  void SendRequestDone(uint32_t id, pdfv1::RequestOutcome outcome,
                       const std::string& detail) {
    pdfv1::ParseStreamResponse out;
    out.set_request_id(id);
    auto* done = out.mutable_request_done();
    done->set_outcome(outcome);
    if (!detail.empty()) done->set_detail(detail);
    Send(out);
  }

  pdfv1::PdfDocument Document() const {
    pdfv1::PdfDocument document;
    if (open_.has_password()) document.set_password(open_.password());
    if (open_.has_doc_id()) document.set_doc_id(open_.doc_id());
    document.set_sha256(digest_);
    return document;
  }

  // Reads until the client half-closes, cancels the stream, or breaks the
  // protocol. Requests are queued as they arrive; the workers serve them.
  grpc::Status ReadLoop() {
    pdfv1::ParseStreamRequest message;
    if (!stream_->Read(&message)) {
      return context_->IsCancelled() ? grpc::Status::CANCELLED
                                     : Invalid("the stream ended before the open");
    }
    if (!message.has_open()) return Invalid("the first message must be the open");
    open_ = message.open();
    if (open_.has_declared_size()) {
      if (open_.declared_size() > limits_.max_document_bytes) {
        return TooLarge(open_.declared_size());
      }
      bytes_->reserve(open_.declared_size());
    }
    bool upload_done = false;
    while (stream_->Read(&message)) {
      switch (message.payload_case()) {
        case pdfv1::ParseStreamRequest::kOpen:
          return Invalid("the stream was already opened");
        case pdfv1::ParseStreamRequest::kChunk: {
          if (upload_done) return Invalid("a chunk arrived after the last one");
          grpc::Status taken = TakeChunk(message.chunk());
          if (!taken.ok()) return taken;
          if (message.chunk().last()) {
            upload_done = true;
            grpc::Status finished = FinishUpload();
            if (!finished.ok()) return finished;
          }
          break;
        }
        case pdfv1::ParseStreamRequest::kParse:
        case pdfv1::ParseStreamRequest::kRender: {
          grpc::Status queued = Enqueue(message);
          if (!queued.ok()) return queued;
          break;
        }
        case pdfv1::ParseStreamRequest::kCancel:
          if (!message.cancel().has_request_id()) {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_all_ = true;
            StopLocked();
            return grpc::Status::OK;
          }
          CancelOne(message.cancel().request_id());
          break;
        case pdfv1::ParseStreamRequest::PAYLOAD_NOT_SET:
          return Invalid("a stream message carried no payload");
      }
    }
    if (context_->IsCancelled()) return grpc::Status::CANCELLED;
    if (!upload_done) return Invalid("the stream was half-closed before the last chunk");
    return grpc::Status::OK;
  }

  grpc::Status TooLarge(uint64_t size) const {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "the document is " + std::to_string(size) +
                            " bytes, above the stream limit of " +
                            std::to_string(limits_.max_document_bytes));
  }

  grpc::Status TakeChunk(const pdfv1::PdfChunk& chunk) {
    if (chunk.offset() != bytes_->size()) {
      return Invalid("chunk offset " + std::to_string(chunk.offset()) +
                     " does not follow the " + std::to_string(bytes_->size()) +
                     " bytes received");
    }
    if (chunk.data().empty() && !chunk.last()) {
      return Invalid("only the last chunk may be empty");
    }
    const uint64_t size = bytes_->size() + chunk.data().size();
    if (size > limits_.max_document_bytes) return TooLarge(size);
    if (open_.has_declared_size() && size > open_.declared_size()) {
      return Invalid("the upload runs past its declared size of " +
                     std::to_string(open_.declared_size()) + " bytes");
    }
    bytes_->append(chunk.data());
    return grpc::Status::OK;
  }

  // The bytes are complete: check them against the open, then let the open's
  // parse run.
  grpc::Status FinishUpload() {
    if (open_.has_declared_size() && bytes_->size() != open_.declared_size()) {
      return Invalid("the upload ended at " + std::to_string(bytes_->size()) +
                     " bytes, short of its declared size of " +
                     std::to_string(open_.declared_size()));
    }
    digest_ = handlers_.sha256_hex(*bytes_);
    if (open_.has_sha256() && open_.sha256() != digest_) {
      // The handshake's typed verdict, answered here: the header carries
      // it, request 0 ends, and later requests are not served.
      pdfv1::ParseStreamResponse header;
      header.set_request_id(0);
      pdfv1::BackendCapabilities* caps =
          header.mutable_parse()->mutable_header()->mutable_capabilities();
      handlers_.verdict(caps, pdfv1::LOAD_STATUS_HASH_MISMATCH,
                        "data does not hash to the supplied sha256 " + open_.sha256());
      Send(header);
      HeaderSent(*caps);
      SendRequestDone(0, pdfv1::REQUEST_OUTCOME_OK, "");
      std::lock_guard<std::mutex> lock(mutex_);
      upload_complete_ = true;
      return grpc::Status::OK;
    }
    if (open_.has_sha256() && handlers_.verified) handlers_.verified(digest_, bytes_);
    Job job;
    job.id = 0;
    pdfv1::ParseRequest& request = job.parse.emplace();
    *request.mutable_document() = Document();
    if (open_.header_only()) {
      // The header and nothing else: the inventory family over a range past
      // any last page, which every backend clamps to no page.
      job.header_only = true;
      request.add_families(pdfv1::PDF_FAMILY_PAGE_INVENTORY);
      request.mutable_pages()->set_begin(std::numeric_limits<uint32_t>::max() - 1);
      request.mutable_pages()->set_end(std::numeric_limits<uint32_t>::max());
    } else {
      *request.mutable_families() = open_.families();
      if (open_.has_pages()) *request.mutable_pages() = open_.pages();
      *request.mutable_options() = open_.options();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      upload_complete_ = true;
      used_ids_.insert(0);
      queue_.push_front(std::move(job));
    }
    changed_.notify_all();
    return grpc::Status::OK;
  }

  grpc::Status Enqueue(const pdfv1::ParseStreamRequest& message) {
    Job job;
    if (message.has_parse()) {
      const pdfv1::ParsePages& parse = message.parse();
      job.id = parse.request_id();
      pdfv1::ParseRequest& request = job.parse.emplace();
      *request.mutable_families() = parse.families();
      if (parse.has_pages()) *request.mutable_pages() = parse.pages();
      *request.mutable_options() = parse.options();
    } else {
      const pdfv1::RenderPages& render = message.render();
      job.id = render.request_id();
      pdfv1::RenderRequest& request = job.render.emplace();
      if (render.has_pages()) *request.mutable_pages() = render.pages();
      request.set_dpi(render.dpi());
      request.set_pixel_format(render.pixel_format());
      if (render.has_background()) *request.mutable_background() = render.background();
      request.set_omit_annotations(render.omit_annotations());
    }
    if (job.id == 0) return Invalid("request_id 0 is the open's; page requests need their own");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!used_ids_.insert(job.id).second) {
      return Invalid("request_id " + std::to_string(job.id) + " was already used");
    }
    queue_.push_back(std::move(job));
    changed_.notify_all();
    return grpc::Status::OK;
  }

  // A queued request keeps its place and ends CANCELLED when a worker
  // reaches it, so its RequestDone never overtakes the open's header; a
  // running one stops through its sink and hook.
  void CancelOne(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Job& job : queue_) {
      if (job.id == id) {
        job.cancel->Cancel();
        return;
      }
    }
    auto live = live_.find(id);
    if (live != live_.end()) live->second->Cancel();
  }

  // Under mutex_: no further request starts, and the running ones are
  // cancelled. Queued requests end CANCELLED once the workers are done.
  void StopLocked() {
    stopped_ = true;
    for (const Job& job : queue_) dropped_.push_back(job.id);
    queue_.clear();
    for (auto& [id, cancel] : live_) cancel->Cancel();
    changed_.notify_all();
  }

  // Under mutex_: the front job may start. Nothing starts before the bytes
  // are complete, and nothing but the open before the open's header.
  bool ReadyLocked() const {
    if (queue_.empty() || !upload_complete_) return false;
    return queue_.front().id == 0 || opened_;
  }

  void WorkLoop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] {
          return stopped_ || ReadyLocked() || (closing_ && queue_.empty());
        });
        if (stopped_ || !ReadyLocked()) return;
        job = std::move(queue_.front());
        queue_.pop_front();
        live_[job.id] = job.cancel;
      }
      Serve(job);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        live_.erase(job.id);
        // The open ended, header or not: later requests may run.
        if (job.id == 0) opened_ = true;
      }
      changed_.notify_all();
    }
  }

  void Serve(const Job& job) {
    if (job.cancel->cancelled()) {
      SendRequestDone(job.id, pdfv1::REQUEST_OUTCOME_CANCELLED, "");
      return;
    }
    std::string not_loaded;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (load_failed_ && job.id != 0) not_loaded = load_detail_;
    }
    if (!not_loaded.empty()) {
      SendRequestDone(job.id, pdfv1::REQUEST_OUTCOME_NOT_LOADED,
                      "the document did not load: " + not_loaded);
      return;
    }
    grpc::Status status;
    if (job.parse.has_value()) {
      pdfv1::ParseRequest request = *job.parse;
      if (job.id != 0) *request.mutable_document() = Document();
      RequestSink<pdfv1::ParseResponse> sink(this, job);
      status = handlers_.parse(request, bytes_, &sink, job.cancel.get());
    } else {
      pdfv1::RenderRequest request = *job.render;
      *request.mutable_document() = Document();
      RequestSink<pdfv1::RenderResponse> sink(this, job);
      status = handlers_.render(request, bytes_, &sink, job.cancel.get());
    }
    if (write_failed()) return;
    if (job.cancel->cancelled()) {
      SendRequestDone(job.id, pdfv1::REQUEST_OUTCOME_CANCELLED, "");
      return;
    }
    SendRequestDone(job.id, OutcomeOf(status), status.error_message());
  }

  grpc::ServerContext* context_;
  Stream* stream_;
  const ParseStreamHandlers& handlers_;
  const ParseStreamLimits& limits_;
  pdfv1::ParseStreamOpen open_;
  // Written by the reader until the upload completes, read-only after.
  std::shared_ptr<std::string> bytes_;
  std::string digest_;

  std::mutex write_mutex_;
  bool write_failed_ = false;

  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Job> queue_;
  std::map<uint32_t, std::shared_ptr<RequestCancel>> live_;
  std::set<uint32_t> used_ids_;
  std::vector<uint32_t> dropped_;
  bool upload_complete_ = false;
  bool opened_ = false;
  bool load_failed_ = false;
  std::string load_detail_;
  bool closing_ = false;
  bool stopped_ = false;
  bool cancelled_all_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace

grpc::Status ServeParseStream(grpc::ServerContext* context, Stream* stream,
                              const ParseStreamHandlers& handlers,
                              const ParseStreamLimits& limits) {
  Session session(context, stream, handlers, limits);
  return session.Run();
}

}  // namespace grpc_pdfium
