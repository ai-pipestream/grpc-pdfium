#pragma once

#include <sys/types.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "worker_role.h"

namespace grpc_pdfium {

// A pool of single-threaded worker processes, each serving the contract on
// its own unix socket. This is the deployment shape PDFium's global-state
// rule demands: concurrency across processes, one request per process at a
// time, and a crash on a hostile document kills one worker, not the
// service.
//
// The pool is split by role (worker_role.h): text workers serve Probe and
// Parse and never render, render workers serve Render. Rendering leaves
// PDFium's substitute font state changed, and text extraction measured
// after it in the same process differed from a clean one; a text worker
// never sees a render, so its output depends only on the request. Each
// role has its own slots and its own wait for a free one.
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

  // Spawns text_size text workers and render_size render workers (each at
  // least one) running self_exe --worker <socket> --role <role>. Sockets
  // live under socket_dir. A stall_limit of zero turns the stall check off.
  WorkerPool(std::string self_exe, std::string socket_dir, int text_size,
             int render_size, std::chrono::seconds stall_limit);
  ~WorkerPool();

  // Waits for a free worker of role (kText or kRender) until deadline, and gives up as soon as call is
  // cancelled (its client went away or its deadline passed). From here
  // until Release the watchdog watches the lease on call's behalf.
  AcquireResult Acquire(WorkerRole role,
                        std::chrono::system_clock::time_point deadline,
                        grpc::ServerContext* call, Lease* lease);

  // Records progress on a lease (a message was forwarded), which restarts
  // the watchdog's clock.
  void Touch(const Lease& lease);

  // Returns the worker to the pool. When failed is true, or the watchdog cut
  // the lease, the worker process is killed and respawned first (its engine
  // state is suspect). Returns true when the watchdog cut the lease.
  bool Release(const Lease& lease, bool failed);

  // The number of workers of role.
  int size(WorkerRole role) const;
  std::chrono::seconds stall_limit() const { return stall_limit_; }

 private:
  using Stub = ai::protomolt::parse::pdf::v1::PdfBackendService::Stub;

  struct Worker {
    WorkerRole role = WorkerRole::kText;
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
  // Touches no shared state but the forker's queue, so a respawn runs it
  // outside the lock.
  Process Spawn(const std::string& socket_path, WorkerRole role);

  // Runs fork_child on the forker thread and returns what it returned.
  //
  // A worker asks for SIGKILL when its parent goes (PR_SET_PDEATHSIG), and
  // Linux sends that signal when the thread that forked it exits, not the
  // process. A respawn runs on whichever gRPC thread released the lease, and
  // gRPC's synchronous server retires idle threads, so a worker forked there
  // was killed a moment after it came up and its slot failed the next call
  // sent to it. Every fork therefore happens on one thread that lives as
  // long as the pool.
  pid_t ForkOnForker(const std::function<pid_t()>& fork_child);

  // The forker thread's loop.
  void Fork();

  // The watchdog thread's loop.
  void Watch();

  const std::string self_exe_;
  const std::string socket_dir_;
  const std::chrono::seconds stall_limit_;
  std::mutex mutex_;
  // Waiters for both roles share it, so a freed slot wakes them all (one
  // woken waiter could be of the other role).
  std::condition_variable available_;
  std::condition_variable stopping_cv_;
  bool stopping_ = false;
  std::vector<Worker> workers_;
  std::thread watchdog_;
  // The forks waiting for the forker thread, and the thread itself, which
  // starts before the first worker and stops after the last one is reaped.
  std::mutex fork_mutex_;
  std::condition_variable fork_cv_;
  std::deque<std::function<void()>> fork_jobs_;
  bool fork_stopping_ = false;
  std::thread forker_;
};

}  // namespace grpc_pdfium
