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

No request can hold a worker for good. A worker call inherits its client's
deadline and cancellation, and a call the client abandons kills its worker,
which is respawned. A request waits for a free worker no longer than its
client does, nor than `GRPC_PDFIUM_QUEUE_TIMEOUT_S` (default 300), and then
fails `RESOURCE_EXHAUSTED`. A watchdog kills a worker whose call forwards
nothing for `GRPC_PDFIUM_REQUEST_TIMEOUT_S` (default 300); that call ends
`DEADLINE_EXCEEDED`, or `CANCELLED` when the front was stuck writing to a
client that stopped reading (the watchdog cancels that call), and the slot
comes back respawned. 0 turns either limit
off; each takes whole seconds up to 604800 (a week), and any other value
stops the service at startup.

Each worker also runs under an address-space limit,
`GRPC_PDFIUM_WORKER_MAX_BYTES` (bytes; default 3 GiB, 0 turns it off,
otherwise at least 256 MiB), with core dumps off. PDFium decodes a stream
in full, up to 1 GiB, even to report an attachment's size, so a document of
a few kilobytes of nested Flate can ask for gigabytes; under the limit the
allocation fails, the worker dies, the call ends `UNAVAILABLE`, and the
slot is respawned. The default leaves room for a 520 MiB document and a
512 MiB raster; lower it when documents and rasters are smaller.

The content-addressed handshake (`PdfDocument.sha256`) is served by the
front process, which owns the client-facing wire: it verifies a supplied
hash against the bytes (a mismatch answers `LOAD_STATUS_HASH_MISMATCH`),
caches verified bytes, and answers hash-only lookups from the cache (a miss
answers `LOAD_STATUS_BYTES_REQUIRED`). Both verdicts are typed on each
RPC's own surface: `ProbeResponse.capabilities`, the `Parse` header, the
`RenderResponse` head. Workers always receive full bytes over their unix
sockets and keep no document bytes, so a worker respawn never loses cached
content. The cache is an in-memory LRU bounded by document count
(`GRPC_PDFIUM_CACHE_MAX_DOCUMENTS`, default 8; 0 disables) and by total
bytes (`GRPC_PDFIUM_CACHE_MAX_BYTES`, default 2 GiB). Hashes come from the
boringssl the gRPC build already carries.

`Parse` loads only the pages its range selects, one at a time. Its header
still lists every page, and PDFium reads page boxes and rotation only from
a loaded page, so each worker remembers the page inventory of the last few
documents it parsed, keyed by the same hash; a client that parses one page
per call pays a load of every page once per document, not once per call.

Page geometry follows the contract frame: PDF user space before `/Rotate`,
shifted so the CropBox's bottom-left corner is (0, 0). That holds for text
cells and their quads, images, vector shapes, links, annotations, form
widgets, and link and outline destinations (which use the frame of the page
they target). Every `PageInfo` says so with `page_space =
PAGE_SPACE_CROP_BOX`; its `media_box` and `crop_box` stay as stored. PDFium
reports user space, so the engine shifts each finished page chunk once
(`src/page_space.*`).

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
./build/grpc_pdfium           # listens on 0.0.0.0:50069 (fleet default)
GRPC_PDFIUM_PORT=50100 ./build/grpc_pdfium   # env override
```

Health and server reflection are enabled; `Probe`, `Parse`, `Render`, and
`GetServiceInfo` are the service surface. `GetServiceInfo` reports the
backend identity (`backend_name` and `engine_version` are the same strings
`Probe` reports), the build version (the image tag in Docker builds, a
`git describe` fallback locally), and the family `UiInfo` block for the
demo shell (no web UI yet; the description says so).

## Docker

```bash
docker build -t grpc-pdfium .
docker run --rm --read-only --tmpfs /tmp -p 50069:50069 grpc-pdfium
scripts/smoke-test.sh grpc-pdfium     # boot-proof a built image
```

The build stage (a Debian trixie toolchain) runs the test suite and the
tests gate the image. The runtime stage is the hardened
`dhi.io/debian-base:trixie-debian13` base: glibc and nothing else, no
package manager, no ldconfig, and the service runs as uid 65532 out of the
box, so no `--user` flag is needed. It carries the one executable (front and
worker in a single binary) plus the shared libraries it needs beyond glibc
(the PDFium engine, libstdc++, libgcc_s), staged from the build stage into
`/usr/local/lib` and found through `LD_LIBRARY_PATH`;
`scripts/stage-runtime-libs.sh` copies that closure at build time and
fails the build if anything would resolve from outside it. The base is
swappable with `--build-arg GRPC_PDFIUM_RUNTIME_IMAGE=<image>` for any
image whose glibc is 2.41 or newer.

The container runs read-only with one requirement: the front spawns its
workers itself and needs a writable `/tmp` for their unix sockets, so
`--read-only` must come with `--tmpfs /tmp` (a compose service needs a
`tmpfs: [/tmp]` entry beside `read_only: true`). Without it the front exits
at startup with "failed to create the worker socket directory".

`scripts/smoke-test.sh IMAGE` is the boot gate CI and the publish workflow
run before any push: the library closure resolves inside the image (the
dynamic loader reports it, since the base has no `ldd`), the front reaches
its "listening on" line under `--read-only --tmpfs /tmp --cap-drop ALL`,
every process runs as uid 65532, and the worker pool is spawned in full.
Pushes to `main` republish `docker.io/pipestreamai/grpc-pdfium:latest`
as a linux/amd64 + linux/arm64 manifest list (the arm64 leg builds
natively on GitHub's hosted arm64 runner; each leg boot-proofs its own
pushed digest before the tag is assembled); a manual `workflow_dispatch`
with a version input also tags that version and stamps it as the build
version.
