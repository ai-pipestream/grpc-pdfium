# grpc-pdfium

A gRPC PDF backend service around PDFium, implementing the fleet's common
`PdfBackendService` contract (`ai.protomolt.parse.pdf.v1`, from the
parser-protos commit this build pins). Apache-2.0, engine included:
this is the standard PDF backend of the parsing fleet.

Status: tiers 0-2. The engine is the sha256-pinned PDFium prebuilt
(chromium/8035, 154.0.8035.0, non-V8/non-XFA, the pypdfium2 precedent).
Served families: typed load status, page inventory, text cells with font
references, page rasters, document metadata, encryption info, fonts with
embedded programs, placed images, hyperlinks, outline, annotations, form
fields, attachments, vector shapes, the tagged structure tree, signatures,
JavaScript listing, and page thumbnails. `Probe` gives per-document
verdicts (a document without an outline declares the family absent); deep
graphics resources are declared unsupported, since this engine does not
type them out. Engine limits worth knowing: no XMP packet and no custom
info keys reach the public API, so `DocMeta` fills the standard keys only.

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

The contract protos come from the pinned parser-protos commit
(sha256-verified download at configure time). To develop against a local
contract instead, point at a directory holding the two proto files:

```bash
cmake -S . -B build -DPDF_PROTO_LOCAL_DIR=/path/to/gRParse/backends
```

## Run

```bash
GRPC_PDFIUM_PORT=50051 ./build/grpc_pdfium
```

Health and server reflection are enabled; `Probe`, `Parse`, and `Render`
are the service surface.
