cmake_minimum_required(VERSION 3.24)
project(grpc_pdfium CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

add_library(grpc_pdfium_flags INTERFACE)
target_compile_options(grpc_pdfium_flags INTERFACE -Wall -Wextra)

# ---------------------------------------------------------------------------
# gRPC + protobuf, the fleet's C++ pattern (same tag as gRParse).
# ---------------------------------------------------------------------------
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(grpc
  GIT_REPOSITORY https://github.com/grpc/grpc.git
  GIT_TAG v1.83.1
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(grpc)

# ---------------------------------------------------------------------------
# The PdfBackendService contract, consumed from the pipestream-protos release
# (the ai.pipestream.parse.pdf.v1 package). The pinned release tarball is the
# default; -DPDF_PROTO_LOCAL_DIR=<pipestream-protos checkout> overrides it for
# contract development only.
# ---------------------------------------------------------------------------
set(PDF_PROTOS_VERSION "0.15.0" CACHE STRING "pipestream-protos release the contract comes from")
set(PDF_PROTOS_SHA256 "558974e94c148b351f7dce74ac1189bd41e0433d5fa104cffa21c82c62c6db9b"
    CACHE STRING "sha256 of the release proto tarball")
set(PDF_PROTOS_URL
    "https://git.rokkon.com/api/packages/ai-pipestream/generic/pipestream-protos/${PDF_PROTOS_VERSION}/pipestream-protos-${PDF_PROTOS_VERSION}.tgz"
    CACHE STRING "URL of the release proto tarball")
set(PDF_PROTO_LOCAL_DIR "" CACHE PATH "local pipestream-protos checkout to use instead of the release tarball")

set(STAGED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(PDF_PROTO_REL "ai/pipestream/parse/pdf/v1")

if(PDF_PROTO_LOCAL_DIR)
  set(PDF_PROTO_SRC "${PDF_PROTO_LOCAL_DIR}/pdf-backend/proto/${PDF_PROTO_REL}")
else()
  set(PROTOS_TGZ "${CMAKE_CURRENT_BINARY_DIR}/pipestream-protos-${PDF_PROTOS_VERSION}.tgz")
  if(NOT EXISTS "${PROTOS_TGZ}")
    file(DOWNLOAD "${PDF_PROTOS_URL}" "${PROTOS_TGZ}"
      EXPECTED_HASH SHA256=${PDF_PROTOS_SHA256}
      STATUS PROTOS_DL_STATUS)
    list(GET PROTOS_DL_STATUS 0 PROTOS_DL_CODE)
    if(NOT PROTOS_DL_CODE EQUAL 0)
      file(REMOVE "${PROTOS_TGZ}")
      message(FATAL_ERROR "proto tarball download failed: ${PROTOS_DL_STATUS}")
    endif()
  endif()
  set(PROTOS_EXPORT "${CMAKE_CURRENT_BINARY_DIR}/protos-export")
  file(ARCHIVE_EXTRACT INPUT "${PROTOS_TGZ}" DESTINATION "${PROTOS_EXPORT}")
  set(PDF_PROTO_SRC "${PROTOS_EXPORT}/pdf-backend/proto/${PDF_PROTO_REL}")
endif()

file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}")
configure_file("${PDF_PROTO_SRC}/pdf_backend_types.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_types.proto" COPYONLY)
configure_file("${PDF_PROTO_SRC}/pdf_backend_service.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_service.proto" COPYONLY)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

set(PDF_BACKEND_PROTOS
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_types.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_service.proto")
set(PDF_BACKEND_GENERATED
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.grpc.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.grpc.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.grpc.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.grpc.pb.h")

add_custom_command(
  OUTPUT ${PDF_BACKEND_GENERATED}
  COMMAND $<TARGET_FILE:protobuf::protoc>
    --proto_path=${STAGED_PROTO_DIR}
    --cpp_out=${GENERATED_DIR}
    --grpc_out=${GENERATED_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
    ${PDF_BACKEND_PROTOS}
  DEPENDS ${PDF_BACKEND_PROTOS} protobuf::protoc grpc_cpp_plugin
  COMMENT "Generating C++ stubs for the PdfBackendService contract")

add_library(pdf_backend_proto STATIC ${PDF_BACKEND_GENERATED})
target_include_directories(pdf_backend_proto SYSTEM PUBLIC "${GENERATED_DIR}")
target_link_libraries(pdf_backend_proto PUBLIC grpc++ grpc++_reflection libprotobuf)

# ---------------------------------------------------------------------------
# The service binary.
# ---------------------------------------------------------------------------
add_executable(grpc_pdfium
  src/main.cpp
  src/pdf_backend_service_impl.cpp)
target_include_directories(grpc_pdfium PRIVATE src)
target_link_libraries(grpc_pdfium PRIVATE pdf_backend_proto grpc_pdfium_flags)

# ---------------------------------------------------------------------------
# Tests: in-process round-trip through real gRPC stubs.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(pdf_backend_contract_test
  test/contract_roundtrip_test.cpp
  src/pdf_backend_service_impl.cpp)
target_include_directories(pdf_backend_contract_test PRIVATE src)
target_link_libraries(pdf_backend_contract_test PRIVATE pdf_backend_proto grpc_pdfium_flags)
add_test(NAME contract_roundtrip COMMAND pdf_backend_contract_test)
