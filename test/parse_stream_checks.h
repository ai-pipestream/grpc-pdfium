// Client-side checks of the ParseStream contract, shared by the tests that
// dial a ParseStream server: the in-process service (contract_roundtrip)
// and the real front (pool_roundtrip). The fixture is any PDF of at least
// one page with text on page 0 ("Hello PDF" in hello.pdf).

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace parse_stream_checks {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

// Everything one ParseStream call answered.
struct StreamRun {
  std::vector<pdfv1::ParseStreamResponse> messages;
  grpc::Status status;

  // The RequestDone that ended request id, or nullptr.
  const pdfv1::RequestDone* DoneOf(uint32_t id) const {
    for (const auto& m : messages) {
      if (m.has_request_done() && m.request_id() == id) return &m.request_done();
    }
    return nullptr;
  }
  // The stream's ParseStreamDone, or nullptr.
  const pdfv1::ParseStreamDone* StreamDone() const {
    for (const auto& m : messages) {
      if (m.has_done()) return &m.done();
    }
    return nullptr;
  }
  std::vector<const pdfv1::ParseResponse*> ParseOf(uint32_t id) const {
    std::vector<const pdfv1::ParseResponse*> out;
    for (const auto& m : messages) {
      if (m.has_parse() && m.request_id() == id) out.push_back(&m.parse());
    }
    return out;
  }
  std::vector<const pdfv1::RenderResponse*> RenderOf(uint32_t id) const {
    std::vector<const pdfv1::RenderResponse*> out;
    for (const auto& m : messages) {
      if (m.has_render() && m.request_id() == id) out.push_back(&m.render());
    }
    return out;
  }
  std::string TextOf(uint32_t id) const {
    std::string text;
    for (const pdfv1::ParseResponse* p : ParseOf(id)) {
      if (!p->has_page()) continue;
      for (const auto& cell : p->page().text_cells()) text += cell.text() + " ";
    }
    return text;
  }
};

inline pdfv1::ParseStreamRequest OpenMessage(const pdfv1::ParseStreamOpen& open) {
  pdfv1::ParseStreamRequest m;
  *m.mutable_open() = open;
  return m;
}

inline pdfv1::ParseStreamRequest RenderMessage(uint32_t id, double dpi) {
  pdfv1::ParseStreamRequest m;
  auto* render = m.mutable_render();
  render->set_request_id(id);
  render->set_dpi(dpi);
  render->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
  render->mutable_pages()->set_begin(0);
  render->mutable_pages()->set_end(1);
  return m;
}

inline pdfv1::ParseStreamRequest ParseMessage(uint32_t id) {
  pdfv1::ParseStreamRequest m;
  auto* parse = m.mutable_parse();
  parse->set_request_id(id);
  parse->add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
  parse->mutable_pages()->set_begin(0);
  parse->mutable_pages()->set_end(1);
  return m;
}

inline pdfv1::ParseStreamRequest CancelMessage(std::optional<uint32_t> id) {
  pdfv1::ParseStreamRequest m;
  auto* cancel = m.mutable_cancel();
  if (id.has_value()) cancel->set_request_id(*id);
  return m;
}

// The open, then the bytes in chunk-sized pieces; inserts[i] goes out
// right after chunk i (an index past the last chunk appends at the end).
inline std::vector<pdfv1::ParseStreamRequest> Upload(
    const pdfv1::ParseStreamOpen& open, const std::string& bytes, size_t chunk,
    const std::vector<std::pair<size_t, pdfv1::ParseStreamRequest>>& inserts = {}) {
  std::vector<pdfv1::ParseStreamRequest> script{OpenMessage(open)};
  size_t index = 0;
  auto insert_after = [&](size_t i) {
    for (const auto& [at, message] : inserts) {
      if (at == i) script.push_back(message);
    }
  };
  for (size_t offset = 0; offset < bytes.size(); offset += chunk, ++index) {
    pdfv1::ParseStreamRequest m;
    auto* c = m.mutable_chunk();
    c->set_offset(offset);
    c->set_data(bytes.substr(offset, chunk));
    c->set_last(offset + chunk >= bytes.size());
    script.push_back(m);
    insert_after(index);
  }
  for (const auto& [at, message] : inserts) {
    if (at >= index) script.push_back(message);
  }
  return script;
}

// Writes the script, half-closes, and reads to the end. Writes and reads
// overlap: the server answers while the client still writes.
inline StreamRun Run(pdfv1::PdfBackendService::Stub* stub,
              const std::vector<pdfv1::ParseStreamRequest>& script) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(60));
  auto stream = stub->ParseStream(&ctx);
  for (const auto& m : script) {
    if (!stream->Write(m)) break;
  }
  stream->WritesDone();
  StreamRun run;
  pdfv1::ParseStreamResponse message;
  while (stream->Read(&message)) run.messages.push_back(message);
  run.status = stream->Finish();
  return run;
}

// Runs every ParseStream check against stub; failures go to check.
inline void CheckAll(pdfv1::PdfBackendService::Stub* stub, const std::string& fixture,
              const std::string& fixture_sha256,
              const std::function<void(bool, const char*)>& check) {
  pdfv1::ParseStreamOpen open;
  open.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
  open.add_families(pdfv1::PDF_FAMILY_FONTS);
  open.set_sha256(fixture_sha256);
  open.set_declared_size(fixture.size());
  const size_t chunk = fixture.size() / 4 + 1;

  // Chunked upload with a render sent mid-upload: the open's parse answers
  // first, the render on the same stream, both end OK, and the stream's
  // Done accounts for every byte.
  {
    const StreamRun run =
        Run(stub, Upload(open, fixture, chunk, {{0, RenderMessage(7, 72.0)}}));
    check(run.status.ok(), "ParseStream: chunked upload finishes OK");
    check(!run.messages.empty() && run.messages.front().request_id() == 0 &&
              run.messages.front().has_parse() &&
              run.messages.front().parse().has_header(),
          "ParseStream: the open's header is the first message");
    const auto parse = run.ParseOf(0);
    check(!parse.empty() &&
              parse.front()->header().capabilities().load_status() ==
                  pdfv1::LOAD_STATUS_OK,
          "ParseStream: the chunked document loads OK");
    check(run.TextOf(0).find("Hello") != std::string::npos,
          "ParseStream: the open's parse carries the fixture text");
    check(!parse.empty() && parse.back()->has_trailer(),
          "ParseStream: the open's parse ends with its trailer");
    const auto rasters = run.RenderOf(7);
    check(rasters.size() == 1 && rasters.front()->has_raster() &&
              rasters.front()->raster().page_index() == 0 &&
              rasters.front()->raster().width_px() > 0,
          "ParseStream: the render sent mid-upload is answered on the stream");
    check(run.DoneOf(0) != nullptr &&
              run.DoneOf(0)->outcome() == pdfv1::REQUEST_OUTCOME_OK,
          "ParseStream: request 0 ends OK");
    check(run.DoneOf(7) != nullptr &&
              run.DoneOf(7)->outcome() == pdfv1::REQUEST_OUTCOME_OK,
          "ParseStream: the render ends OK");
    const pdfv1::ParseStreamDone* done = run.StreamDone();
    check(done != nullptr && done->bytes_received() == fixture.size() &&
              done->sha256() == fixture_sha256 && !done->cancelled(),
          "ParseStream: Done counts every byte and names their hash");
    check(!run.messages.empty() && run.messages.back().has_done(),
          "ParseStream: Done is the last message");
  }

  // A header-only open, then pages asked for later, a bad render that fails
  // only its own request, and a cancel that reaches a request before it
  // starts (nothing starts before the last chunk).
  {
    pdfv1::ParseStreamOpen lean = open;
    lean.set_header_only(true);
    pdfv1::ParseStreamRequest bad_render = RenderMessage(4, -1.0);
    const StreamRun run = Run(
        stub, Upload(lean, fixture, chunk,
                     {{0, RenderMessage(5, 72.0)},
                      {0, CancelMessage(5)},
                      {99, ParseMessage(3)},
                      {99, bad_render}}));
    check(run.status.ok(), "ParseStream: header-only session finishes OK");
    const auto opened = run.ParseOf(0);
    check(opened.size() == 1 && opened.front()->has_header() &&
              opened.front()->header().pages_size() >= 1,
          "ParseStream: header_only answers the open with the header alone");
    check(run.TextOf(3).find("Hello") != std::string::npos,
          "ParseStream: ParsePages answers like Parse");
    check(run.DoneOf(3) != nullptr &&
              run.DoneOf(3)->outcome() == pdfv1::REQUEST_OUTCOME_OK,
          "ParseStream: ParsePages ends OK");
    check(run.RenderOf(5).empty() && run.DoneOf(5) != nullptr &&
              run.DoneOf(5)->outcome() == pdfv1::REQUEST_OUTCOME_CANCELLED,
          "ParseStream: a cancelled request ends CANCELLED and renders nothing");
    check(run.DoneOf(4) != nullptr &&
              run.DoneOf(4)->outcome() == pdfv1::REQUEST_OUTCOME_INVALID_ARGUMENT,
          "ParseStream: a bad dpi fails its request, not the stream");
  }

  // A Cancel for the whole stream ends it at once, OK, with Done saying so.
  {
    std::vector<pdfv1::ParseStreamRequest> script = Upload(open, fixture, chunk);
    script.resize(2);  // the open and the first chunk only
    script.push_back(CancelMessage(std::nullopt));
    const StreamRun run = Run(stub, script);
    check(run.status.ok(), "ParseStream: a stream-wide cancel ends OK");
    check(run.StreamDone() != nullptr && run.StreamDone()->cancelled(),
          "ParseStream: Done reports the cancel");
  }

  // The upload's hash is checked: a wrong sha256 is the handshake's typed
  // verdict, and later requests are not served.
  {
    pdfv1::ParseStreamOpen wrong = open;
    wrong.set_sha256(std::string(64, '0'));
    const StreamRun run =
        Run(stub, Upload(wrong, fixture, chunk, {{99, RenderMessage(2, 72.0)}}));
    check(run.status.ok(), "ParseStream: a hash mismatch is not a gRPC error");
    const auto opened = run.ParseOf(0);
    check(!opened.empty() && opened.front()->has_header() &&
              opened.front()->header().capabilities().load_status() ==
                  pdfv1::LOAD_STATUS_HASH_MISMATCH,
          "ParseStream: a wrong sha256 answers LOAD_STATUS_HASH_MISMATCH");
    check(run.DoneOf(2) != nullptr &&
              run.DoneOf(2)->outcome() == pdfv1::REQUEST_OUTCOME_NOT_LOADED,
          "ParseStream: requests after a failed open end NOT_LOADED");
  }

  // Protocol violations fail the stream.
  {
    std::vector<pdfv1::ParseStreamRequest> script = Upload(open, fixture, chunk);
    script.front() = RenderMessage(1, 72.0);
    check(Run(stub, script).status.error_code() == grpc::INVALID_ARGUMENT,
          "ParseStream: a stream that does not start with the open is refused");
  }
  {
    std::vector<pdfv1::ParseStreamRequest> script = Upload(open, fixture, chunk);
    script[2].mutable_chunk()->set_offset(script[2].chunk().offset() + 1);
    check(Run(stub, script).status.error_code() == grpc::INVALID_ARGUMENT,
          "ParseStream: a chunk out of order is refused");
  }
  {
    std::vector<pdfv1::ParseStreamRequest> script = Upload(open, fixture, chunk);
    script.pop_back();
    check(Run(stub, script).status.error_code() == grpc::INVALID_ARGUMENT,
          "ParseStream: a half-close before the last chunk is refused");
  }
  {
    const StreamRun run = Run(
        stub, Upload(open, fixture, chunk,
                     {{0, RenderMessage(6, 72.0)}, {0, RenderMessage(6, 72.0)}}));
    check(run.status.error_code() == grpc::INVALID_ARGUMENT,
          "ParseStream: a reused request_id is refused");
  }
}

}  // namespace parse_stream_checks
