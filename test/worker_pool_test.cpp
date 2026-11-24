// Pool startup test: a pool whose workers cannot start must fail to
// construct, quickly and with a reason, instead of coming up with dead
// slots that fail every request later. /bin/false stands in for a worker
// binary that exits before serving its socket (a failed exec, a library
// that does not load, an address-space limit too small for the host).

#include <stdlib.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "worker_pool.h"

int main() {
  int failures = 0;
  auto check = [&failures](bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  };

  char socket_dir[] = "/tmp/grpc-pdfium-pool-test-XXXXXX";
  check(mkdtemp(socket_dir) != nullptr, "socket directory created");

  const auto started = std::chrono::steady_clock::now();
  bool threw = false;
  std::string reason;
  try {
    grpc_pdfium::WorkerPool pool("/bin/false", socket_dir, 1, 1,
                                 std::chrono::seconds(300));
  } catch (const std::exception& e) {
    threw = true;
    reason = e.what();
  }
  const auto took = std::chrono::steady_clock::now() - started;
  check(threw, "a pool whose first worker exits at once fails to construct");
  check(reason.find("text") != std::string::npos,
        "the failure names the worker that did not start");
  check(took < std::chrono::seconds(10),
        "a worker that exited is noticed without waiting out the connect limit");

  if (failures == 0) {
    std::printf("worker_pool: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "worker_pool: %d check(s) failed\n", failures);
  return 1;
}
