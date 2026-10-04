// Pool test: launch the real front binary (which spawns its worker
// processes), parse its listening line for the ephemeral port, dial it,
// and check that Probe and Parse round-trip through the pool. This is the
// process topology production runs, not an in-process shortcut.
//
// The second half exercises the content-addressed handshake
// (PdfDocument.sha256): the byte cache lives in the front, so these checks
// must go through the real binary. The child runs with
// GRPC_PDFIUM_CACHE_MAX_DOCUMENTS=2 so eviction is reachable.
//
// The last part runs fronts with one worker and short limits, and freezes
// the worker with SIGSTOP the way a pathological document wedges one, to
// check that deadlines, cancellation, the bounded wait for a worker and
// the watchdog all free the pool.
// A front under a one-second watchdog parses a document whose page
// inventory takes seconds, which the worker's progress signals must carry
// through. A last front runs its worker under a 1 GiB address-space limit
// and feeds it a decompression bomb, which must cost that worker and
// nothing more.

#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

// A running front process and the port it bound.
struct Front {
  pid_t pid = -1;
  int port = 0;
  int out_fd = -1;
};

// Launches the front binary on an ephemeral port with extra environment and
// reads its listening line for the bound port.
Front StartFront(const char* binary,
                 const std::vector<std::pair<const char*, const char*>>& env) {
  Front front;
  int out_pipe[2];
  Check(pipe(out_pipe) == 0, "stdout pipe created");
  front.pid = fork();
  if (front.pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    setenv("GRPC_PDFIUM_PORT", "0", 1);
    for (const auto& [name, value] : env) setenv(name, value, 1);
    execl(binary, binary, static_cast<char*>(nullptr));
    std::perror("execl front");
    _exit(127);
  }
  close(out_pipe[1]);
  front.out_fd = out_pipe[0];
  std::string line;
  char c = 0;
  while (read(front.out_fd, &c, 1) == 1 && c != '\n') line.push_back(c);
  const char* marker = "listening on 0.0.0.0:";
  size_t pos = line.find(marker);
  if (pos != std::string::npos) {
    front.port = std::atoi(line.c_str() + pos + std::strlen(marker));
  }
  Check(front.port > 0, "front reported its bound port");
  return front;
}

// True when the front binary, run with extra environment, exits with a
// failure status within ten seconds instead of starting to serve.
bool RefusesToStart(
    const char* binary,
    const std::vector<std::pair<const char*, const char*>>& env) {
  const pid_t pid = fork();
  if (pid == 0) {
    setenv("GRPC_PDFIUM_PORT", "0", 1);
    setenv("GRPC_PDFIUM_WORKERS", "1", 1);
    for (const auto& [name, value] : env) setenv(name, value, 1);
    execl(binary, binary, static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      return WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
             WEXITSTATUS(status) != 127;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return false;
}

void StopFront(Front* front) {
  kill(front->pid, SIGTERM);
  // The front owns worker children; give it a moment, then make sure it is
  // gone either way.
  int status = 0;
  if (waitpid(front->pid, &status, WNOHANG) == 0) {
    sleep(1);
    if (waitpid(front->pid, &status, WNOHANG) == 0) {
      kill(front->pid, SIGKILL);
      waitpid(front->pid, &status, 0);
    }
  }
  close(front->out_fd);
}

// The front's worker processes: its children, found through /proc.
std::vector<pid_t> WorkerPids(pid_t front) {
  std::vector<pid_t> pids;
  DIR* proc = opendir("/proc");
  if (proc == nullptr) return pids;
  while (dirent* entry = readdir(proc)) {
    const pid_t pid = std::atoi(entry->d_name);
    if (pid <= 0) continue;
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    std::getline(stat_file, stat);
    // Fields after the command name, which sits in parentheses: state, ppid.
    const size_t close_paren = stat.rfind(')');
    if (close_paren == std::string::npos) continue;
    std::istringstream rest(stat.substr(close_paren + 1));
    std::string state;
    pid_t ppid = 0;
    rest >> state >> ppid;
    if (ppid == front) pids.push_back(pid);
  }
  closedir(proc);
  return pids;
}

// Whether pid is a live process rather than a zombie its parent has not
// reaped, from /proc.
bool Running(pid_t pid) {
  std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
  std::string stat;
  std::getline(stat_file, stat);
  const size_t close_paren = stat.rfind(')');
  if (close_paren == std::string::npos) return false;
  std::istringstream rest(stat.substr(close_paren + 1));
  std::string state;
  rest >> state;
  return !state.empty() && state != "Z" && state != "X";
}

// Freezes the front's only worker, the way a document that never finishes
// wedges one. The front's kill (SIGKILL) still ends it.
bool FreezeWorker(pid_t front) {
  const std::vector<pid_t> workers = WorkerPids(front);
  return workers.size() == 1 && kill(workers[0], SIGSTOP) == 0;
}

std::unique_ptr<pdfv1::PdfBackendService::Stub> Dial(int port) {
  return pdfv1::PdfBackendService::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                          grpc::InsecureChannelCredentials()));
}

// A process's resident set size in KiB, from /proc.
long RssKib(pid_t pid) {
  std::ifstream status("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6);
  }
  return -1;
}

// A process's peak resident set size in KiB, from /proc; -1 once it is gone.
long PeakRssKib(pid_t pid) {
  std::ifstream status("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmHWM:", 0) == 0) return std::atol(line.c_str() + 6);
  }
  return -1;
}

// A fixture that sits next to the one at fixture_path.
std::string ReadSibling(const std::string& fixture_path, const char* name) {
  const std::string dir = fixture_path.substr(0, fixture_path.rfind('/') + 1);
  std::ifstream in(dir + name, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// One Parse of the last page of a 100-page document (by hash alone when
// data is empty), and how long it took. inventory is the header's page
// count.
struct TimedParse {
  grpc::Status status;
  int inventory = 0;
  std::chrono::steady_clock::duration took{};
};

TimedParse ParseLastPage(pdfv1::PdfBackendService::Stub* stub,
                         const std::string& data, const std::string& sha) {
  TimedParse out;
  const auto started = std::chrono::steady_clock::now();
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(120));
  pdfv1::ParseRequest request;
  request.mutable_document()->set_data(data);
  request.mutable_document()->set_sha256(sha);
  request.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
  request.mutable_pages()->set_begin(99);
  request.mutable_pages()->set_end(100);
  auto reader = stub->Parse(&ctx, request);
  pdfv1::ParseResponse message;
  while (reader->Read(&message)) {
    if (message.has_header()) out.inventory = message.header().pages_size();
  }
  out.status = reader->Finish();
  out.took = std::chrono::steady_clock::now() - started;
  return out;
}

// A Probe of data with an optional deadline (zero means none).
grpc::Status ProbeWithin(pdfv1::PdfBackendService::Stub* stub,
                         const std::string& data,
                         std::chrono::milliseconds deadline) {
  grpc::ClientContext ctx;
  if (deadline.count() > 0) {
    ctx.set_deadline(std::chrono::system_clock::now() + deadline);
  }
  pdfv1::ProbeRequest request;
  request.mutable_document()->set_data(data);
  pdfv1::ProbeResponse response;
  return stub->Probe(&ctx, request, &response);
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

  Front front = StartFront(argv[1], {{"GRPC_PDFIUM_WORKERS", "2"},
                                     {"GRPC_PDFIUM_CACHE_MAX_DOCUMENTS", "2"}});
  if (front.port > 0) {
    auto stub = Dial(front.port);

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

    // GetServiceInfo is answered by the front itself, no worker round-trip.
    {
      grpc::ClientContext ctx;
      pdfv1::ServiceInfoRequest request;
      pdfv1::ServiceInfoResponse response;
      grpc::Status status = stub->GetServiceInfo(&ctx, request, &response);
      Check(status.ok(), "GetServiceInfo through the front succeeded");
      Check(response.backend_name() == "grpc-pdfium",
            "front service info reports the backend name");
      Check(response.engine_version() ==
                "pdfium 154.0.8035.0 (chromium/8035)",
            "front service info reports the engine version");
      Check(!response.build_version().empty(),
            "front service info reports a build version");
      Check(response.ui().title() == "PDFium" &&
                response.ui().path() == "/ui/pdfium" &&
                !response.ui().description().empty(),
            "front service info carries the UiInfo block");
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

    // A range the contract forbids (end not above begin) is answered
    // INVALID_ARGUMENT by the front itself, before it costs a worker.
    {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(fixture);
      request.mutable_pages()->set_begin(0xFFFFFFFFu);
      request.mutable_pages()->set_end(0xFFFFFFFFu);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      Check(!reader->Read(&message), "front streams nothing for a bad range");
      Check(reader->Finish().error_code() == grpc::INVALID_ARGUMENT,
            "front rejects a Parse range with end not above begin");
    }
    // A range above INT_MAX used to reach a worker as a negative page
    // index and crash it, twice per request with the retry. The worker now
    // clamps it to the page count, so it renders nothing, and to the end
    // of the document when begin is in range.
    {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.mutable_document()->set_data(fixture);
      request.set_dpi(72.0);
      request.mutable_pages()->set_begin(0x80000000u);
      request.mutable_pages()->set_end(0x80000001u);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      bool rastered = false;
      while (reader->Read(&message)) rastered = rastered || message.has_raster();
      Check(reader->Finish().ok() && !rastered,
            "a Render range above INT_MAX is clamped to no page");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.mutable_document()->set_data(fixture);
      request.set_dpi(72.0);
      request.mutable_pages()->set_begin(0);
      request.mutable_pages()->set_end(0xFFFFFFFFu);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      int rasters = 0;
      while (reader->Read(&message)) rasters += message.has_raster() ? 1 : 0;
      Check(reader->Finish().ok() && rasters == 1,
            "a Render range to UINT32_MAX renders the document");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(fixture);
      request.mutable_pages()->set_begin(0);
      request.mutable_pages()->set_end(1);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      bool saw_page = false;
      while (reader->Read(&message)) {
        if (message.has_page()) saw_page = true;
      }
      Check(reader->Finish().ok() && saw_page,
            "pool still parses after the out-of-range requests");
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

  StopFront(&front);

  using std::chrono::milliseconds;
  using std::chrono::seconds;

  // Limits that are not counts, or so large that adding them to a clock
  // would overflow it and put every deadline in the past, stop the front at
  // startup instead of being replaced or wrapping.
  Check(RefusesToStart(argv[1], {{"GRPC_PDFIUM_REQUEST_TIMEOUT_S", "10000000000"}}),
        "a request timeout past the ceiling stops the start");
  Check(RefusesToStart(argv[1],
                       {{"GRPC_PDFIUM_QUEUE_TIMEOUT_S", "99999999999999999999"}}),
        "a queue timeout that overflows strtoll stops the start");
  Check(RefusesToStart(argv[1], {{"GRPC_PDFIUM_REQUEST_TIMEOUT_S", "5m"}}),
        "a timeout that is not a number stops the start");
  Check(RefusesToStart(argv[1], {{"GRPC_PDFIUM_WORKER_MAX_BYTES", "1000"}}),
        "a worker limit below the minimum stops the start");

  // One worker, a long watchdog limit and a three-second wait for a free
  // worker: client deadlines and cancellation must reach the worker call,
  // and the wait must end. Before, the front waited on a frozen worker
  // forever and every later request queued behind it.
  front = StartFront(argv[1], {{"GRPC_PDFIUM_WORKERS", "1"},
                               {"GRPC_PDFIUM_REQUEST_TIMEOUT_S", "60"},
                               {"GRPC_PDFIUM_QUEUE_TIMEOUT_S", "3"}});
  if (front.port > 0) {
    auto stub = Dial(front.port);
    Check(ProbeWithin(stub.get(), fixture, seconds(20)).ok(),
          "one-worker front serves");

    Check(FreezeWorker(front.pid), "worker frozen");
    Check(ProbeWithin(stub.get(), fixture, seconds(1)).error_code() ==
              grpc::StatusCode::DEADLINE_EXCEEDED,
          "a frozen worker runs into the client's deadline");
    Check(ProbeWithin(stub.get(), fixture, seconds(20)).ok(),
          "the expired call's worker was replaced");

    Check(FreezeWorker(front.pid), "worker frozen again");
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_data(fixture);
      pdfv1::ProbeResponse response;
      std::thread canceller([&ctx] {
        std::this_thread::sleep_for(milliseconds(500));
        ctx.TryCancel();
      });
      const grpc::Status status = stub->Probe(&ctx, request, &response);
      canceller.join();
      Check(status.error_code() == grpc::StatusCode::CANCELLED,
            "the client cancels its call to a frozen worker");
    }
    Check(ProbeWithin(stub.get(), fixture, seconds(20)).ok(),
          "the cancelled call's worker was replaced");

    Check(FreezeWorker(front.pid), "worker frozen a third time");
    grpc::Status held;
    std::thread holder([&] { held = ProbeWithin(stub.get(), fixture, seconds(8)); });
    std::this_thread::sleep_for(milliseconds(500));
    const auto waited_from = std::chrono::steady_clock::now();
    const grpc::Status queued = ProbeWithin(stub.get(), fixture, seconds(30));
    const auto waited = std::chrono::steady_clock::now() - waited_from;
    holder.join();
    Check(queued.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED,
          "a request that finds every worker busy gets RESOURCE_EXHAUSTED");
    Check(waited >= seconds(2) && waited < seconds(7),
          "the wait for a worker ends at the queue limit");
    Check(held.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED,
          "the call holding the frozen worker hits its deadline");
    Check(ProbeWithin(stub.get(), fixture, seconds(20)).ok(),
          "the pool serves again after the frozen worker is replaced");
  }
  StopFront(&front);

  // A worker call still sending a large request to a worker that stopped
  // reading does not end when its client goes away, so the watchdog kills
  // the worker of a lease still held after its client left. Requests
  // waiting for a worker meanwhile hold no copy of the document: a request
  // addressed by hash gets the cached bytes filled in only once it leases a
  // worker (before, every queued request held a full copy in the front).
  front = StartFront(argv[1], {{"GRPC_PDFIUM_WORKERS", "1"},
                               {"GRPC_PDFIUM_REQUEST_TIMEOUT_S", "60"},
                               {"GRPC_PDFIUM_QUEUE_TIMEOUT_S", "60"}});
  if (front.port > 0) {
    auto stub = Dial(front.port);
    constexpr long kDocumentKib = 16 * 1024;
    const std::string big(static_cast<size_t>(kDocumentKib) * 1024, 'x');
    const std::string big_sha = grpc_pdfium::Sha256Hex(big);
    auto probe_by_hash = [&stub, &big_sha](std::chrono::milliseconds deadline) {
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + deadline);
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_sha256(big_sha);
      pdfv1::ProbeResponse response;
      return stub->Probe(&ctx, request, &response);
    };
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_data(big);
      request.mutable_document()->set_sha256(big_sha);
      pdfv1::ProbeResponse response;
      Check(stub->Probe(&ctx, request, &response).ok(),
            "16 MiB document uploaded and cached");
    }
    Check(FreezeWorker(front.pid), "large-request worker frozen");
    // One request holds the frozen worker (with the one copy a worker call
    // needs); eight more queue behind it.
    grpc::Status held;
    std::thread holder([&] { held = probe_by_hash(seconds(4)); });
    std::this_thread::sleep_for(milliseconds(500));
    const long rss_before = RssKib(front.pid);
    std::vector<std::thread> queued;
    for (int i = 0; i < 8; ++i) {
      queued.emplace_back([&] { probe_by_hash(seconds(3)); });
    }
    std::this_thread::sleep_for(milliseconds(1500));
    const long rss_queued = RssKib(front.pid);
    for (auto& t : queued) t.join();
    holder.join();
    Check(rss_before > 0 && rss_queued - rss_before < 2 * kDocumentKib,
          "requests queued by hash hold no copy of the document");
    Check(held.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED,
          "a large request to a frozen worker runs into its deadline");
    Check(ProbeWithin(stub.get(), fixture, seconds(20)).ok(),
          "the pool serves again after the abandoned large request");
  }
  StopFront(&front);

  // A one-second watchdog: a call that makes no progress has its worker
  // killed and ends DEADLINE_EXCEEDED although its client set no deadline,
  // and the pool recovers on its own.
  front = StartFront(argv[1], {{"GRPC_PDFIUM_WORKERS", "1"},
                               {"GRPC_PDFIUM_REQUEST_TIMEOUT_S", "1"}});
  if (front.port > 0) {
    auto stub = Dial(front.port);
    Check(FreezeWorker(front.pid), "watchdog front's worker frozen");
    const auto started = std::chrono::steady_clock::now();
    const grpc::Status stalled = ProbeWithin(stub.get(), fixture, milliseconds(0));
    const auto took = std::chrono::steady_clock::now() - started;
    Check(stalled.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED,
          "the watchdog ends a call whose worker makes no progress");
    Check(took >= seconds(1) && took < seconds(15),
          "the watchdog acts after its limit");
    Check(ProbeWithin(stub.get(), fixture, seconds(20)).ok(),
          "the watchdog's worker was replaced");

    // A client that stops reading: the front blocks writing a 600 DPI raster
    // (about 100 MB, more than any flow-control window) and the lease makes
    // no progress, so the watchdog cuts the call itself.
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(520 * 1024 * 1024);
    auto raster_stub = pdfv1::PdfBackendService::NewStub(grpc::CreateCustomChannel(
        "127.0.0.1:" + std::to_string(front.port),
        grpc::InsecureChannelCredentials(), args));
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(fixture);
    request.set_dpi(600.0);
    auto reader = raster_stub->Render(&ctx, request);
    std::this_thread::sleep_for(seconds(5));
    pdfv1::RenderResponse message;
    const bool read = reader->Read(&message);
    const grpc::StatusCode cut = reader->Finish().error_code();
    Check(!read && (cut == grpc::StatusCode::CANCELLED ||
                    cut == grpc::StatusCode::DEADLINE_EXCEEDED),
          "the watchdog cuts a call whose client stopped reading");
    Check(ProbeWithin(stub.get(), fixture, seconds(20)).ok(),
          "the pool serves again after the stalled client");
  }
  StopFront(&front);

  // A long document's first Parse: the page inventory loads all 100 pages
  // before the header goes out, several seconds that forward nothing. The
  // engine signals progress per page, so a one-second watchdog leaves the
  // worker alone and its inventory cache survives for the next Parse of
  // the same hash. Before, the watchdog killed the worker on every first
  // Parse, and with it the cache, so the document could never be parsed.
  {
    const std::string slow = ReadSibling(argv[2], "slow-inventory.pdf");
    Check(!slow.empty(), "slow-inventory fixture read");
    front = StartFront(argv[1], {{"GRPC_PDFIUM_WORKERS", "1"},
                                 {"GRPC_PDFIUM_REQUEST_TIMEOUT_S", "1"}});
    if (front.port > 0 && !slow.empty()) {
      auto stub = Dial(front.port);
      const std::string slow_sha = grpc_pdfium::Sha256Hex(slow);
      const TimedParse first = ParseLastPage(stub.get(), slow, slow_sha);
      Check(first.status.ok() && first.inventory == 100,
            "a page inventory longer than the watchdog limit completes");
      Check(first.took >= seconds(2),
            "the fixture's inventory outlasts the watchdog limit (else this "
            "check proves nothing)");
      const TimedParse second = ParseLastPage(stub.get(), "", slow_sha);
      Check(second.status.ok() && second.inventory == 100,
            "the next Parse by hash is served");
      Check(second.took * 2 < first.took,
            "the next Parse by hash reuses the worker's inventory");
    }
    StopFront(&front);
  }

  // A decompression bomb: about two kilobytes whose one attachment inflates
  // to 1 GiB, which PDFium decodes in full just to report its size (about
  // 2 GiB resident at the peak). A Parse that does not ask for attachment
  // data never decodes it and is served whole. One that does: under a 1 GiB
  // worker address-space limit the decode fails inside the worker, which
  // dies; the call ends UNAVAILABLE, no worker ever holds more than the
  // limit, and the pool serves again.
  {
    const std::string bomb = ReadSibling(argv[2], "attachment-bomb.pdf");
    Check(!bomb.empty(), "attachment-bomb fixture read");

    constexpr long kLimitKib = 1024 * 1024;
    front = StartFront(argv[1], {{"GRPC_PDFIUM_WORKERS", "1"},
                                 {"GRPC_PDFIUM_WORKER_MAX_BYTES", "1073741824"}});
    if (front.port > 0 && !bomb.empty()) {
      auto stub = Dial(front.port);
      {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(60));
        pdfv1::ParseRequest request;
        request.mutable_document()->set_data(bomb);
        auto reader = stub->Parse(&ctx, request);
        pdfv1::ParseResponse message;
        bool saw_trailer = false;
        int attachments = 0;
        bool sized = false;
        while (reader->Read(&message)) {
          if (message.has_trailer()) saw_trailer = true;
          if (message.has_attachment()) {
            ++attachments;
            sized = sized || message.attachment().has_size_bytes() ||
                    !message.attachment().data().empty();
          }
        }
        Check(reader->Finish().ok() && saw_trailer,
              "a default Parse of the bomb is served whole");
        Check(attachments == 1 && !sized,
              "the bomb's attachment is listed without decoding it");
      }
      // Sample the worker's peak resident size while the call runs: a
      // worker that survives keeps its peak, one that dies is read until
      // it goes.
      std::atomic<bool> done{false};
      long peak_kib = 0;
      const std::vector<pid_t> workers = WorkerPids(front.pid);
      Check(workers.size() == 1, "bomb front has one worker");
      std::thread sampler([&] {
        while (!done.load()) {
          for (pid_t pid : workers) peak_kib = std::max(peak_kib, PeakRssKib(pid));
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (pid_t pid : workers) peak_kib = std::max(peak_kib, PeakRssKib(pid));
      });
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(60));
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(bomb);
      request.add_families(pdfv1::PDF_FAMILY_ATTACHMENTS);
      request.mutable_options()->set_include_attachment_data(true);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      bool saw_trailer = false;
      while (reader->Read(&message)) {
        if (message.has_trailer()) saw_trailer = true;
      }
      const grpc::Status status = reader->Finish();
      done.store(true);
      sampler.join();
      Check(status.error_code() == grpc::StatusCode::UNAVAILABLE && !saw_trailer,
            "a decompression bomb costs its worker and answers UNAVAILABLE");
      Check(peak_kib > 0 && peak_kib < kLimitKib,
            "the bomb's worker stays under its address-space limit");
      Check(ProbeWithin(stub.get(), fixture, std::chrono::seconds(20)).ok(),
            "the pool serves again after the bomb");
    }
    StopFront(&front);
  }

  // A respawned worker outlives the gRPC thread that respawned it. The
  // worker's parent-death signal follows the thread that forked it, and the
  // synchronous server retires the threads that served a call, so a worker
  // forked on one was killed seconds after it came up and its slot failed
  // the next call. Kill the one worker so the next call respawns it, then
  // give the server time to retire threads and check it is still there.
  {
    front = StartFront(argv[1], {{"GRPC_PDFIUM_WORKERS", "1"}});
    if (front.port > 0) {
      auto stub = Dial(front.port);
      const std::vector<pid_t> before = WorkerPids(front.pid);
      Check(before.size() == 1 && kill(before[0], SIGKILL) == 0,
            "the one worker is killed");
      Check(ProbeWithin(stub.get(), fixture, std::chrono::seconds(20)).ok(),
            "the call after the kill is served by a respawned worker");
      for (int round = 0; round < 3; ++round) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Check(ProbeWithin(stub.get(), fixture, std::chrono::seconds(20)).ok(),
              "the respawned worker keeps serving");
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
      const std::vector<pid_t> after = WorkerPids(front.pid);
      Check(after.size() == 1 && after[0] != before[0] && Running(after[0]),
            "the respawned worker is still running after its thread retires");
      std::vector<pid_t> live = after;
      live.erase(std::remove_if(live.begin(), live.end(),
                                [](pid_t pid) { return !Running(pid); }),
                 live.end());
      Check(live == after, "no worker of the front is a zombie");
    }
    StopFront(&front);
  }

  if (failures == 0) {
    std::printf("pool_roundtrip: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "pool_roundtrip: %d check(s) failed\n", failures);
  return 1;
}
