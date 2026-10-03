#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"
#include "fpdf_text.h"
#include "fpdfview.h"
#include "page_space.h"

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
  // The catalog names an interactive form (/AcroForm or XFA).
  bool has_form = false;
};

DocFacts GatherDocFacts(FPDF_DOCUMENT doc);

// Emits the document-level families (metadata, encryption, outline,
// attachments, signatures, javascript) that the request selected. Returns
// false when emit returned false. Attachment bytes too large to send are
// left out with a warning. Outline destinations are moved into their target
// pages' CropBox frames by origins.
bool EmitDocLevelFamilies(
    FPDF_DOCUMENT doc, const ai::protomolt::parse::pdf::v1::ParseRequest& request,
    const DocFacts& facts, const CropOrigins& origins,
    const std::function<
        bool(const ai::protomolt::parse::pdf::v1::ParseResponse&)>& emit,
    std::vector<ai::protomolt::parse::pdf::v1::ParseWarning>* warnings);

// Fills the tier 1-2 page-level families the request selected into the
// chunk (images, hyperlinks, annotations, form fields, shapes, thumbnail)
// and collects embedded font programs found on the page for separate
// emission. form_handle may be null when form fields were not requested.
// Image pixels past the decode limits are left out with a warning.
void ExtractPageTier12(
    FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_FORMHANDLE form_handle,
    const ai::protomolt::parse::pdf::v1::ParseRequest& request,
    FontInterner* fonts,
    ai::protomolt::parse::pdf::v1::PageChunk* chunk,
    ai::protomolt::parse::pdf::v1::FontTableChunk* new_fonts,
    std::vector<ai::protomolt::parse::pdf::v1::EmbeddedFont>* embedded_fonts,
    std::vector<ai::protomolt::parse::pdf::v1::ParseWarning>* warnings);

// Appends one page's structure tree roots to chunk, counting the nodes.
// Only called for tagged documents.
void AppendStructTree(FPDF_PAGE page, uint32_t page_index,
                      ai::protomolt::parse::pdf::v1::StructTreeChunk* chunk,
                      uint64_t* node_count);

// True when the request selects this family (empty selection = all).
bool WantFamily(const ai::protomolt::parse::pdf::v1::ParseRequest& request,
                ai::protomolt::parse::pdf::v1::PdfFamily family);

// Interns the font the character at index on a text page is drawn with
// (text_object is that character's FPDFText_GetTextObject, which may be
// null), adding a new font's table entry and program as InternFont does.
// Returns the font id, or nullopt when the engine names no font.
std::optional<uint32_t> InternCharFont(
    FPDF_TEXTPAGE text_page, int index, FPDF_PAGEOBJECT text_object,
    bool want_program, FontInterner* fonts,
    ai::protomolt::parse::pdf::v1::FontTableChunk* new_fonts,
    std::vector<ai::protomolt::parse::pdf::v1::EmbeddedFont>* embedded_fonts);

}  // namespace tier12

// Assigns stable ids to (base name, descriptor flags) font identities within
// one Parse stream; shared between the text-page cells and the page-object
// font walk so both reference one table. Both read the key from the font
// handle (FPDFFont_GetBaseFontName, FPDFFont_GetFlags), so a font gets one
// id whichever path meets it first.
class FontInterner {
 public:
  uint32_t Intern(const std::string& name, int flags, bool* is_new);

 private:
  std::map<std::pair<std::string, int>, uint32_t> ids_;
};

}  // namespace grpc_pdfium
