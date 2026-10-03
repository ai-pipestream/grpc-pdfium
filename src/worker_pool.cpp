#include "worker_pool.h"

#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

WorkerPool::WorkerPool(std::string self_exe, std::string socket_dir, int size,
                       std::chrono::seconds stall_limit)
    : self_exe_(std::move(self_exe)),
      socket_dir_(std::move(socket_dir)),
      stall_limit_(stall_limit) {
  if (size < 1) size = 1;
  workers_.resize(static_cast<size_t>(size));
  for (int i = 0; i < size; ++i) {
    Worker& w = workers_[static_cast<size_t>(i)];
    w.socket_path = socket_dir_ + "/worker-" + std::to_string(i) + ".sock";
    Process process = Spawn(w.socket_path);
    if (process.pid < 0) throw std::runtime_error("fork failed for worker");
    w.pid = process.pid;
    w.channel = std::move(process.channel);
    w.stub = std::move(process.stub);
  }
  watchdog_ = std::thread([this] { Watch(); });
}

WorkerPool::~WorkerPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  stopping_cv_.notify_all();
  if (watchdog_.joinable()) watchdog_.join();
  for (auto& w : workers_) {
    if (w.pid > 0) {
      kill(w.pid, SIGTERM);
      waitpid(w.pid, nullptr, 0);
      unlink(w.socket_path.c_str());
    }
  }
}

WorkerPool::Process WorkerPool::Spawn(const std::string& socket_path) const {
  unlink(socket_path.c_str());
  Process process;
  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid == 0) {
    // Die with the front no matter how it goes down; an orphaned worker
    // would hold inherited descriptors (and a pool slot's socket) forever.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(0);
    execl(self_exe_.c_str(), self_exe_.c_str(), "--worker",
          socket_path.c_str(), static_cast<char*>(nullptr));
    // Only reached when exec fails. The front is multithreaded, so the
    // child may only make async-signal-safe calls: write(2), not stdio.
    static constexpr char kExecFailed[] = "grpc-pdfium: exec of a worker failed\n";
    ssize_t ignored = write(STDERR_FILENO, kExecFailed, sizeof(kExecFailed) - 1);
    (void)ignored;
    _exit(127);
  }
  process.pid = pid;
  grpc::ChannelArguments args;
  // Fleet message-size convention; a page raster at model DPI must fit.
  args.SetMaxReceiveMessageSize(520 * 1024 * 1024);
  args.SetMaxSendMessageSize(520 * 1024 * 1024);
  // The first connect usually beats the exec. gRPC's default reconnect
  // backoff (about a second) would hold every respawn that long; a local
  // socket can be retried quickly.
  args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 50);
  args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 50);
  args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
  process.channel = grpc::CreateCustomChannel(
      "unix://" + socket_path, grpc::InsecureChannelCredentials(), args);
  process.stub = pdfv1::PdfBackendService::NewStub(process.channel);
  // Wait for the worker socket to come up so the first request does not
  // race the exec; a worker that cannot start (or a failed fork) surfaces
  // on its first RPC as UNAVAILABLE and is respawned then.
  if (pid > 0) {
    process.channel->WaitForConnected(std::chrono::system_clock::now() +
                                      std::chrono::seconds(15));
  }
  return process;
}

WorkerPool::AcquireResult WorkerPool::Acquire(
    std::chrono::system_clock::time_point deadline, grpc::ServerContext* call,
    Lease* lease) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    for (size_t i = 0; i < workers_.size(); ++i) {
      Worker& w = workers_[i];
      if (w.busy) continue;
      w.busy = true;
      w.call = call;
      w.stall_deadline = std::chrono::steady_clock::now() + stall_limit_;
      w.abandoned = false;
      w.stalled = false;
      w.call_cancelled = false;
      *lease = Lease{static_cast<int>(i), w.stub.get()};
      return AcquireResult::kLeased;
    }
    if (call != nullptr && call->IsCancelled()) return AcquireResult::kCancelled;
    const auto now = std::chrono::system_clock::now();
    if (now >= deadline) return AcquireResult::kTimedOut;
    // A client going away signals nothing here, so look again at least
    // every 100 ms.
    available_.wait_until(lock,
                          std::min(deadline, now + std::chrono::milliseconds(100)));
  }
}

void WorkerPool::Touch(const Lease& lease) {
  std::lock_guard<std::mutex> lock(mutex_);
  workers_[static_cast<size_t>(lease.index)].stall_deadline =
      std::chrono::steady_clock::now() + stall_limit_;
}

bool WorkerPool::Release(const Lease& lease, bool failed) {
  std::unique_lock<std::mutex> lock(mutex_);
  Worker& w = workers_[static_cast<size_t>(lease.index)];
  const bool stalled = w.stalled;
  w.call = nullptr;
  const pid_t pid = w.pid;
  bool reaped = false;
  if (!failed && !stalled) {
    // Reap a worker that died on its own (crash on a hostile document)
    // without blocking; it is respawned like a failed one.
    reaped = pid > 0 && waitpid(pid, nullptr, WNOHANG) == pid;
    if (!reaped) {
      w.busy = false;
      lock.unlock();
      available_.notify_one();
      return false;
    }
  }
  // Respawn outside the lock: the kill, the reap, the fork and the wait for
  // the new socket can take seconds, and every other Acquire and Release
  // would stall behind them. The slot stays busy meanwhile, and with no
  // call on it the watchdog leaves it alone.
  w.pid = -1;
  const std::string socket_path = w.socket_path;
  lock.unlock();
  if (pid > 0 && !reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
  Process process = Spawn(socket_path);
  lock.lock();
  w.pid = process.pid;
  w.channel = std::move(process.channel);
  w.stub = std::move(process.stub);
  w.busy = false;
  lock.unlock();
  available_.notify_one();
  return stalled;
}

void WorkerPool::Watch() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    stopping_cv_.wait_for(lock, std::chrono::seconds(1));
    if (stopping_) break;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < workers_.size(); ++i) {
      Worker& w = workers_[i];
      if (w.call == nullptr) continue;
      if (w.stalled) {
        // Still leased a second after its worker was killed: the front is
        // blocked writing to a client that stopped reading. Cancelling the
        // call unblocks it.
        if (!w.call_cancelled && now >= w.stall_deadline) {
          w.call_cancelled = true;
          w.call->TryCancel();
        }
        continue;
      }
      // A call whose client went away normally ends at once and releases
      // its worker. One still leased a tick later is stuck on the worker
      // (a call blocked sending to a worker that stopped reading does not
      // end on cancellation), so the worker goes.
      if (w.call->IsCancelled() && !w.abandoned) {
        w.abandoned = true;
        continue;
      }
      const bool no_progress =
          stall_limit_.count() > 0 && now >= w.stall_deadline;
      if (!w.abandoned && !no_progress) continue;
      // Kill the worker: a front call reading from or writing to it fails
      // at once, and Release respawns it.
      w.stalled = true;
      w.stall_deadline = now + std::chrono::seconds(1);
      if (w.pid > 0) kill(w.pid, SIGKILL);
      if (w.abandoned) {
        std::cerr << "grpc-pdfium: worker " << i
                  << " still held after its client went away; killed it"
                  << std::endl;
      } else {
        std::cerr << "grpc-pdfium: worker " << i << " made no progress for "
                  << stall_limit_.count() << " s; killed it" << std::endl;
      }
    }
  }
}

}  // namespace grpc_pdfium
