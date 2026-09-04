#pragma once

#include <sys/types.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace grpc_pdfium {

// A pool of single-threaded worker processes, each serving the contract on
// its own unix socket. This is the deployment shape PDFium's global-state
// rule demands: concurrency across processes, one request per process at a
// time, and a crash on a hostile document kills one worker, not the
// service.
class WorkerPool {
 public:
  struct Lease {
    int index = -1;
    ai::pipestream::parse::pdf::v1::PdfBackendService::Stub* stub = nullptr;
  };

  // Spawns size workers running self_exe --worker <socket>. Sockets live
  // under socket_dir.
  WorkerPool(std::string self_exe, std::string socket_dir, int size);
  ~WorkerPool();

  // Blocks until a worker is free and returns its lease.
  Lease Acquire();

  // Returns the worker to the pool. When failed is true the worker process
  // is killed and respawned first (its engine state is suspect).
  void Release(const Lease& lease, bool failed);

 private:
  struct Worker {
    pid_t pid = -1;
    std::string socket_path;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ai::pipestream::parse::pdf::v1::PdfBackendService::Stub>
        stub;
    bool busy = false;
  };

  void Spawn(int index);

  std::string self_exe_;
  std::string socket_dir_;
  std::mutex mutex_;
  std::condition_variable available_;
  std::vector<Worker> workers_;
};

}  // namespace grpc_pdfium
