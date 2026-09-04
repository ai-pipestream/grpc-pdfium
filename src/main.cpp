#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "pdf_backend_service_impl.h"
#include "pdfium_engine.h"
#include "proxy_service_impl.h"
#include "worker_pool.h"

namespace {

// Fleet message-size convention (matches gRParse): big enough for a page
// raster at model DPI on any page size we have met.
constexpr int kMaxMessageBytes = 520 * 1024 * 1024;

int RunWorker(const std::string& socket_path) {
  grpc_pdfium::PdfiumEngine::InitProcess();
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

std::string SelfExe() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  buf[n] = '\0';
  return buf;
}

int RunFront() {
  const char* port_env = std::getenv("GRPC_PDFIUM_PORT");
  const std::string address =
      std::string("0.0.0.0:") + (port_env != nullptr ? port_env : "50051");

  int pool_size = 4;
  if (const char* workers_env = std::getenv("GRPC_PDFIUM_WORKERS")) {
    pool_size = std::max(1, std::atoi(workers_env));
  }

  std::string socket_dir_template = "/tmp/grpc-pdfium-XXXXXX";
  char* socket_dir = mkdtemp(socket_dir_template.data());
  if (socket_dir == nullptr) {
    std::cerr << "failed to create the worker socket directory" << std::endl;
    return 1;
  }

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc_pdfium::WorkerPool pool(SelfExe(), socket_dir, pool_size);
  grpc_pdfium::ProxyServiceImpl service(&pool);
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
    return RunWorker(argv[2]);
  }
  return RunFront();
}
