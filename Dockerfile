# syntax=docker/dockerfile:1.28
# grpc-pdfium, published as a linux/amd64 + linux/arm64 manifest list; each
# architecture builds and tests natively on its own runner pool.
#
# The build stage compiles the service and runs the test suite; the tests
# gate the image. The engine is the sha256-pinned PDFium prebuilt downloaded
# at configure time. The runtime stage is a hardened, glibc-only base: no
# package manager, no ldconfig run, and no shell needed. Every shared
# library the binary needs beyond glibc (libpdfium, libstdc++, libgcc_s) is
# staged from the build stage into /usr/local/lib and found through
# LD_LIBRARY_PATH; scripts/stage-runtime-libs.sh copies the closure and
# fails the build if anything would still resolve from outside it.
#
# The build stage is Debian trixie on purpose: the runtime base's glibc is
# 2.41, and a binary linked against a newer glibc (ubuntu 26.04's) refuses
# to load there. The base is swappable for any image whose glibc is 2.41 or
# newer:
#   --build-arg GRPC_PDFIUM_RUNTIME_IMAGE=<image>
ARG GRPC_PDFIUM_RUNTIME_IMAGE=dhi.io/debian-base:trixie-debian13

FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git make ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The image tag, reported by GetServiceInfo as build_version.
ARG GRPC_PDFIUM_BUILD_VERSION=latest
# The cache id encodes every ABI-sensitive dependency; bump it when gRPC,
# PDFium, or the toolchain moves. TARGETARCH keeps the amd64 and arm64 legs
# from sharing one build tree. The contract protos and the PDFium tarball
# are downloaded at configure time (both sha256-pinned), so the build needs
# network access.
RUN --mount=type=cache,id=grpc-pdfium-trixie-grpc1.83.1-chromium8035-${TARGETARCH},target=/build \
    cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DGRPC_PDFIUM_BUILD_VERSION=${GRPC_PDFIUM_BUILD_VERSION} \
    && cmake --build /build --parallel \
    && ctest --test-dir /build --output-on-failure \
    && mkdir -p /out/lib \
    && cp /build/grpc_pdfium /out/ \
    && cp /build/pdfium/lib/libpdfium.so /out/lib/ \
    && scripts/stage-runtime-libs.sh /out/lib /out/grpc_pdfium

# LD_LIBRARY_PATH stands in for ldconfig, and the numeric USER works with or
# without a passwd entry (65532 is the conventional nonroot uid in hardened
# images). The worker processes are exec'd from /proc/self/exe and inherit
# the environment, so they find the libraries the same way.
FROM ${GRPC_PDFIUM_RUNTIME_IMAGE}

COPY --from=build /out/lib/ /usr/local/lib/
COPY --from=build /out/grpc_pdfium /usr/local/bin/grpc_pdfium

ENV GRPC_PDFIUM_PORT=50069 \
    LD_LIBRARY_PATH=/usr/local/lib

# The front spawns its workers itself and needs a writable /tmp for their
# unix sockets, so a read-only container must mount a tmpfs there:
#   docker run --rm --read-only --tmpfs /tmp -p 50069:50069 grpc-pdfium
USER 65532:65532
EXPOSE 50069
ENTRYPOINT ["/usr/local/bin/grpc_pdfium"]
