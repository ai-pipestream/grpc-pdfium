#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "byte_cache.h"
#include "pdf_backend_service_impl.h"
#include "pdfium_engine.h"
#include "proxy_service_impl.h"
#include "worker_pool.h"

namespace {

// Fleet message-size convention (matches gRParse): big enough for a page
// raster at model DPI on any page size we have met.
constexpr int kMaxMessageBytes = 520 * 1024 * 1024;

// A worker's default address-space limit. PDFium decodes a stream whole
// (up to 1 GiB) even to report its size, so a few kilobytes of nested
// Flate can ask a worker for gigabytes; the limit makes such a document
// cost one worker, which dies on the failed allocation and is respawned,
// instead of the host's memory. 3 GiB leaves room for a 520 MiB document
// and a 512 MiB raster with their copies.
constexpr uint64_t kDefaultWorkerMaxBytes = 3ULL << 30;
// Below this a worker cannot start its gRPC server and PDFium, so a smaller
// nonzero limit is refused rather than leaving the pool respawning workers
// that never come up.
constexpr uint64_t kMinWorkerMaxBytes = 256ULL << 20;

// GRPC_PDFIUM_WORKER_MAX_BYTES, the worker address-space limit in bytes (0
// turns it off; otherwise at least kMinWorkerMaxBytes). The front checks it at startup too, so a malformed value
// stops the service there rather than in every worker it spawns.
bool WorkerMaxBytesFromEnv(uint64_t* max_bytes) {
  *max_bytes = kDefaultWorkerMaxBytes;
  const char* env = std::getenv("GRPC_PDFIUM_WORKER_MAX_BYTES");
  if (env == nullptr) return true;
  char* end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(env, &end, 10);
  if (*env == '\0' || *env == '-' || *end != '\0' || errno == ERANGE) {
    std::cerr << "GRPC_PDFIUM_WORKER_MAX_BYTES is not a byte count: " << env
              << std::endl;
    return false;
  }
  if (value != 0 && value < kMinWorkerMaxBytes) {
    std::cerr << "GRPC_PDFIUM_WORKER_MAX_BYTES " << value
              << " is below the " << kMinWorkerMaxBytes
              << "-byte minimum (0 turns the limit off)" << std::endl;
    return false;
  }
  *max_bytes = value;
  return true;
}

// Caps the worker's address space and turns off core dumps, which a capped
// worker dying on a bomb would otherwise write at the size of its address
// space. Returns false, having said why, when the kernel refuses.
bool LimitWorkerMemory(uint64_t max_bytes) {
  const rlimit no_core{0, 0};
  if (setrlimit(RLIMIT_CORE, &no_core) != 0) {
    std::cerr << "worker setrlimit(RLIMIT_CORE): " << std::strerror(errno)
              << std::endl;
    return false;
  }
  if (max_bytes == 0) return true;
  const rlimit address_space{static_cast<rlim_t>(max_bytes),
                             static_cast<rlim_t>(max_bytes)};
  if (setrlimit(RLIMIT_AS, &address_space) != 0) {
    std::cerr << "worker setrlimit(RLIMIT_AS, " << max_bytes
              << "): " << std::strerror(errno) << std::endl;
    return false;
  }
  return true;
}

// Tells the front's watchdog the engine is still working through a phase
// that streams nothing (the page inventory), over the worker's end of the
// progress socket pair. One signal per quarter second is plenty for a
// watchdog that looks once a second. A full socket buffer (EAGAIN) means
// signals are already waiting, so that is not a failure; any other error is
// reported once, since the next signal would only repeat it.
void SignalProgress(int fd) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last_sent;
  static bool reported = false;
  const Clock::time_point now = Clock::now();
  if (now - last_sent < std::chrono::milliseconds(250)) return;
  last_sent = now;
  const char beat = 1;
  if (send(fd, &beat, 1, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN &&
      errno != EWOULDBLOCK && !reported) {
    reported = true;
    std::cerr << "worker progress signal failed: " << std::strerror(errno)
              << std::endl;
  }
}

// The descriptor after --progress-fd, or -1 (having said why) when it is
// not one.
int ProgressFdFromArg(const char* arg) {
  char* end = nullptr;
  errno = 0;
  const long fd = std::strtol(arg, &end, 10);
  if (*arg == '\0' || *end != '\0' || errno == ERANGE || fd < 0 ||
      fd > INT32_MAX) {
    std::cerr << "--progress-fd is not a descriptor: " << arg << std::endl;
    return -1;
  }
  return static_cast<int>(fd);
}

// progress_fd is the worker's end of the front's progress socket pair, or
// -1 when the front could not make one.
int RunWorker(const std::string& socket_path, int progress_fd) {
  uint64_t max_bytes = 0;
  if (!WorkerMaxBytesFromEnv(&max_bytes) || !LimitWorkerMemory(max_bytes)) {
    return 1;
  }
  grpc_pdfium::PdfiumEngine::InitProcess();
  if (progress_fd >= 0) {
    grpc_pdfium::PdfiumEngine::SetProgressHook(
        [progress_fd] { SignalProgress(progress_fd); });
  }
  grpc_pdfium::PdfBackendServiceImpl service;
  grpc::ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
  builder.SetMaxSendMessageSize(kMaxMessageBytes);
  builder.AddListeningPort("unix://" + socket_path,
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    std::cerr << "worker failed to bind " << socket_path << std::endl;
    return 1;
  }
  server->Wait();
  return 0;
}

// The longest limit a seconds setting accepts. Far above any sane watchdog
// or queue wait, and far below the point where adding it to a clock's
// now() would overflow the clock's nanosecond count (about 292 years) and
// put the deadline in the past, which would cut every call at once.
constexpr long long kMaxLimitSeconds = 7LL * 24 * 3600;

// A whole number of seconds from the environment, fallback_s when unset.
// A value that is not a count of seconds, or above kMaxLimitSeconds, is
// refused (nullopt, having said why) rather than replaced.
std::optional<std::chrono::seconds> SecondsFromEnv(const char* name,
                                                   long long fallback_s) {
  const char* env = std::getenv(name);
  if (env == nullptr) return std::chrono::seconds(fallback_s);
  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(env, &end, 10);
  if (*env == '\0' || *end != '\0' || errno == ERANGE || value < 0 ||
      value > kMaxLimitSeconds) {
    std::cerr << name << " must be a whole number of seconds from 0 (off) to "
              << kMaxLimitSeconds << ", got \"" << env << "\"" << std::endl;
    return std::nullopt;
  }
  return std::chrono::seconds(value);
}

std::string SelfExe() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  buf[n] = '\0';
  return buf;
}

int RunFront() {
  const char* port_env = std::getenv("GRPC_PDFIUM_PORT");
  // Fleet default port 50069 (the old 50051 collides with gRParse).
  const std::string address =
      std::string("0.0.0.0:") + (port_env != nullptr ? port_env : "50069");

  int pool_size = 4;
  if (const char* workers_env = std::getenv("GRPC_PDFIUM_WORKERS")) {
    pool_size = std::max(1, std::atoi(workers_env));
  }

  // The content-addressed byte cache bounds: how many documents, and their
  // summed size. GRPC_PDFIUM_CACHE_MAX_DOCUMENTS=0 disables the cache.
  size_t cache_max_documents = 8;
  if (const char* env = std::getenv("GRPC_PDFIUM_CACHE_MAX_DOCUMENTS")) {
    cache_max_documents = std::strtoull(env, nullptr, 10);
  }
  size_t cache_max_bytes = 2ULL << 30;
  if (const char* env = std::getenv("GRPC_PDFIUM_CACHE_MAX_BYTES")) {
    cache_max_bytes = std::strtoull(env, nullptr, 10);
  }

  // How long a worker call may go without forwarding a message before the
  // watchdog kills its worker, and how long a request may wait for a free
  // worker. 0 turns either limit off (a request then waits as long as its
  // client does).
  const std::optional<std::chrono::seconds> request_timeout =
      SecondsFromEnv("GRPC_PDFIUM_REQUEST_TIMEOUT_S", 300);
  const std::optional<std::chrono::seconds> queue_timeout =
      SecondsFromEnv("GRPC_PDFIUM_QUEUE_TIMEOUT_S", 300);
  if (!request_timeout || !queue_timeout) return 1;

  // Workers read the limit themselves; checked here so a bad value fails
  // the start, not every spawn.
  uint64_t worker_max_bytes = 0;
  if (!WorkerMaxBytesFromEnv(&worker_max_bytes)) return 1;

  std::string socket_dir_template = "/tmp/grpc-pdfium-XXXXXX";
  char* socket_dir = mkdtemp(socket_dir_template.data());
  if (socket_dir == nullptr) {
    std::cerr << "failed to create the worker socket directory" << std::endl;
    return 1;
  }

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc_pdfium::WorkerPool pool(SelfExe(), socket_dir, pool_size,
                               *request_timeout);
  grpc_pdfium::ByteCache byte_cache(cache_max_documents, cache_max_bytes);
  grpc_pdfium::ProxyServiceImpl service(&pool, &byte_cache, *queue_timeout);
  grpc::ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
  builder.SetMaxSendMessageSize(kMaxMessageBytes);
  int selected_port = 0;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(),
                           &selected_port);
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || selected_port == 0) {
    std::cerr << "failed to bind " << address << std::endl;
    return 1;
  }
  // GRPC_PDFIUM_PORT=0 asks for an ephemeral port; the printed address is
  // the bound one either way, and tests parse it from this line.
  std::cout << "grpc-pdfium listening on 0.0.0.0:" << selected_port << " ("
            << grpc_pdfium::PdfiumEngine::EngineVersion() << ", " << pool_size
            << " workers)" << std::endl;
  server->Wait();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--worker") {
    return RunWorker(argv[2], -1);
  }
  if (argc == 5 && std::string(argv[1]) == "--worker" &&
      std::string(argv[3]) == "--progress-fd") {
    const int progress_fd = ProgressFdFromArg(argv[4]);
    if (progress_fd < 0) return 1;
    return RunWorker(argv[2], progress_fd);
  }
  return RunFront();
}
