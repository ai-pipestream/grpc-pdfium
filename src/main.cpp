#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "pdf_backend_service_impl.h"

int main() {
  const char* port_env = std::getenv("GRPC_PDFIUM_PORT");
  const std::string address =
      std::string("0.0.0.0:") + (port_env != nullptr ? port_env : "50051");

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc_pdfium::PdfBackendServiceImpl service;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    std::cerr << "failed to bind " << address << std::endl;
    return 1;
  }
  std::cout << "grpc-pdfium listening on " << address << std::endl;
  server->Wait();
  return 0;
}
