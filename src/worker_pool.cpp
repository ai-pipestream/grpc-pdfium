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
#include <future>
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
                             const char* role, int progress_fd,
                             const char* progress_arg) {
  // Both ends were made close-on-exec, so a worker spawned by another
  // thread meanwhile inherits neither; this child keeps its own end.
  if (progress_fd >= 0 && fcntl(progress_fd, F_SETFD, 0) == 0) {
    execl(exe, exe, "--worker", socket_path, "--progress-fd", progress_arg,
          "--role", role, static_cast<char*>(nullptr));
  } else {
    execl(exe, exe, "--worker", socket_path, "--role", role,
          static_cast<char*>(nullptr));
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

// How long a worker has to answer on its socket after it was started.
constexpr std::chrono::seconds kConnectLimit(15);
// The least time between two starts of the same slot by the watchdog, so
// a worker that cannot start costs one fork every couple of seconds.
constexpr std::chrono::seconds kRestartPause(2);

}  // namespace

WorkerPool::WorkerPool(std::string self_exe, std::string socket_dir,
                       int text_size, int render_size,
                       std::chrono::seconds stall_limit)
    : self_exe_(std::move(self_exe)),
      socket_dir_(std::move(socket_dir)),
      stall_limit_(stall_limit) {
  forker_ = std::thread([this] { Fork(); });
  text_size = std::max(text_size, 1);
  render_size = std::max(render_size, 1);
  workers_.resize(static_cast<size_t>(text_size + render_size));
  for (int i = 0; i < text_size + render_size; ++i) {
    Worker& w = workers_[static_cast<size_t>(i)];
    w.role = i < text_size ? WorkerRole::kText : WorkerRole::kRender;
    w.socket_path = socket_dir_ + "/" + WorkerRoleName(w.role) + "-" +
                    std::to_string(i) + ".sock";
    Process process = Spawn(w.socket_path, w.role);
    w.pid = process.pid;
    w.channel = std::move(process.channel);
    w.stub = std::move(process.stub);
    w.progress_fd = process.progress_fd;
    w.up = process.connected;
    if (!process.connected) {
      // A worker that does not come up now would fail every request sent
      // to it later, behind a front that looks healthy. The destructor
      // does not run for a constructor that throws, so what was started is
      // stopped here; a joinable thread member would terminate.
      StopStartedWorkers();
      throw std::runtime_error(std::string(WorkerRoleName(w.role)) +
                               " worker " + std::to_string(i) + " " +
                               process.failure);
    }
  }
  published_serving_ = ServingLocked();
  watchdog_ = std::thread([this] { Watch(); });
}

void WorkerPool::StopStartedWorkers() {
  for (auto& w : workers_) {
    if (w.pid > 0) {
      kill(w.pid, SIGKILL);
      waitpid(w.pid, nullptr, 0);
    }
    if (!w.socket_path.empty()) unlink(w.socket_path.c_str());
    if (w.progress_fd >= 0) close(w.progress_fd);
  }
  {
    std::lock_guard<std::mutex> lock(fork_mutex_);
    fork_stopping_ = true;
  }
  fork_cv_.notify_all();
  forker_.join();
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
  {
    std::lock_guard<std::mutex> lock(fork_mutex_);
    fork_stopping_ = true;
  }
  fork_cv_.notify_all();
  if (forker_.joinable()) forker_.join();
}

void WorkerPool::Fork() {
  std::unique_lock<std::mutex> lock(fork_mutex_);
  for (;;) {
    fork_cv_.wait(lock, [this] { return fork_stopping_ || !fork_jobs_.empty(); });
    if (fork_jobs_.empty()) return;
    std::function<void()> job = std::move(fork_jobs_.front());
    fork_jobs_.pop_front();
    lock.unlock();
    job();
    lock.lock();
  }
}

pid_t WorkerPool::ForkOnForker(const std::function<pid_t()>& fork_child) {
  std::packaged_task<pid_t()> task(fork_child);
  std::future<pid_t> pid = task.get_future();
  {
    std::lock_guard<std::mutex> lock(fork_mutex_);
    fork_jobs_.emplace_back([&task] { task(); });
  }
  fork_cv_.notify_one();
  return pid.get();
}

WorkerPool::Process WorkerPool::Start(const std::string& socket_path,
                                      WorkerRole role) {
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
  const pid_t parent = getpid();
  const pid_t pid = ForkOnForker([&] {
    const pid_t child = fork();
    if (child == 0) {
      // Die with the front no matter how it goes down; an orphaned worker
      // would hold inherited descriptors (and a pool slot's socket)
      // forever. The signal is tied to the forking thread, which is why
      // this runs on the forker (see ForkOnForker).
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent) _exit(0);
      ExecWorker(self_exe_.c_str(), socket_path.c_str(), WorkerRoleName(role),
                 pair[1], progress_arg.c_str());
    }
    return child;
  });
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
  if (pid < 0) process.failure = "could not be forked";
  return process;
}

WorkerPool::Process WorkerPool::Spawn(const std::string& socket_path,
                                      WorkerRole role) {
  Process process = Start(socket_path, role);
  if (process.pid < 0) return process;
  // Wait for the worker socket to come up so the first request does not
  // race the exec. The wait is taken in short steps: a worker that exits
  // before it serves (a failed exec, a library that does not load, a
  // socket it cannot bind) is reaped and reported at once.
  const auto deadline = std::chrono::system_clock::now() + kConnectLimit;
  for (;;) {
    const auto now = std::chrono::system_clock::now();
    if (process.channel->WaitForConnected(
            std::min(deadline, now + std::chrono::milliseconds(100)))) {
      process.connected = true;
      return process;
    }
    int status = 0;
    if (waitpid(process.pid, &status, WNOHANG) == process.pid) {
      process.pid = -1;
      process.failure = "exited before it served its socket";
      if (WIFEXITED(status)) {
        process.failure += " (exit status " +
                           std::to_string(WEXITSTATUS(status)) + ")";
      } else if (WIFSIGNALED(status)) {
        process.failure += " (signal " + std::to_string(WTERMSIG(status)) + ")";
      }
      return process;
    }
    if (std::chrono::system_clock::now() >= deadline) {
      process.failure = "did not serve its socket within " +
                        std::to_string(kConnectLimit.count()) + " s";
      return process;
    }
  }
}

bool WorkerPool::ServingLocked() const {
  bool text = false;
  bool render = false;
  for (const Worker& w : workers_) {
    if (!w.up) continue;
    if (w.role == WorkerRole::kText) text = true;
    if (w.role == WorkerRole::kRender) render = true;
  }
  return text && render;
}

bool WorkerPool::Serving() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ServingLocked();
}

void WorkerPool::SetHealthListener(std::function<void(bool)> listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  health_listener_ = std::move(listener);
  published_serving_ = ServingLocked();
  if (health_listener_) health_listener_(published_serving_);
}

void WorkerPool::PublishHealthLocked() {
  const bool serving = ServingLocked();
  if (serving == published_serving_) return;
  published_serving_ = serving;
  std::cerr << "grpc-pdfium: "
            << (serving ? "every role has a worker again; health is SERVING"
                        : "a role has no worker up; health is NOT_SERVING")
            << std::endl;
  if (health_listener_) health_listener_(serving);
}

int WorkerPool::size(WorkerRole role) const {
  return static_cast<int>(std::ranges::count_if(
      workers_, [role](const Worker& w) { return w.role == role; }));
}

WorkerPool::AcquireResult WorkerPool::Acquire(
    WorkerRole role, std::chrono::system_clock::time_point deadline,
    grpc::ServerContext* call, Lease* lease) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    // A free worker that is up is taken first. One that is not up (dead or
    // still starting) is leased only when the role has no worker up at
    // all: the call then fails fast on it, as it did before the pool
    // tracked this, instead of waiting out the queue limit.
    bool role_has_up = false;
    size_t pick = workers_.size();
    for (size_t i = 0; i < workers_.size(); ++i) {
      const Worker& w = workers_[i];
      if (w.role != role) continue;
      role_has_up = role_has_up || w.up;
      if (w.busy) continue;
      if (w.up) {
        pick = i;
        break;
      }
      if (pick == workers_.size()) pick = i;
    }
    if (pick < workers_.size() && !workers_[pick].up && role_has_up) {
      pick = workers_.size();
    }
    if (pick < workers_.size()) {
      const size_t i = pick;
      Worker& w = workers_[i];
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
      // The call went through, so a worker leased while it was still
      // coming up is up now.
      if (pid > 0 && !w.up) {
        w.up = true;
        PublishHealthLocked();
      }
      w.busy = false;
      lock.unlock();
      available_.notify_all();
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
  const WorkerRole role = w.role;
  lock.unlock();
  if (pid > 0 && !reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
  if (progress_fd >= 0) close(progress_fd);
  Process process = Spawn(socket_path, role);
  if (!process.connected) {
    std::cerr << "grpc-pdfium: respawned worker " << lease.index << " "
              << process.failure << std::endl;
  }
  lock.lock();
  w.pid = process.pid;
  w.channel = std::move(process.channel);
  w.stub = std::move(process.stub);
  w.progress_fd = process.progress_fd;
  // A worker that did not come up is the watchdog's from here: it has had
  // its connect limit, so the watchdog replaces it on its next look.
  w.up = process.connected;
  const auto now = std::chrono::steady_clock::now();
  w.give_up = now;
  w.next_start = now;
  w.busy = false;
  PublishHealthLocked();
  lock.unlock();
  available_.notify_all();
  return stalled;
}

std::vector<size_t> WorkerPool::CheckIdleWorkers(
    std::chrono::steady_clock::time_point now) {
  std::vector<size_t> restart;
  for (size_t i = 0; i < workers_.size(); ++i) {
    Worker& w = workers_[i];
    if (w.busy) continue;
    if (w.pid > 0 && waitpid(w.pid, nullptr, WNOHANG) == w.pid) {
      w.pid = -1;
      if (w.up) {
        std::cerr << "grpc-pdfium: worker " << i << " died while idle"
                  << std::endl;
      }
      w.up = false;
    }
    if (w.up) continue;
    if (w.pid > 0) {
      if (w.channel->GetState(true) == GRPC_CHANNEL_READY) {
        w.up = true;
        std::cerr << "grpc-pdfium: worker " << i << " is up again" << std::endl;
        continue;
      }
      // Still coming up.
      if (now < w.give_up) continue;
    }
    if (now < w.next_start) continue;
    w.busy = true;
    restart.push_back(i);
  }
  return restart;
}

void WorkerPool::Watch() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    stopping_cv_.wait_for(lock, std::chrono::seconds(1));
    if (stopping_) break;
    const auto now = std::chrono::steady_clock::now();
    // Workers no call holds: one that died is reaped here, so the health
    // status drops as soon as a role has none left, and is started again
    // outside the lock. The start does not wait for the socket; a later
    // look marks the worker up once it answers.
    const std::vector<size_t> restart = CheckIdleWorkers(now);
    PublishHealthLocked();
    for (const size_t i : restart) {
      Worker& w = workers_[i];
      const pid_t old_pid = w.pid;
      w.pid = -1;
      const int old_progress_fd = w.progress_fd;
      w.progress_fd = -1;
      const std::string socket_path = w.socket_path;
      const WorkerRole role = w.role;
      lock.unlock();
      if (old_pid > 0) {
        kill(old_pid, SIGKILL);
        waitpid(old_pid, nullptr, 0);
      }
      if (old_progress_fd >= 0) close(old_progress_fd);
      Process process = Start(socket_path, role);
      lock.lock();
      w.pid = process.pid;
      w.channel = std::move(process.channel);
      w.stub = std::move(process.stub);
      w.progress_fd = process.progress_fd;
      w.up = false;
      w.give_up = now + kConnectLimit;
      w.next_start = now + kRestartPause;
      w.busy = false;
    }
    if (!restart.empty()) available_.notify_all();
    if (stopping_) break;
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
