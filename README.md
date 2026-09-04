# grpc-pdfium

A gRPC PDF backend service around PDFium, implementing the fleet's common
`PdfBackendService` contract (`ai.pipestream.parse.pdf.v1`, from the
pipestream-protos release this build pins). Apache-2.0, engine included:
this is the standard PDF backend of the parsing fleet.

Status: tier 0. The engine is the sha256-pinned PDFium prebuilt
(chromium/8035, 154.0.8035.0, non-V8/non-XFA, the pypdfium2 precedent) and
the floor families are served: typed load status, page inventory, text
cells with font references, and page rasters. Tier 1-2 families are
declared unsupported in `Probe` until they land (see gRParse
`docs/pdf-backend-services.md`, milestone M2).

PDFium keeps process-global state and is not thread-safe, so the service
runs as a pool of single-threaded worker processes behind a gRPC front
(`GRPC_PDFIUM_WORKERS`, default 4). Workers speak the same contract over
unix sockets, die with the front (`PR_SET_PDEATHSIG`), and a crash on a
hostile document costs one worker, which is respawned.

## Build and test

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The contract protos come from the pinned pipestream-protos release tarball
(sha256-verified download at configure time). To develop against a local
contract instead:

```bash
cmake -S . -B build -DPDF_PROTO_LOCAL_DIR=/path/to/pipestream-protos
```

## Run

```bash
GRPC_PDFIUM_PORT=50051 ./build/grpc_pdfium
```

Health and server reflection are enabled; `Probe`, `Parse`, and `Render`
are the service surface.
