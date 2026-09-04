// Pool test: launch the real front binary (which spawns its worker
// processes), parse its listening line for the ephemeral port, dial it,
// and check that Probe and Parse round-trip through the pool. This is the
// process topology production runs, not an in-process shortcut.

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

#include "ai/pipestream/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace pdfv1 = ai::pipestream::parse::pdf::v1;

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
            all_text += cell.text();
          }
        }
      }
      Check(reader->Finish().ok(), "pool Parse finished OK");
      Check(saw_header && saw_trailer, "pool Parse framed header and trailer");
      Check(all_text.find("Hello PDF") != std::string::npos,
            "pool Parse returned the fixture text");
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
