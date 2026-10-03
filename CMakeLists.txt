cmake_minimum_required(VERSION 3.24)
project(grpc_pdfium CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

add_library(grpc_pdfium_flags INTERFACE)
target_compile_options(grpc_pdfium_flags INTERFACE -Wall -Wextra)

# ---------------------------------------------------------------------------
# The build version GetServiceInfo reports. Docker builds pass the image tag
# with -DGRPC_PDFIUM_BUILD_VERSION=..., local builds fall back to
# git describe, then "dev".
# ---------------------------------------------------------------------------
set(GRPC_PDFIUM_BUILD_VERSION "" CACHE STRING
    "build version GetServiceInfo reports (the image tag in Docker builds)")
if(GRPC_PDFIUM_BUILD_VERSION STREQUAL "")
  find_package(Git QUIET)
  if(Git_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE GRPC_PDFIUM_BUILD_VERSION
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
endif()
if(GRPC_PDFIUM_BUILD_VERSION STREQUAL "")
  set(GRPC_PDFIUM_BUILD_VERSION "dev")
endif()
target_compile_definitions(grpc_pdfium_flags INTERFACE
  GRPC_PDFIUM_BUILD_VERSION="${GRPC_PDFIUM_BUILD_VERSION}")

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
# A shallow clone needs a tag name, and a tag can move, so the checkout is
# held to the commit v1.83.1 named when it was pinned. Bump both together.
set(GRPC_COMMIT "aae267021b1ac256f8b9038d0ef528c3798cc137")
find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  WORKING_DIRECTORY "${grpc_SOURCE_DIR}"
  OUTPUT_VARIABLE GRPC_CHECKED_OUT
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT GRPC_CHECKED_OUT STREQUAL GRPC_COMMIT)
  message(FATAL_ERROR "gRPC checkout is ${GRPC_CHECKED_OUT}, expected ${GRPC_COMMIT}")
endif()

# ---------------------------------------------------------------------------
# The PdfBackendService contract (package ai.protomolt.parse.pdf.v1),
# fetched from the parser-protos repo at a pinned commit; gRParse backends/
# holds the same bytes. -DPDF_PROTO_LOCAL_DIR=<directory holding the .proto
# files> overrides for contract development only.
# The download comes from the GitHub mirror's raw URL, not git.rokkon.com:
# GitHub-hosted runners cannot resolve the Forgejo host, and the per-file
# sha256 pins below verify the bytes either way.
# ---------------------------------------------------------------------------
set(PDF_PROTOS_COMMIT "56e78a9186607847828fc3ef95f06daab2cc7209"
    CACHE STRING "parser-protos commit the contract comes from")
set(PDF_PROTOS_BASE_URL
    "https://raw.githubusercontent.com/ai-pipestream/parser-protos/${PDF_PROTOS_COMMIT}/proto/ai/protomolt/parse/pdf/v1"
    CACHE STRING "base URL the contract proto files are downloaded from")
set(PDF_PROTO_TYPES_SHA256
    "c8a556c4f978c22f918d9dcbdb765951f591f6598cd59edef83bc75e855b2d44"
    CACHE STRING "sha256 of pdf_backend_types.proto")
set(PDF_PROTO_SERVICE_SHA256
    "f0d47132b48af9cf09007afac32b26ef8f8e521b16f7bc105780d6afef5e4e8a"
    CACHE STRING "sha256 of pdf_backend_service.proto")
set(PDF_PROTO_LOCAL_DIR "" CACHE PATH "directory holding the contract protos, instead of the pinned download")

set(STAGED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(PDF_PROTO_REL "ai/protomolt/parse/pdf/v1")

if(PDF_PROTO_LOCAL_DIR)
  set(PDF_PROTO_SRC "${PDF_PROTO_LOCAL_DIR}")
else()
  set(PDF_PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/protos-export/${PDF_PROTOS_COMMIT}")
  foreach(PROTO_SPEC IN ITEMS
      "pdf_backend_types.proto;${PDF_PROTO_TYPES_SHA256}"
      "pdf_backend_service.proto;${PDF_PROTO_SERVICE_SHA256}")
    list(GET PROTO_SPEC 0 PROTO_NAME)
    list(GET PROTO_SPEC 1 PROTO_SHA)
    if(NOT EXISTS "${PDF_PROTO_SRC}/${PROTO_NAME}")
      file(DOWNLOAD "${PDF_PROTOS_BASE_URL}/${PROTO_NAME}"
        "${PDF_PROTO_SRC}/${PROTO_NAME}"
        EXPECTED_HASH SHA256=${PROTO_SHA}
        STATUS PROTO_DL_STATUS)
      list(GET PROTO_DL_STATUS 0 PROTO_DL_CODE)
      if(NOT PROTO_DL_CODE EQUAL 0)
        file(REMOVE "${PDF_PROTO_SRC}/${PROTO_NAME}")
        message(FATAL_ERROR "contract proto download failed: ${PROTO_DL_STATUS}")
      endif()
    endif()
  endforeach()
endif()

file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}")
configure_file("${PDF_PROTO_SRC}/pdf_backend_types.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_types.proto" COPYONLY)
configure_file("${PDF_PROTO_SRC}/pdf_backend_service.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_service.proto" COPYONLY)

# ---------------------------------------------------------------------------
# The PDFium engine, from the sha256-pinned prebuilt binary release
# (bblanchon/pdfium-binaries, the pypdfium2 precedent). Non-V8, non-XFA.
# ---------------------------------------------------------------------------
set(PDFIUM_RELEASE "chromium/8035" CACHE STRING "pdfium-binaries release tag")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(PDFIUM_ARCH "arm64")
  set(PDFIUM_SHA256_DEFAULT
      "10bb7728a5268593a31c2024e0e57ea26b73b7a54195054f43d2ad0869d55a48")
else()
  set(PDFIUM_ARCH "x64")
  set(PDFIUM_SHA256_DEFAULT
      "2e6db042dd2cff2d5247023dbec6c7ebb800042ce83c835d6468d45229669bd4")
endif()
set(PDFIUM_SHA256 "${PDFIUM_SHA256_DEFAULT}" CACHE STRING "sha256 of the pdfium prebuilt tarball")
string(REPLACE "/" "%2F" PDFIUM_RELEASE_ESCAPED "${PDFIUM_RELEASE}")
set(PDFIUM_TGZ "${CMAKE_CURRENT_BINARY_DIR}/pdfium-linux-${PDFIUM_ARCH}.tgz")
if(NOT EXISTS "${PDFIUM_TGZ}")
  file(DOWNLOAD
    "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_RELEASE_ESCAPED}/pdfium-linux-${PDFIUM_ARCH}.tgz"
    "${PDFIUM_TGZ}"
    EXPECTED_HASH SHA256=${PDFIUM_SHA256}
    STATUS PDFIUM_DL_STATUS)
  list(GET PDFIUM_DL_STATUS 0 PDFIUM_DL_CODE)
  if(NOT PDFIUM_DL_CODE EQUAL 0)
    file(REMOVE "${PDFIUM_TGZ}")
    message(FATAL_ERROR "pdfium tarball download failed: ${PDFIUM_DL_STATUS}")
  endif()
endif()
set(PDFIUM_DIR "${CMAKE_CURRENT_BINARY_DIR}/pdfium")
file(ARCHIVE_EXTRACT INPUT "${PDFIUM_TGZ}" DESTINATION "${PDFIUM_DIR}")
set(PDFium_DIR "${PDFIUM_DIR}")
find_package(PDFium REQUIRED CONFIG PATHS "${PDFIUM_DIR}" NO_DEFAULT_PATH)

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
# The service binary: front (worker pool + proxy) and worker in one
# executable, selected by --worker.
# ---------------------------------------------------------------------------
add_executable(grpc_pdfium
  src/main.cpp
  src/byte_cache.cpp
  src/page_space.cpp
  src/pdf_backend_service_impl.cpp
  src/pdfium_engine.cpp
  src/pdfium_tier12.cpp
  src/proxy_service_impl.cpp
  src/sha256.cpp
  src/worker_pool.cpp)
target_include_directories(grpc_pdfium PRIVATE src)
# crypto is the boringssl target gRPC already builds (the module SSL
# provider); it supplies the SHA-256 of the content-addressed handshake.
target_link_libraries(grpc_pdfium PRIVATE pdf_backend_proto pdfium crypto grpc_pdfium_flags)

# ---------------------------------------------------------------------------
# Tests: in-process round-trip through real gRPC stubs, plus a pool test
# against the real front binary.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(pdf_backend_contract_test
  test/contract_roundtrip_test.cpp
  src/page_space.cpp
  src/pdf_backend_service_impl.cpp
  src/pdfium_engine.cpp
  src/pdfium_tier12.cpp)
target_include_directories(pdf_backend_contract_test PRIVATE src)
target_link_libraries(pdf_backend_contract_test PRIVATE pdf_backend_proto pdfium grpc_pdfium_flags)
add_test(NAME contract_roundtrip COMMAND pdf_backend_contract_test
  "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/hello.pdf")

add_executable(tier12_fixture_test
  test/tier12_fixture_test.cpp
  src/page_space.cpp
  src/pdf_backend_service_impl.cpp
  src/pdfium_engine.cpp
  src/pdfium_tier12.cpp
  src/sha256.cpp)
target_include_directories(tier12_fixture_test PRIVATE src)
target_link_libraries(tier12_fixture_test PRIVATE pdf_backend_proto pdfium crypto grpc_pdfium_flags)
add_test(NAME tier12_fixture COMMAND tier12_fixture_test
  "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")

add_executable(pool_roundtrip_test test/pool_roundtrip_test.cpp src/sha256.cpp)
target_include_directories(pool_roundtrip_test PRIVATE src)
target_link_libraries(pool_roundtrip_test PRIVATE pdf_backend_proto crypto grpc_pdfium_flags)
add_test(NAME pool_roundtrip COMMAND pool_roundtrip_test
  "$<TARGET_FILE:grpc_pdfium>"
  "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/hello.pdf")
