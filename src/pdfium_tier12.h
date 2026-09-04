#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/parse/pdf/v1/pdf_backend_service.pb.h"
#include "fpdfview.h"

namespace grpc_pdfium {

class FontInterner;

// Tier 1-2 family extraction. Same threading rule as the rest of the
// engine: one call at a time per process.
namespace tier12 {

// Cheap per-document facts for Probe's ABSENT_IN_DOCUMENT verdicts.
struct DocFacts {
  bool tagged = false;
  bool encrypted = false;
  bool has_outline = false;
  int signature_count = 0;
  int javascript_count = 0;
  int attachment_count = 0;
};

DocFacts GatherDocFacts(FPDF_DOCUMENT doc);

// Emits the document-level families (metadata, encryption, outline,
// attachments, signatures, javascript) that the request selected. Returns
// false when emit returned false.
bool EmitDocLevelFamilies(
    FPDF_DOCUMENT doc, const ai::pipestream::parse::pdf::v1::ParseRequest& request,
    const DocFacts& facts,
    const std::function<
        bool(const ai::pipestream::parse::pdf::v1::ParseResponse&)>& emit);

// Fills the tier 1-2 page-level families the request selected into the
// chunk (images, hyperlinks, annotations, form fields, shapes, thumbnail)
// and collects embedded font programs found on the page for separate
// emission. form_handle may be null when form fields were not requested.
void ExtractPageTier12(
    FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_FORMHANDLE form_handle,
    const ai::pipestream::parse::pdf::v1::ParseRequest& request,
    FontInterner* fonts,
    ai::pipestream::parse::pdf::v1::PageChunk* chunk,
    ai::pipestream::parse::pdf::v1::FontTableChunk* new_fonts,
    std::vector<ai::pipestream::parse::pdf::v1::EmbeddedFont>* embedded_fonts);

// Emits one StructTreeChunk assembled from every page's structure tree.
// Only called for tagged documents. Returns false when emit returned false.
bool EmitStructTree(
    const std::vector<FPDF_PAGE>& pages,
    const std::function<
        bool(const ai::pipestream::parse::pdf::v1::ParseResponse&)>& emit,
    uint64_t* node_count);

// True when the request selects this family (empty selection = all).
bool WantFamily(const ai::pipestream::parse::pdf::v1::ParseRequest& request,
                ai::pipestream::parse::pdf::v1::PdfFamily family);

}  // namespace tier12

// Assigns stable ids to (base name, flags) font identities within one Parse
// stream; shared between the text-page cells and the page-object font walk
// so both reference one table.
class FontInterner {
 public:
  uint32_t Intern(const std::string& name, int flags, bool* is_new);

 private:
  std::map<std::pair<std::string, int>, uint32_t> ids_;
};

}  // namespace grpc_pdfium
