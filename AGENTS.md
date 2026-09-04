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
