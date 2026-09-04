# grpc-pdfium

A gRPC PDF backend service around PDFium, implementing the fleet's common
`PdfBackendService` contract (`ai.pipestream.parse.pdf.v1`, from the
pipestream-protos release this build pins). Apache-2.0, engine included:
this is the standard PDF backend of the parsing fleet.

Status: walking skeleton. The contract is served in full and every RPC
round-trips typed data, but the PDFium engine is not linked yet, so every
document reports a typed load failure. The engine, the worker-process pool,
and the tier 0 families land next (see gRParse
`docs/pdf-backend-services.md`, milestone M1).

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
