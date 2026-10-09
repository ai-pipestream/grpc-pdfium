#pragma once

#include <optional>
#include <string_view>

namespace grpc_pdfium {

// What a worker process serves. The pool keeps text work and rendering on
// separate processes: rendering a page with a substituted multiple-master
// font (PDFium's built-in "Chrome Sans MM" stands in for a non-embedded
// TrueType font with /Widths) leaves the shared substitute face in a
// different state, and later text extraction in that process measures char
// boxes from it, so the right edges of text cells moved by up to half a
// point depending on what the worker had rendered before. A text worker
// never renders, so Parse and Probe answer the same for the same bytes no
// matter which worker serves them or what ran earlier.
//
// kAny serves everything: a worker started without --role, and the
// in-process service the engine tests use.
enum class WorkerRole { kAny, kText, kRender };

inline const char* WorkerRoleName(WorkerRole role) {
  switch (role) {
    case WorkerRole::kText:
      return "text";
    case WorkerRole::kRender:
      return "render";
    case WorkerRole::kAny:
      break;
  }
  return "any";
}

// The role a --role argument names; nullopt when it names none.
inline std::optional<WorkerRole> ParseWorkerRole(std::string_view name) {
  if (name == "text") return WorkerRole::kText;
  if (name == "render") return WorkerRole::kRender;
  if (name == "any") return WorkerRole::kAny;
  return std::nullopt;
}

// Probe and Parse are text work; Render is rendering.
inline bool ServesText(WorkerRole role) { return role != WorkerRole::kRender; }
inline bool ServesRender(WorkerRole role) { return role != WorkerRole::kText; }

}  // namespace grpc_pdfium
