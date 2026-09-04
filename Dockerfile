# syntax=docker/dockerfile:1.26
# grpc-pdfium — amd64-only, like the rest of the family's C++ services.
#
# The build stage compiles the service and runs the test suite; the tests
# gate the image. The engine is the sha256-pinned PDFium prebuilt downloaded
# at configure time; the runtime stage carries the one executable (front and
# worker in a single binary, workers spawned with --worker) plus the PDFium
# shared library it links against.

FROM ubuntu:26.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git make ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The image tag, reported by GetServiceInfo as build_version.
ARG GRPC_PDFIUM_BUILD_VERSION=latest
# The cache id encodes every ABI-sensitive dependency; bump it when gRPC,
# PDFium, or the toolchain moves.
RUN --mount=type=cache,id=grpc-pdfium-ubuntu26.04-grpc1.83.1-chromium8035,target=/build \
    cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DGRPC_PDFIUM_BUILD_VERSION=${GRPC_PDFIUM_BUILD_VERSION} \
    && cmake --build /build --parallel \
    && ctest --test-dir /build --output-on-failure \
    && mkdir -p /out \
    && cp /build/grpc_pdfium /out/ \
    && cp /build/pdfium/lib/libpdfium.so /out/

FROM ubuntu:26.04

COPY --from=build /out/grpc_pdfium /usr/local/bin/grpc_pdfium
COPY --from=build /out/libpdfium.so /usr/local/lib/libpdfium.so
RUN ldconfig

ENV GRPC_PDFIUM_PORT=50069

# The front spawns its workers itself and needs /tmp for their sockets:
#   docker run --rm --tmpfs /tmp -p 50069:50069 grpc-pdfium
USER 65532:65532
EXPOSE 50069
ENTRYPOINT ["/usr/local/bin/grpc_pdfium"]
