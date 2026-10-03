#include "pdfium_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fpdf_edit.h"
#include "fpdf_formfill.h"
#include "fpdf_text.h"
#include "fpdf_transformpage.h"
#include "fpdfview.h"
#include "page_space.h"
#include "pdfium_tier12.h"

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

constexpr char kBackendName[] = "grpc-pdfium";
constexpr char kEngineVersion[] = "pdfium 154.0.8035.0 (chromium/8035)";

// The largest raster Render produces: one PageRaster must fit the fleet's
// 520 MiB message limit, and the bitmap is allocated before it is filled,
// so a larger page is refused before any pixel memory is taken.
constexpr double kMaxRasterBytes = 512.0 * 1024 * 1024;

// UTF-16 (host order, as PDFium emits) to UTF-8, surrogate pairs included.
// Invalid sequences become U+FFFD rather than dropping text silently.
std::string Utf16ToUtf8(const std::vector<unsigned short>& units) {
  std::string out;
  out.reserve(units.size() * 3);
  auto append_code_point = [&out](uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };
  for (size_t i = 0; i < units.size(); ++i) {
    unsigned short u = units[i];
    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size() &&
        units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
      uint32_t cp = 0x10000 + ((static_cast<uint32_t>(u) - 0xD800) << 10) +
                    (units[i + 1] - 0xDC00);
      append_code_point(cp);
      ++i;
    } else if (u >= 0xD800 && u <= 0xDFFF) {
      append_code_point(0xFFFD);
    } else {
      append_code_point(u);
    }
  }
  return out;
}

struct LoadedDocument {
  FPDF_DOCUMENT doc = nullptr;
  pdfv1::LoadStatus status = pdfv1::LOAD_STATUS_UNSPECIFIED;
  std::string detail;

  ~LoadedDocument() {
    if (doc != nullptr) FPDF_CloseDocument(doc);
  }
};

void LoadDocument(const pdfv1::PdfDocument& request, LoadedDocument* out) {
  const std::string& data = request.data();
  if (data.rfind("%PDF-", 0) != 0) {
    out->status = pdfv1::LOAD_STATUS_NOT_PDF;
    out->detail = "missing %PDF- header";
    return;
  }
  const char* password =
      request.has_password() ? request.password().c_str() : nullptr;
  out->doc = FPDF_LoadMemDocument64(data.data(), data.size(), password);
  if (out->doc != nullptr) {
    out->status = pdfv1::LOAD_STATUS_OK;
    return;
  }
  switch (FPDF_GetLastError()) {
    case FPDF_ERR_FORMAT:
      out->status = pdfv1::LOAD_STATUS_CORRUPT;
      out->detail = "not parsable as PDF (FPDF_ERR_FORMAT)";
      break;
    case FPDF_ERR_PASSWORD:
      out->status = request.has_password()
                        ? pdfv1::LOAD_STATUS_PASSWORD_INCORRECT
                        : pdfv1::LOAD_STATUS_PASSWORD_REQUIRED;
      break;
    case FPDF_ERR_SECURITY:
      out->status = pdfv1::LOAD_STATUS_UNSUPPORTED_ENCRYPTION;
      break;
    default:
      out->status = pdfv1::LOAD_STATUS_ENGINE_ERROR;
      out->detail =
          "FPDF_LoadMemDocument64 error " + std::to_string(FPDF_GetLastError());
      break;
  }
}

void FillCapabilities(const LoadedDocument& loaded,
                      pdfv1::BackendCapabilities* caps) {
  caps->set_backend_name(kBackendName);
  caps->set_engine_version(kEngineVersion);
  caps->set_load_status(loaded.status);
  if (loaded.status != pdfv1::LOAD_STATUS_OK) {
    if (!loaded.detail.empty()) caps->set_load_detail(loaded.detail);
    return;
  }
  caps->set_page_count(
      static_cast<uint32_t>(FPDF_GetPageCount(loaded.doc)));
  const tier12::DocFacts facts = tier12::GatherDocFacts(loaded.doc);
  for (int f = pdfv1::PdfFamily_MIN + 1; f <= pdfv1::PdfFamily_MAX; ++f) {
    if (!pdfv1::PdfFamily_IsValid(f)) continue;
    auto family = static_cast<pdfv1::PdfFamily>(f);
    auto* verdict = caps->add_families();
    verdict->set_family(family);
    // Document-level families the engine can count cheaply get a real
    // per-document verdict; page-scoped families report SUPPORTED, which
    // means "the backend emits whatever the document holds".
    bool absent = false;
    switch (family) {
      case pdfv1::PDF_FAMILY_STRUCT_TREE: absent = !facts.tagged; break;
      case pdfv1::PDF_FAMILY_ENCRYPTION_INFO: absent = !facts.encrypted; break;
      case pdfv1::PDF_FAMILY_OUTLINE: absent = !facts.has_outline; break;
      case pdfv1::PDF_FAMILY_SIGNATURES: absent = facts.signature_count == 0; break;
      case pdfv1::PDF_FAMILY_JAVASCRIPT: absent = facts.javascript_count == 0; break;
      case pdfv1::PDF_FAMILY_ATTACHMENTS: absent = facts.attachment_count == 0; break;
      // Without an interactive form the engine has no form handle and
      // emits no widget, so a caller can skip the page walk.
      case pdfv1::PDF_FAMILY_FORM_FIELDS: absent = !facts.has_form; break;
      case pdfv1::PDF_FAMILY_DEEP_RESOURCES:
        verdict->set_support(pdfv1::FAMILY_SUPPORT_UNSUPPORTED_BY_BACKEND);
        verdict->set_detail("the engine does not type out page resources");
        continue;
      default: break;
    }
    verdict->set_support(absent ? pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT
                                : pdfv1::FAMILY_SUPPORT_SUPPORTED);
  }
}

void SetBox(pdfv1::BoundingBox* box, float left, float bottom, float right,
            float top) {
  box->set_x0(std::min(left, right));
  box->set_y0(std::min(bottom, top));
  box->set_x1(std::max(left, right));
  box->set_y1(std::max(bottom, top));
}

// /MediaBox, /CropBox and /Rotate are inheritable through the page tree
// (ISO 32000-1 7.7.3.4), but FPDFPage_GetMediaBox and FPDFPage_GetCropBox
// read only the page's own dictionary and miss a box set on a /Pages node.
// FPDF_GetPageBoundingBox is the CropBox the renderer uses: inherited,
// clipped to the MediaBox (14.11.2) and falling back to it, which is what
// PageInfo.crop_box names and what the raster covers. Page geometry is
// reported relative to this box (see page_space.h); the boxes themselves
// stay as stored.
void FillPageInfo(FPDF_PAGE page, int index, pdfv1::PageInfo* info) {
  info->set_page_index(static_cast<uint32_t>(index));
  info->set_page_space(pdfv1::PAGE_SPACE_CROP_BOX);
  info->set_width_pts(FPDF_GetPageWidthF(page));
  info->set_height_pts(FPDF_GetPageHeightF(page));
  info->set_rotation_degrees(FPDFPage_GetRotation(page) * 90);
  FS_RECTF crop;
  if (!FPDF_GetPageBoundingBox(page, &crop)) return;
  SetBox(info->mutable_crop_box(), crop.left, crop.bottom, crop.right,
         crop.top);
  float left = 0;
  float bottom = 0;
  float right = 0;
  float top = 0;
  if (FPDFPage_GetMediaBox(page, &left, &bottom, &right, &top)) {
    SetBox(info->mutable_media_box(), left, bottom, right, top);
  } else if (crop.right > crop.left && crop.top > crop.bottom) {
    // An inherited MediaBox has no getter. Widening the CropBox to all of
    // user space makes the renderer's box the MediaBox itself; the CropBox
    // is then set back to the box read above, so the page's effective box
    // (and anything computed from it later) is unchanged. The edit lives
    // only in this request's in-memory document.
    constexpr float kAll = std::numeric_limits<float>::max();
    FPDFPage_SetCropBox(page, -kAll, -kAll, kAll, kAll);
    FS_RECTF media;
    const bool has_media = FPDF_GetPageBoundingBox(page, &media);
    FPDFPage_SetCropBox(page, crop.left, crop.bottom, crop.right, crop.top);
    if (has_media) {
      SetBox(info->mutable_media_box(), media.left, media.bottom, media.right,
             media.top);
    }
  }
}

// The pages [begin, end) a request selects, clamped to the document. The
// arithmetic stays unsigned: the services reject ranges the contract
// forbids, and a range past the last page comes out empty, never negative.
struct PageSpan {
  int begin = 0;
  int end = 0;
};

PageSpan SelectPages(bool has_range, const pdfv1::PageRange& range,
                     int page_count) {
  const uint32_t count = static_cast<uint32_t>(std::max(page_count, 0));
  if (!has_range) return {0, static_cast<int>(count)};
  const uint32_t begin = std::min(range.begin(), count);
  const uint32_t end = std::max(begin, std::min(range.end(), count));
  return {static_cast<int>(begin), static_cast<int>(end)};
}

// Pages loaded in this process; see PdfiumEngine::PageLoads.
uint64_t page_loads = 0;

// A loaded page, closed when it goes out of scope. FPDF_LoadPage parses the
// page's content stream, the expensive part of touching a page, so every
// load in the engine goes through here and is counted.
class ScopedPage {
 public:
  ScopedPage(FPDF_DOCUMENT doc, int index) : page_(FPDF_LoadPage(doc, index)) {
    ++page_loads;
  }
  ~ScopedPage() {
    if (page_ != nullptr) FPDF_ClosePage(page_);
  }
  ScopedPage(const ScopedPage&) = delete;
  ScopedPage& operator=(const ScopedPage&) = delete;

  FPDF_PAGE get() const { return page_; }

 private:
  FPDF_PAGE page_;
};

// The page inventories of recently parsed documents. Every Parse header
// carries the whole inventory, and PDFium exposes /Rotate and the page
// boxes only on a loaded page, so building one costs a load of every page.
// A client that parses a long document one page per call (gRParse does)
// would pay that on every call; with the cache a worker pays it once per
// document.
//
// The key is PdfDocument.sha256 as the front forwards it. The front checks
// a hash that comes with bytes and fills a hash-only request from the bytes
// it cached under that very hash, so within the pool the hash names these
// bytes. The byte size and page count must match too, so a direct caller
// with a wrong hash is not served another document's inventory.
//
// Same threading rule as the rest of the engine: one call at a time.
class InventoryCache {
 public:
  using Pages = google::protobuf::RepeatedPtrField<pdfv1::PageInfo>;

  const Pages* Find(const std::string& sha256, size_t size_bytes,
                    int page_count) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->sha256 != sha256) continue;
      if (it->size_bytes != size_bytes || it->page_count != page_count) {
        return nullptr;
      }
      entries_.splice(entries_.begin(), entries_, it);
      return &entries_.front().pages;
    }
    return nullptr;
  }

  void Insert(const std::string& sha256, size_t size_bytes, int page_count,
              const Pages& pages) {
    if (pages.size() > kMaxPages) return;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->sha256 == sha256) {
        total_pages_ -= it->pages.size();
        entries_.erase(it);
        break;
      }
    }
    while (!entries_.empty() &&
           (entries_.size() >= kMaxDocuments ||
            total_pages_ + pages.size() > kMaxPages)) {
      total_pages_ -= entries_.back().pages.size();
      entries_.pop_back();
    }
    entries_.push_front(Entry{sha256, size_bytes, page_count, pages});
    total_pages_ += pages.size();
  }

 private:
  // About 200 bytes per page: a few tens of MB at the ceiling.
  static constexpr size_t kMaxDocuments = 8;
  static constexpr int kMaxPages = 100000;

  struct Entry {
    std::string sha256;
    size_t size_bytes;
    int page_count;
    Pages pages;
  };
  std::list<Entry> entries_;  // front is most recently used
  int total_pages_ = 0;
};

// Fills the header inventory: every page's PageInfo, from the cache when
// the document hash is known, else by loading each page in turn (one page
// open at a time).
void FillInventory(FPDF_DOCUMENT doc, const pdfv1::PdfDocument& document,
                   int page_count, InventoryCache::Pages* pages) {
  static InventoryCache cache;
  if (document.has_sha256()) {
    if (const auto* cached =
            cache.Find(document.sha256(), document.data().size(), page_count)) {
      *pages = *cached;
      return;
    }
  }
  for (int i = 0; i < page_count; ++i) {
    ScopedPage page(doc, i);
    if (page.get() != nullptr) FillPageInfo(page.get(), i, pages->Add());
  }
  if (document.has_sha256()) {
    cache.Insert(document.sha256(), document.data().size(), page_count, *pages);
  }
}

// Whitespace as the word splitter: ASCII space controls plus the common
// Unicode space code points the text page emits.
bool IsWordBreak(unsigned short unit) {
  if (unit <= 0x20) return true;
  if (unit == 0xA0) return true;
  if (unit >= 0x2000 && unit <= 0x200B) return true;
  return unit == 0x2028 || unit == 0x2029 || unit == 0x3000 || unit == 0xFEFF;
}

// Extracts text cells for one page at word granularity, the same shape the
// in-process poppler path emits: split the character stream on whitespace,
// take the union of the text rects each word occupies, and read font
// identity from the word's first character.
void ExtractTextCells(FPDF_PAGE page, pdfv1::PageChunk* chunk,
                      bool want_programs, FontInterner* fonts,
                      pdfv1::FontTableChunk* new_fonts,
                      std::vector<pdfv1::EmbeddedFont>* embedded_fonts,
                      uint64_t* cell_count) {
  FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
  if (text_page == nullptr) return;
  const int char_count = FPDFText_CountChars(text_page);
  std::vector<unsigned short> units;
  if (char_count > 0) {
    units.resize(static_cast<size_t>(char_count) + 1);
    FPDFText_GetText(text_page, 0, char_count, units.data());
    units.resize(static_cast<size_t>(char_count));
  }

  int start = -1;
  for (int i = 0; i <= char_count; ++i) {
    const bool boundary =
        i == char_count || IsWordBreak(units[static_cast<size_t>(i)]);
    if (!boundary) {
      if (start < 0) start = i;
      continue;
    }
    if (start < 0) continue;
    const int count = i - start;
    std::vector<unsigned short> word(units.begin() + start,
                                     units.begin() + i);

    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
    bool has_box = false;
    // Loose char boxes come from font metrics, not glyph ink, so every word
    // on a line shares the same vertical extent. That uniformity is what
    // downstream geometry sorts rely on (tight ink boxes make "page" start
    // below "1" and reading order scrambles).
    for (int c = start; c < start + count; ++c) {
      FS_RECTF rect;
      if (!FPDFText_GetLooseCharBox(text_page, c, &rect)) continue;
      const double lo_x = std::min(rect.left, rect.right);
      const double hi_x = std::max(rect.left, rect.right);
      const double lo_y = std::min(rect.top, rect.bottom);
      const double hi_y = std::max(rect.top, rect.bottom);
      if (hi_x <= lo_x || hi_y <= lo_y) continue;
      if (!has_box) {
        x0 = lo_x;
        y0 = lo_y;
        x1 = hi_x;
        y1 = hi_y;
        has_box = true;
      } else {
        x0 = std::min(x0, lo_x);
        y0 = std::min(y0, lo_y);
        x1 = std::max(x1, hi_x);
        y1 = std::max(y1, hi_y);
      }
    }
    const int word_start = start;
    start = -1;
    if (!has_box) continue;

    auto* cell = chunk->add_text_cells();
    cell->set_text(Utf16ToUtf8(word));
    auto* bbox = cell->mutable_bbox();
    bbox->set_x0(x0);
    bbox->set_y0(y0);
    bbox->set_x1(x1);
    bbox->set_y1(y1);
    auto* quad = cell->mutable_quad();
    quad->set_x0(x0);
    quad->set_y0(y0);
    quad->set_x1(x1);
    quad->set_y1(y0);
    quad->set_x2(x1);
    quad->set_y2(y1);
    quad->set_x3(x0);
    quad->set_y3(y1);

    cell->set_font_size(FPDFText_GetFontSize(text_page, word_start));
    // Font and render mode come from the text object that draws the word's
    // first character: the same font handle the page-object walk sees, so
    // the cell's font_id is the entry that carries the embedded program.
    FPDF_PAGEOBJECT text_object = FPDFText_GetTextObject(text_page, word_start);
    if (std::optional<uint32_t> font_id = tier12::InternCharFont(
            text_page, word_start, text_object, want_programs, fonts,
            new_fonts, embedded_fonts)) {
      cell->set_font_id(*font_id);
    }
    if (text_object != nullptr) {
      // Tr 0..7 map onto the contract's modes one up; Tr 3 is the invisible
      // OCR underlay.
      static_assert(pdfv1::TEXT_RENDERING_MODE_FILL ==
                    FPDF_TEXTRENDERMODE_FILL + 1);
      static_assert(pdfv1::TEXT_RENDERING_MODE_CLIP ==
                    FPDF_TEXTRENDERMODE_CLIP + 1);
      const FPDF_TEXT_RENDERMODE mode =
          FPDFTextObj_GetTextRenderMode(text_object);
      if (mode >= FPDF_TEXTRENDERMODE_FILL && mode <= FPDF_TEXTRENDERMODE_CLIP) {
        cell->set_rendering_mode(
            static_cast<pdfv1::TextRenderingMode>(mode + 1));
      }
    }
    ++*cell_count;
  }
  FPDFText_ClosePage(text_page);
}

}  // namespace

void PdfiumEngine::InitProcess() {
  static bool initialized = false;
  if (initialized) return;
  FPDF_LIBRARY_CONFIG config;
  std::memset(&config, 0, sizeof(config));
  config.version = 2;
  FPDF_InitLibraryWithConfig(&config);
  initialized = true;
}

const char* PdfiumEngine::BackendName() { return kBackendName; }

const char* PdfiumEngine::EngineVersion() { return kEngineVersion; }

uint64_t PdfiumEngine::PageLoads() { return page_loads; }

void PdfiumEngine::Probe(const pdfv1::PdfDocument& document,
                         pdfv1::BackendCapabilities* caps) {
  LoadedDocument loaded;
  LoadDocument(document, &loaded);
  FillCapabilities(loaded, caps);
}

bool PdfiumEngine::Parse(
    const pdfv1::ParseRequest& request,
    const std::function<bool(const pdfv1::ParseResponse&)>& emit) {
  LoadedDocument loaded;
  LoadDocument(request.document(), &loaded);

  pdfv1::ParseResponse header_msg;
  auto* header = header_msg.mutable_header();
  FillCapabilities(loaded, header->mutable_capabilities());
  const int page_count =
      loaded.status == pdfv1::LOAD_STATUS_OK ? FPDF_GetPageCount(loaded.doc) : 0;
  if (loaded.status == pdfv1::LOAD_STATUS_OK) {
    FillInventory(loaded.doc, request.document(), page_count,
                  header->mutable_pages());
  }
  bool client_ok = emit(header_msg);
  if (!client_ok || loaded.status != pdfv1::LOAD_STATUS_OK) return client_ok;

  const PageSpan span =
      SelectPages(request.has_pages(), request.pages(), page_count);

  const CropOrigins origins = CropOriginsOf(header->pages());
  const tier12::DocFacts facts = tier12::GatherDocFacts(loaded.doc);
  std::vector<pdfv1::ParseWarning> warnings;
  client_ok = tier12::EmitDocLevelFamilies(loaded.doc, request, facts, origins,
                                           emit, &warnings);

  // Form-field access goes through a form-fill environment; a zeroed
  // struct with just the version is the read-only setup.
  FPDF_FORMFILLINFO form_info;
  std::memset(&form_info, 0, sizeof(form_info));
  form_info.version = 2;
  FPDF_FORMHANDLE form_handle = nullptr;
  if (client_ok && tier12::WantFamily(request, pdfv1::PDF_FAMILY_FORM_FIELDS)) {
    form_handle = FPDFDOC_InitFormFillEnvironment(loaded.doc, &form_info);
  }

  const bool want_text =
      tier12::WantFamily(request, pdfv1::PDF_FAMILY_TEXT_CELLS);
  const bool want_programs =
      tier12::WantFamily(request, pdfv1::PDF_FAMILY_EMBEDDED_FONTS);
  FontInterner fonts;
  std::map<pdfv1::PdfFamily, uint64_t> counts;
  counts[pdfv1::PDF_FAMILY_PAGE_INVENTORY] = static_cast<uint64_t>(page_count);
  // Only the requested pages are loaded, one at a time, each closed once
  // its messages are out.
  for (int i = span.begin; client_ok && i < span.end; ++i) {
    ScopedPage loaded_page(loaded.doc, i);
    FPDF_PAGE page = loaded_page.get();
    if (page == nullptr) continue;
    pdfv1::ParseResponse page_msg;
    auto* chunk = page_msg.mutable_page();
    chunk->set_page_index(static_cast<uint32_t>(i));
    pdfv1::FontTableChunk new_fonts;
    std::vector<pdfv1::EmbeddedFont> embedded_fonts;
    if (want_text) {
      ExtractTextCells(page, chunk, want_programs, &fonts, &new_fonts,
                       &embedded_fonts, &counts[pdfv1::PDF_FAMILY_TEXT_CELLS]);
    }
    tier12::ExtractPageTier12(loaded.doc, page, form_handle, request, &fonts,
                              chunk, &new_fonts, &embedded_fonts, &warnings);
    ShiftToCropSpace(origins, chunk);
    counts[pdfv1::PDF_FAMILY_PLACED_IMAGES] += chunk->images_size();
    counts[pdfv1::PDF_FAMILY_HYPERLINKS] += chunk->hyperlinks_size();
    counts[pdfv1::PDF_FAMILY_ANNOTATIONS] += chunk->annotations_size();
    counts[pdfv1::PDF_FAMILY_FORM_FIELDS] += chunk->form_fields_size();
    counts[pdfv1::PDF_FAMILY_VECTOR_SHAPES] += chunk->shapes_size();
    if (chunk->has_thumbnail()) ++counts[pdfv1::PDF_FAMILY_THUMBNAILS];
    if (new_fonts.fonts_size() > 0) {
      counts[pdfv1::PDF_FAMILY_FONTS] +=
          static_cast<uint64_t>(new_fonts.fonts_size());
      pdfv1::ParseResponse fonts_msg;
      *fonts_msg.mutable_fonts() = new_fonts;
      client_ok = emit(fonts_msg);
      if (!client_ok) break;
    }
    client_ok = emit(page_msg);
    for (auto& program : embedded_fonts) {
      if (!client_ok) break;
      pdfv1::ParseResponse font_msg;
      *font_msg.mutable_embedded_font() = std::move(program);
      ++counts[pdfv1::PDF_FAMILY_EMBEDDED_FONTS];
      client_ok = emit(font_msg);
    }
  }

  // The structure tree is a document-level family, so the page range does
  // not apply; PDFium builds it per page, so each page is loaded in turn.
  if (client_ok && facts.tagged &&
      tier12::WantFamily(request, pdfv1::PDF_FAMILY_STRUCT_TREE)) {
    pdfv1::ParseResponse tree_msg;
    auto* tree = tree_msg.mutable_struct_tree();
    for (int i = 0; i < page_count; ++i) {
      ScopedPage page(loaded.doc, i);
      if (page.get() == nullptr) continue;
      tier12::AppendStructTree(page.get(), static_cast<uint32_t>(i), tree,
                               &counts[pdfv1::PDF_FAMILY_STRUCT_TREE]);
    }
    if (tree->roots_size() > 0) client_ok = emit(tree_msg);
  }

  if (form_handle != nullptr) FPDFDOC_ExitFormFillEnvironment(form_handle);
  if (!client_ok) return false;

  pdfv1::ParseResponse trailer_msg;
  auto* trailer = trailer_msg.mutable_trailer();
  for (const auto& [family, count] : counts) {
    auto* entry = trailer->add_counts();
    entry->set_family(family);
    entry->set_count(count);
  }
  for (auto& warning : warnings) *trailer->add_warnings() = std::move(warning);
  return emit(trailer_msg);
}

grpc::Status PdfiumEngine::Render(
    const pdfv1::RenderRequest& request,
    const std::function<bool(const pdfv1::RenderResponse&)>& emit) {
  LoadedDocument loaded;
  LoadDocument(request.document(), &loaded);
  if (loaded.status != pdfv1::LOAD_STATUS_OK) {
    // A load failure is typed in the stream head, never a bare gRPC error:
    // exactly one message, then the stream ends.
    pdfv1::RenderResponse msg;
    auto* head = msg.mutable_head();
    head->set_load_status(loaded.status);
    if (!loaded.detail.empty()) head->set_load_detail(loaded.detail);
    return emit(msg) ? grpc::Status::OK : grpc::Status::CANCELLED;
  }

  const PageSpan span = SelectPages(request.has_pages(), request.pages(),
                                    FPDF_GetPageCount(loaded.doc));

  const bool gray = request.pixel_format() == pdfv1::PIXEL_FORMAT_GRAY8;
  const double scale = request.dpi() / 72.0;
  for (int i = span.begin; i < span.end; ++i) {
    ScopedPage loaded_page(loaded.doc, i);
    FPDF_PAGE page = loaded_page.get();
    if (page == nullptr) continue;
    // Size the raster in floating point and check it against the ceiling
    // before any int conversion or allocation. PDFium pads rows to 4 bytes.
    const double width_px =
        std::max(1.0, std::round(FPDF_GetPageWidthF(page) * scale));
    const double height_px =
        std::max(1.0, std::round(FPDF_GetPageHeightF(page) * scale));
    const double row_bytes = std::ceil(width_px * (gray ? 1 : 3) / 4.0) * 4.0;
    if (!(row_bytes * height_px <= kMaxRasterBytes)) {
      return grpc::Status(
          grpc::StatusCode::RESOURCE_EXHAUSTED,
          "page " + std::to_string(i) + " at " + std::to_string(request.dpi()) +
              " DPI is " + std::to_string(static_cast<int64_t>(width_px)) + "x" +
              std::to_string(static_cast<int64_t>(height_px)) +
              " pixels, above the 512 MiB raster limit");
    }
    const int width = static_cast<int>(width_px);
    const int height = static_cast<int>(height_px);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        width, height, gray ? FPDFBitmap_Gray : FPDFBitmap_BGR, nullptr, 0);
    if (bitmap == nullptr) {
      return grpc::Status(
          grpc::StatusCode::RESOURCE_EXHAUSTED,
          "bitmap allocation failed for page " + std::to_string(i));
    }
    // Background: opaque white unless the request sets one. Gray output
    // collapses the color to its luma via the blue channel of FillRect's
    // ARGB, which PDFium maps onto the 8-bit surface.
    FPDF_DWORD background = 0xFFFFFFFF;
    if (request.has_background()) {
      const auto& c = request.background();
      auto to_byte = [](double v) {
        return static_cast<FPDF_DWORD>(
            std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
      };
      background = (to_byte(c.alpha()) << 24) | (to_byte(c.red()) << 16) |
                   (to_byte(c.green()) << 8) | to_byte(c.blue());
    }
    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, background);
    int flags = request.omit_annotations() ? 0 : FPDF_ANNOT;
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, flags);

    pdfv1::RenderResponse msg;
    auto* raster = msg.mutable_raster();
    raster->set_page_index(static_cast<uint32_t>(i));
    raster->set_width_px(static_cast<uint32_t>(width));
    raster->set_height_px(static_cast<uint32_t>(height));
    int stride = FPDFBitmap_GetStride(bitmap);
    raster->set_stride_bytes(static_cast<uint32_t>(stride));
    raster->set_pixel_format(gray ? pdfv1::PIXEL_FORMAT_GRAY8
                                  : pdfv1::PIXEL_FORMAT_BGR8);
    raster->set_dpi(request.dpi());
    raster->set_pixels(FPDFBitmap_GetBuffer(bitmap),
                       static_cast<size_t>(stride) * height);
    FPDFBitmap_Destroy(bitmap);
    if (!emit(msg)) return grpc::Status::CANCELLED;
  }
  return grpc::Status::OK;
}

}  // namespace grpc_pdfium
