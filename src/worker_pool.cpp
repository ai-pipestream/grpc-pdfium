#include "worker_pool.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

// The child side of a spawn: execs the worker, handing it its end of the
// progress socket pair when there is one. The front is multithreaded, so
// only async-signal-safe calls are made here (fcntl, execl, write, _exit).
[[noreturn]] void ExecWorker(const char* exe, const char* socket_path,
                             int progress_fd, const char* progress_arg) {
  // Both ends were made close-on-exec, so a worker spawned by another
  // thread meanwhile inherits neither; this child keeps its own end.
  if (progress_fd >= 0 && fcntl(progress_fd, F_SETFD, 0) == 0) {
    execl(exe, exe, "--worker", socket_path, "--progress-fd", progress_arg,
          static_cast<char*>(nullptr));
  } else {
    execl(exe, exe, "--worker", socket_path, static_cast<char*>(nullptr));
  }
  // Only reached when exec fails: write(2), not stdio.
  static constexpr char kExecFailed[] = "grpc-pdfium: exec of a worker failed\n";
  ssize_t ignored = write(STDERR_FILENO, kExecFailed, sizeof(kExecFailed) - 1);
  (void)ignored;
  _exit(127);
}

// Reads every progress signal waiting on fd; true when there was one. The
// socket is never blocked on: an empty queue ends the read (EAGAIN).
bool DrainProgress(int fd) {
  if (fd < 0) return false;
  bool any = false;
  char beat = 0;
  while (recv(fd, &beat, 1, MSG_DONTWAIT) > 0) any = true;
  return any;
}

}  // namespace

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
    w.progress_fd = process.progress_fd;
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
    if (w.progress_fd >= 0) close(w.progress_fd);
  }
}

WorkerPool::Process WorkerPool::Spawn(const std::string& socket_path) const {
  unlink(socket_path.c_str());
  Process process;
  // The worker's progress channel: pair[0] stays here, pair[1] goes to the
  // worker. Without it the worker still serves, but a long page inventory
  // counts against the stall limit as it did before the channel existed.
  int pair[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, pair) != 0) {
    std::cerr << "grpc-pdfium: worker progress socket pair failed: "
              << std::strerror(errno) << std::endl;
    pair[0] = pair[1] = -1;
  }
  // Formatted before the fork: the child may not allocate.
  const std::string progress_arg = std::to_string(pair[1]);
  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid == 0) {
    // Die with the front no matter how it goes down; an orphaned worker
    // would hold inherited descriptors (and a pool slot's socket) forever.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(0);
    ExecWorker(self_exe_.c_str(), socket_path.c_str(), pair[1],
               progress_arg.c_str());
  }
  if (pair[1] >= 0) close(pair[1]);
  process.progress_fd = pair[0];
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
      // Signals left from an earlier lease are not this lease's progress.
      DrainProgress(w.progress_fd);
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
  const int progress_fd = w.progress_fd;
  w.progress_fd = -1;
  const std::string socket_path = w.socket_path;
  lock.unlock();
  if (pid > 0 && !reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
  if (progress_fd >= 0) close(progress_fd);
  Process process = Spawn(socket_path);
  lock.lock();
  w.pid = process.pid;
  w.channel = std::move(process.channel);
  w.stub = std::move(process.stub);
  w.progress_fd = process.progress_fd;
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
      // The engine signalled progress in a phase that forwards nothing.
      if (DrainProgress(w.progress_fd)) w.stall_deadline = now + stall_limit_;
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
