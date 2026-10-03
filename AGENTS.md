# Agent rules for grpc-pdfium

One of three interchangeable PDF backend services (grpc-pdfium, grpc-qparse,
grpc-poppler) implementing `PdfBackendService`, package
`ai.protomolt.parse.pdf.v1`. gRParse is the client (single target or a comma
list for consensus mode via `GRPARSE_PDF_BACKEND`).

- **The contract lives in the parser-protos repo**
  (`git.rokkon.com/ai-pipestream/parser-protos`, GitHub mirror of the same
  name). This build downloads the proto files from it at the commit pinned
  in `CMakeLists.txt` (`PDF_PROTOS_COMMIT`, per-file sha256). gRParse
  `backends/` carries identical copies. To change the contract: land the
  same bytes in parser-protos and gRParse `backends/`, then advance the pin
  and hashes here. The contract is additive only; never renumber, retype,
  or remove anything.
- **This project is NOT part of the pipestream-ai platform.** Never put the
  contract in, or take dependencies from, `/work/main/pipestream-ai` or the
  `pipestream-protos` repo. (The contract briefly lived there as a
  `pdf-backend` module; reverted 2026-09-04. Do not repeat that.)
- Word-level text cells come from PDFium LOOSE char boxes; that granularity
  is load-bearing for gRParse's fold. Do not switch to tight-ink or
  line-level cells without running both the differential harness and a
  gRParse scorecard leg.
- Page geometry is crop-relative (`PageInfo.page_space =
  PAGE_SPACE_CROP_BOX`): every family is extracted in PDFium's user space
  and shifted by the CropBox origin in one place, `src/page_space.*`, after
  extraction. A new geometry field must be added there, or it ships in the
  wrong frame.
- The content-addressed handshake (`PdfDocument.sha256`, cache verdicts
  `LOAD_STATUS_BYTES_REQUIRED` / `LOAD_STATUS_HASH_MISMATCH`) lives in the
  FRONT process only (`src/proxy_service_impl.cpp`, `src/byte_cache.*`):
  it owns the client-facing wire, and workers always receive full bytes
  over their unix sockets. A worker's only state is derived: the page
  inventory cache in `src/pdfium_engine.cpp`, keyed by the hash the front
  forwards (it relies on the front's hash check), so `Parse` loads only the
  requested pages after the first call for a document. Byte cache bounds
  are env knobs `GRPC_PDFIUM_CACHE_MAX_DOCUMENTS` (default 8, 0 disables) and
  `GRPC_PDFIUM_CACHE_MAX_BYTES` (default 2 GiB). SHA-256 is boringssl's
  one-shot `SHA256()` (`src/sha256.*`), the TLS library gRPC already
  builds; do not add another crypto dependency.
- The front bounds every worker lease (`src/proxy_service_impl.cpp`,
  `src/worker_pool.*`): worker calls are made with
  `ClientContext::FromServerContext`, so the client's deadline and
  cancellation reach them; an abandoned call kills its worker; the wait for
  a free worker ends at `GRPC_PDFIUM_QUEUE_TIMEOUT_S` (RESOURCE_EXHAUSTED);
  and the pool watchdog kills a worker whose call makes no progress for
  `GRPC_PDFIUM_REQUEST_TIMEOUT_S` (DEADLINE_EXCEEDED; CANCELLED when the
  front was stuck writing to a client that stopped reading). Progress is a
  forwarded message or a worker heartbeat: each worker writes a byte per
  page load to a socket pair the front gave it (`--heartbeat-fd`), so a
  first `Parse` filling a large document's inventory is not cut before its
  header. Both default to
  300 s, 0 turns one off, and a value that is not whole seconds up to a
  week stops the start. Respawns run outside the pool lock.
  Each worker caps its own address space at
  `GRPC_PDFIUM_WORKER_MAX_BYTES` (default 3 GiB, 0 off) and disables core
  dumps (`src/main.cpp`): PDFium decodes whole streams with no size check
  of its own, so that limit, not the engine's payload ceilings, is what
  bounds a decompression bomb.
- Default port is 50069 (`GRPC_PDFIUM_PORT` overrides). 50051/50052/50053
  belong to gRParse, grPOIc, and grpc-libreoffice; the PDF backends own
  50069 (pdfium), 50070 (qparse), 50071 (poppler) in the workspace table.
- `GetServiceInfo` is served by both the front and the workers, answered
  from `src/service_info.h`: `backend_name`/`engine_version` are the exact
  strings `Probe` reports, `build_version` comes from the
  `GRPC_PDFIUM_BUILD_VERSION` define (CMake cache var; Docker builds pass
  the image tag, local builds fall back to `git describe`, then "dev"),
  and the `UiInfo` block follows the family convention (no web UI yet; the
  description says so).
- The Docker image is multi-stage: the build stage (Debian trixie
  toolchain) runs the ctest suite as a gate; the runtime is the hardened
  `dhi.io/debian-base:trixie-debian13` base (glibc only, no package
  manager, no ldconfig, uid 65532) carrying the binary plus the staged
  shared-library closure (`scripts/stage-runtime-libs.sh`, PDFium
  included) under `/usr/local/lib` on `LD_LIBRARY_PATH`. The build stage
  must stay on a glibc no newer than the runtime base's (2.41); a build on
  ubuntu 26.04 produces a binary the base cannot load.
  `GRPC_PDFIUM_RUNTIME_IMAGE` swaps the base. A read-only container needs
  `--tmpfs /tmp` for the worker sockets. `scripts/smoke-test.sh IMAGE` is
  the boot gate (closure, boot to listening under the hardened flags, uid,
  worker count); `ci.yml` runs the image build then the smoke test on
  push/PR, and `.github/workflows/publish.yml` builds, smoke-tests, and
  only then pushes `docker.io/pipestreamai/grpc-pdfium:latest` on push to
  `main` as a linux/amd64 + linux/arm64 manifest list (each leg pushes by
  digest and boot-proofs its own on its own architecture; the arm64 leg
  runs natively on GitHub's hosted `ubuntu-24.04-arm` runner); Docker Hub auth via the `DOCKER_USER`/`DOCKER_TOKEN`
  org secrets.
