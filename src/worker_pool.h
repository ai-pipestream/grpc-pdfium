#pragma once

#include <sys/types.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace grpc_pdfium {

// A pool of single-threaded worker processes, each serving the contract on
// its own unix socket. This is the deployment shape PDFium's global-state
// rule demands: concurrency across processes, one request per process at a
// time, and a crash on a hostile document kills one worker, not the
// service.
//
// A watchdog bounds every lease. A call that forwards nothing for the stall
// limit, or that is still leased a second after its client went away, has
// its worker killed, which ends a front call stuck on it; if the lease is
// still held a second later, the front is stuck writing to a client that
// stopped reading, and the client's call is cancelled. Either way the slot
// comes back respawned, so no document can hold a worker for good.
//
// Progress is a forwarded message, or a progress signal from the worker
// itself: each worker gets one end of a datagram socket pair (its
// --progress-fd), and the engine signals on it while it works through a
// phase that streams nothing, the page inventory a document's first Parse
// builds by loading every page. A long document's inventory therefore runs
// as long as its pages keep loading, and only a page that stops the engine
// for the stall limit costs the worker (and the inventory cache it holds).
class WorkerPool {
 public:
  struct Lease {
    int index = -1;
    ai::protomolt::parse::pdf::v1::PdfBackendService::Stub* stub = nullptr;
  };

  enum class AcquireResult { kLeased, kTimedOut, kCancelled };

  // Spawns size workers running self_exe --worker <socket>. Sockets live
  // under socket_dir. A stall_limit of zero turns the stall check off.
  WorkerPool(std::string self_exe, std::string socket_dir, int size,
             std::chrono::seconds stall_limit);
  ~WorkerPool();

  // Waits for a free worker until deadline, and gives up as soon as call is
  // cancelled (its client went away or its deadline passed). From here
  // until Release the watchdog watches the lease on call's behalf.
  AcquireResult Acquire(std::chrono::system_clock::time_point deadline,
                        grpc::ServerContext* call, Lease* lease);

  // Records progress on a lease (a message was forwarded), which restarts
  // the watchdog's clock.
  void Touch(const Lease& lease);

  // Returns the worker to the pool. When failed is true, or the watchdog cut
  // the lease, the worker process is killed and respawned first (its engine
  // state is suspect). Returns true when the watchdog cut the lease.
  bool Release(const Lease& lease, bool failed);

  int size() const { return static_cast<int>(workers_.size()); }
  std::chrono::seconds stall_limit() const { return stall_limit_; }

 private:
  using Stub = ai::protomolt::parse::pdf::v1::PdfBackendService::Stub;

  struct Worker {
    pid_t pid = -1;
    std::string socket_path;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<Stub> stub;
    // The front's end of the worker's progress socket pair; -1 when the
    // pair could not be made (the worker then runs without signalling).
    int progress_fd = -1;
    // Leased, or being respawned.
    bool busy = false;
    // The call holding the lease; null while the slot is free or respawning,
    // which is what keeps the watchdog away from it.
    grpc::ServerContext* call = nullptr;
    // When the watchdog acts next unless the lease makes progress first.
    std::chrono::steady_clock::time_point stall_deadline;
    // The watchdog saw the call's client gone while the lease was held.
    bool abandoned = false;
    // The watchdog killed this lease's worker.
    bool stalled = false;
    // The watchdog cancelled this lease's call.
    bool call_cancelled = false;
  };

  // A started worker process and its channel.
  struct Process {
    pid_t pid = -1;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<Stub> stub;
    int progress_fd = -1;
  };

  // Starts one worker on socket_path and waits for its socket to come up.
  // Touches no shared state, so a respawn runs it outside the lock.
  Process Spawn(const std::string& socket_path) const;

  // The watchdog thread's loop.
  void Watch();

  const std::string self_exe_;
  const std::string socket_dir_;
  const std::chrono::seconds stall_limit_;
  std::mutex mutex_;
  std::condition_variable available_;
  std::condition_variable stopping_cv_;
  bool stopping_ = false;
  std::vector<Worker> workers_;
  std::thread watchdog_;
};

}  // namespace grpc_pdfium
