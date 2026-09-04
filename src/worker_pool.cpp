#include "worker_pool.h"

#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

WorkerPool::WorkerPool(std::string self_exe, std::string socket_dir, int size)
    : self_exe_(std::move(self_exe)), socket_dir_(std::move(socket_dir)) {
  if (size < 1) size = 1;
  workers_.resize(static_cast<size_t>(size));
  for (int i = 0; i < size; ++i) Spawn(i);
}

WorkerPool::~WorkerPool() {
  for (auto& w : workers_) {
    if (w.pid > 0) {
      kill(w.pid, SIGTERM);
      waitpid(w.pid, nullptr, 0);
      unlink(w.socket_path.c_str());
    }
  }
}

void WorkerPool::Spawn(int index) {
  Worker& w = workers_[static_cast<size_t>(index)];
  w.socket_path = socket_dir_ + "/worker-" + std::to_string(index) + ".sock";
  unlink(w.socket_path.c_str());

  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid < 0) throw std::runtime_error("fork failed for worker");
  if (pid == 0) {
    // Die with the front no matter how it goes down; an orphaned worker
    // would hold inherited descriptors (and a pool slot's socket) forever.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(0);
    execl(self_exe_.c_str(), self_exe_.c_str(), "--worker",
          w.socket_path.c_str(), static_cast<char*>(nullptr));
    // Only reached when exec fails.
    std::perror("execl worker");
    _exit(127);
  }
  w.pid = pid;
  grpc::ChannelArguments args;
  // Fleet message-size convention; a page raster at model DPI must fit.
  args.SetMaxReceiveMessageSize(520 * 1024 * 1024);
  args.SetMaxSendMessageSize(520 * 1024 * 1024);
  w.channel = grpc::CreateCustomChannel("unix://" + w.socket_path,
                                        grpc::InsecureChannelCredentials(), args);
  w.stub = pdfv1::PdfBackendService::NewStub(w.channel);
  // Wait for the worker socket to come up so the first request does not
  // race the exec; a worker that cannot start within the deadline surfaces
  // on its first RPC as UNAVAILABLE and is respawned then.
  w.channel->WaitForConnected(std::chrono::system_clock::now() +
                              std::chrono::seconds(15));
  w.busy = false;
}

WorkerPool::Lease WorkerPool::Acquire() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    for (size_t i = 0; i < workers_.size(); ++i) {
      if (!workers_[i].busy) {
        workers_[i].busy = true;
        return Lease{static_cast<int>(i), workers_[i].stub.get()};
      }
    }
    available_.wait(lock);
  }
}

void WorkerPool::Release(const Lease& lease, bool failed) {
  std::unique_lock<std::mutex> lock(mutex_);
  Worker& w = workers_[static_cast<size_t>(lease.index)];
  if (failed) {
    if (w.pid > 0) {
      kill(w.pid, SIGKILL);
      waitpid(w.pid, nullptr, 0);
    }
    Spawn(lease.index);
  } else {
    // Reap a worker that died on its own (crash on a hostile document)
    // without blocking; respawn so the slot stays usable.
    if (w.pid > 0 && waitpid(w.pid, nullptr, WNOHANG) == w.pid) {
      Spawn(lease.index);
    }
  }
  w.busy = false;
  lock.unlock();
  available_.notify_one();
}

}  // namespace grpc_pdfium
