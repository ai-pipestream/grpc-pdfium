// Pool test: launch the real front binary (which spawns its worker
// processes), parse its listening line for the ephemeral port, dial it,
// and check that Probe and Parse round-trip through the pool. This is the
// process topology production runs, not an in-process shortcut.
//
// The second half exercises the content-addressed handshake
// (PdfDocument.sha256): the byte cache lives in the front, so these checks
// must go through the real binary. The child runs with
// GRPC_PDFIUM_CACHE_MAX_DOCUMENTS=2 so eviction is reachable.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "sha256.h"

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <grpc_pdfium binary> <hello.pdf>\n",
                 argv[0]);
    return 2;
  }

  std::string fixture;
  {
    std::ifstream in(argv[2], std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    fixture = buf.str();
  }
  Check(!fixture.empty(), "fixture PDF read");

  int out_pipe[2];
  Check(pipe(out_pipe) == 0, "stdout pipe created");
  pid_t front = fork();
  if (front == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    setenv("GRPC_PDFIUM_PORT", "0", 1);
    setenv("GRPC_PDFIUM_WORKERS", "2", 1);
    setenv("GRPC_PDFIUM_CACHE_MAX_DOCUMENTS", "2", 1);
    execl(argv[1], argv[1], static_cast<char*>(nullptr));
    std::perror("execl front");
    _exit(127);
  }
  close(out_pipe[1]);

  // Read the listening line to learn the bound port.
  std::string line;
  char c = 0;
  while (read(out_pipe[0], &c, 1) == 1 && c != '\n') line.push_back(c);
  const char* marker = "listening on 0.0.0.0:";
  size_t pos = line.find(marker);
  int port = 0;
  if (pos != std::string::npos) {
    port = std::atoi(line.c_str() + pos + std::strlen(marker));
  }
  Check(port > 0, "front reported its bound port");

  if (port > 0) {
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                       grpc::InsecureChannelCredentials());
    auto stub = pdfv1::PdfBackendService::NewStub(channel);

    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_data(fixture);
      pdfv1::ProbeResponse response;
      grpc::Status status = stub->Probe(&ctx, request, &response);
      Check(status.ok(), "Probe through the pool succeeded");
      Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
            "pool Probe loads the fixture");
    }

    {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(fixture);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      std::string all_text;
      bool saw_header = false;
      bool saw_trailer = false;
      while (reader->Read(&message)) {
        if (message.has_header()) saw_header = true;
        if (message.has_trailer()) saw_trailer = true;
        if (message.has_page()) {
          for (const auto& cell : message.page().text_cells()) {
            all_text += cell.text() + " ";
          }
        }
      }
      Check(reader->Finish().ok(), "pool Parse finished OK");
      Check(saw_header && saw_trailer, "pool Parse framed header and trailer");
      Check(all_text.find("Hello PDF") != std::string::npos,
            "pool Parse returned the fixture text");
    }

    // The content-addressed handshake. The cache runs with capacity 2
    // (GRPC_PDFIUM_CACHE_MAX_DOCUMENTS above) so eviction is testable.
    Check(grpc_pdfium::Sha256Hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 matches the empty-string vector");
    const std::string fixture_sha = grpc_pdfium::Sha256Hex(fixture);
    Check(fixture_sha.size() == 64, "fixture hash is lowercase hex");
    std::string wrong_sha = fixture_sha;
    wrong_sha[0] = wrong_sha[0] == '0' ? '1' : '0';

    // First call by hash: cache miss, typed BYTES_REQUIRED, no gRPC error.
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_sha256(fixture_sha);
      pdfv1::ProbeResponse response;
      grpc::Status status = stub->Probe(&ctx, request, &response);
      Check(status.ok(), "Probe by hash is not a gRPC error");
      Check(response.capabilities().load_status() ==
                pdfv1::LOAD_STATUS_BYTES_REQUIRED,
            "unknown hash reports LOAD_STATUS_BYTES_REQUIRED");
    }

    // Bytes with a wrong hash: typed HASH_MISMATCH on all three surfaces.
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      auto* doc = request.mutable_document();
      doc->set_data(fixture);
      doc->set_sha256(wrong_sha);
      pdfv1::ProbeResponse response;
      grpc::Status status = stub->Probe(&ctx, request, &response);
      Check(status.ok(), "hash mismatch Probe is not a gRPC error");
      Check(response.capabilities().load_status() ==
                pdfv1::LOAD_STATUS_HASH_MISMATCH,
            "wrong hash reports LOAD_STATUS_HASH_MISMATCH");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      auto* doc = request.mutable_document();
      doc->set_data(fixture);
      doc->set_sha256(wrong_sha);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      Check(reader->Read(&message) && message.has_header(),
            "hash mismatch Parse still sends the header");
      Check(message.header().capabilities().load_status() ==
                pdfv1::LOAD_STATUS_HASH_MISMATCH,
            "Parse header reports LOAD_STATUS_HASH_MISMATCH");
      Check(!reader->Read(&message), "Parse ends after the mismatch header");
      Check(reader->Finish().ok(), "hash mismatch Parse is not a gRPC error");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      auto* doc = request.mutable_document();
      doc->set_data(fixture);
      doc->set_sha256(wrong_sha);
      request.set_dpi(72.0);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      Check(reader->Read(&message) && message.has_head(),
            "hash mismatch Render sends the head");
      Check(message.head().load_status() == pdfv1::LOAD_STATUS_HASH_MISMATCH,
            "Render head reports LOAD_STATUS_HASH_MISMATCH");
      Check(!reader->Read(&message), "Render ends after the mismatch head");
      Check(reader->Finish().ok(), "hash mismatch Render is not a gRPC error");
    }

    // The retry with bytes and the right hash succeeds and caches them.
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      auto* doc = request.mutable_document();
      doc->set_data(fixture);
      doc->set_sha256(fixture_sha);
      pdfv1::ProbeResponse response;
      grpc::Status status = stub->Probe(&ctx, request, &response);
      Check(status.ok(), "Probe with bytes and hash succeeded");
      Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
            "retry with bytes loads the fixture");
    }

    // Second call by hash: the bytes are cached, none ride the wire.
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_sha256(fixture_sha);
      pdfv1::ProbeResponse response;
      grpc::Status status = stub->Probe(&ctx, request, &response);
      Check(status.ok(), "second Probe by hash succeeded");
      Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
            "cached hash probes the fixture");
      Check(response.capabilities().page_count() == 1,
            "cached hash reports the fixture's page count");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_sha256(fixture_sha);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      std::string all_text;
      bool saw_trailer = false;
      while (reader->Read(&message)) {
        if (message.has_trailer()) saw_trailer = true;
        if (message.has_page()) {
          for (const auto& cell : message.page().text_cells()) {
            all_text += cell.text() + " ";
          }
        }
      }
      Check(reader->Finish().ok(), "Parse by hash finished OK");
      Check(all_text.find("Hello PDF") != std::string::npos,
            "Parse by hash returned the fixture text");
      Check(saw_trailer, "Parse by hash ends with the trailer");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.mutable_document()->set_sha256(fixture_sha);
      request.set_dpi(72.0);
      request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      Check(reader->Read(&message) && message.has_raster(),
            "Render by hash produced a raster");
      Check(message.raster().width_px() == 612 &&
                message.raster().height_px() == 792,
            "Render by hash rasterized the cached bytes");
      Check(reader->Finish().ok(), "Render by hash finished OK");
    }

    // Cache misses on Parse and Render: one typed message, then the stream
    // ends, and Finish stays OK.
    {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_sha256(
          "0000000000000000000000000000000000000000000000000000000000000000");
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      Check(reader->Read(&message) && message.has_header(),
            "Parse cache miss sends the header");
      Check(message.header().capabilities().load_status() ==
                pdfv1::LOAD_STATUS_BYTES_REQUIRED,
            "Parse header reports LOAD_STATUS_BYTES_REQUIRED");
      Check(!reader->Read(&message), "Parse cache miss ends after the header");
      Check(reader->Finish().ok(), "Parse cache miss is not a gRPC error");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.mutable_document()->set_sha256(
          "0000000000000000000000000000000000000000000000000000000000000000");
      request.set_dpi(72.0);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      Check(reader->Read(&message) && message.has_head(),
            "Render cache miss sends the head");
      Check(message.head().load_status() == pdfv1::LOAD_STATUS_BYTES_REQUIRED,
            "Render head reports LOAD_STATUS_BYTES_REQUIRED");
      Check(!reader->Read(&message), "Render cache miss ends after the head");
      Check(reader->Finish().ok(), "Render cache miss is not a gRPC error");
    }

    // Empty data without a hash is INVALID_ARGUMENT on all three RPCs.
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      pdfv1::ProbeResponse response;
      grpc::Status status = stub->Probe(&ctx, request, &response);
      Check(status.error_code() == grpc::INVALID_ARGUMENT,
            "empty data without hash is INVALID_ARGUMENT on Probe");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      Check(!reader->Read(&message), "invalid Parse streams nothing");
      Check(reader->Finish().error_code() == grpc::INVALID_ARGUMENT,
            "empty data without hash is INVALID_ARGUMENT on Parse");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.set_dpi(72.0);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      Check(!reader->Read(&message), "invalid Render streams nothing");
      Check(reader->Finish().error_code() == grpc::INVALID_ARGUMENT,
            "empty data without hash is INVALID_ARGUMENT on Render");
    }

    // Eviction: capacity is 2, so caching two junk documents evicts the
    // fixture; the junk bytes stay cached even though they are not PDFs.
    {
      const std::string junk_a = "junk document a bytes";
      const std::string junk_b = "junk document b bytes";
      for (const std::string* junk : {&junk_a, &junk_b}) {
        grpc::ClientContext ctx;
        pdfv1::ProbeRequest request;
        auto* doc = request.mutable_document();
        doc->set_data(*junk);
        doc->set_sha256(grpc_pdfium::Sha256Hex(*junk));
        pdfv1::ProbeResponse response;
        grpc::Status status = stub->Probe(&ctx, request, &response);
        Check(status.ok(), "junk upload Probe succeeded");
        Check(response.capabilities().load_status() ==
                  pdfv1::LOAD_STATUS_NOT_PDF,
              "junk bytes report LOAD_STATUS_NOT_PDF");
      }
      {
        grpc::ClientContext ctx;
        pdfv1::ProbeRequest request;
        request.mutable_document()->set_sha256(fixture_sha);
        pdfv1::ProbeResponse response;
        grpc::Status status = stub->Probe(&ctx, request, &response);
        Check(status.ok(), "evicted Probe is not a gRPC error");
        Check(response.capabilities().load_status() ==
                  pdfv1::LOAD_STATUS_BYTES_REQUIRED,
              "the fixture was evicted at capacity 2");
      }
      {
        grpc::ClientContext ctx;
        pdfv1::ProbeRequest request;
        request.mutable_document()->set_sha256(grpc_pdfium::Sha256Hex(junk_a));
        pdfv1::ProbeResponse response;
        grpc::Status status = stub->Probe(&ctx, request, &response);
        Check(status.ok(), "junk Probe by hash succeeded");
        Check(response.capabilities().load_status() ==
                  pdfv1::LOAD_STATUS_NOT_PDF,
              "the newest junk document stayed cached");
      }
    }
  }

  kill(front, SIGTERM);
  // The front owns worker children; give it a moment, then make sure it is
  // gone either way.
  int status = 0;
  if (waitpid(front, &status, WNOHANG) == 0) {
    sleep(1);
    if (waitpid(front, &status, WNOHANG) == 0) {
      kill(front, SIGKILL);
      waitpid(front, &status, 0);
    }
  }
  close(out_pipe[0]);

  if (failures == 0) {
    std::printf("pool_roundtrip: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "pool_roundtrip: %d check(s) failed\n", failures);
  return 1;
}
