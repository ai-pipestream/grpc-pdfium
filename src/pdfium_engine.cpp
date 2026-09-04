#include "pdfium_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "fpdf_edit.h"
#include "fpdf_formfill.h"
#include "fpdf_text.h"
#include "fpdf_transformpage.h"
#include "fpdfview.h"
#include "pdfium_tier12.h"

namespace grpc_pdfium {

namespace pdfv1 = ai::pipestream::parse::pdf::v1;

namespace {

constexpr char kBackendName[] = "grpc-pdfium";
constexpr char kEngineVersion[] = "pdfium 154.0.8035.0 (chromium/8035)";

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

void FillPageInfo(FPDF_DOCUMENT doc, FPDF_PAGE page, int index,
                  pdfv1::PageInfo* info) {
  (void)doc;
  info->set_page_index(static_cast<uint32_t>(index));
  info->set_width_pts(FPDF_GetPageWidthF(page));
  info->set_height_pts(FPDF_GetPageHeightF(page));
  info->set_rotation_degrees(FPDFPage_GetRotation(page) * 90);
  float left = 0;
  float bottom = 0;
  float right = 0;
  float top = 0;
  if (FPDFPage_GetMediaBox(page, &left, &bottom, &right, &top)) {
    auto* box = info->mutable_media_box();
    box->set_x0(left);
    box->set_y0(bottom);
    box->set_x1(right);
    box->set_y1(top);
  }
  if (FPDFPage_GetCropBox(page, &left, &bottom, &right, &top)) {
    auto* box = info->mutable_crop_box();
    box->set_x0(left);
    box->set_y0(bottom);
    box->set_x1(right);
    box->set_y1(top);
  } else if (info->has_media_box()) {
    *info->mutable_crop_box() = info->media_box();
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
                      FontInterner* fonts, pdfv1::FontTableChunk* new_fonts,
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
    char name_buf[256];
    int flags = 0;
    unsigned long name_len = FPDFText_GetFontInfo(
        text_page, word_start, name_buf, sizeof(name_buf), &flags);
    if (name_len > 0) {
      std::string name(name_buf,
                       std::min<unsigned long>(name_len, sizeof(name_buf)));
      // The reported length includes the trailing NUL.
      while (!name.empty() && name.back() == '\0') name.pop_back();
      bool is_new = false;
      uint32_t id = fonts->Intern(name, flags, &is_new);
      cell->set_font_id(id);
      if (is_new) {
        auto* ref = new_fonts->add_fonts();
        ref->set_font_id(id);
        ref->set_base_name(name);
        ref->set_descriptor_flags(static_cast<uint32_t>(flags));
        // Embedded-program presence lands with the tier 1 font work; the
        // tier 0 table reports identity and descriptor flags only.
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

const char* PdfiumEngine::EngineVersion() { return kEngineVersion; }

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
  int page_count =
      loaded.status == pdfv1::LOAD_STATUS_OK ? FPDF_GetPageCount(loaded.doc) : 0;
  std::vector<FPDF_PAGE> pages(static_cast<size_t>(page_count), nullptr);
  for (int i = 0; i < page_count; ++i) {
    pages[static_cast<size_t>(i)] = FPDF_LoadPage(loaded.doc, i);
    if (pages[static_cast<size_t>(i)] != nullptr) {
      FillPageInfo(loaded.doc, pages[static_cast<size_t>(i)], i,
                   header->add_pages());
    }
  }
  bool client_ok = emit(header_msg);
  if (!client_ok || loaded.status != pdfv1::LOAD_STATUS_OK) {
    for (FPDF_PAGE p : pages) {
      if (p != nullptr) FPDF_ClosePage(p);
    }
    return client_ok;
  }

  int begin = 0;
  int end = page_count;
  if (request.has_pages()) {
    begin = std::min<int>(static_cast<int>(request.pages().begin()), page_count);
    end = std::min<int>(static_cast<int>(request.pages().end()), page_count);
  }

  const tier12::DocFacts facts = tier12::GatherDocFacts(loaded.doc);
  client_ok = tier12::EmitDocLevelFamilies(loaded.doc, request, facts, emit);

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
  FontInterner fonts;
  std::map<pdfv1::PdfFamily, uint64_t> counts;
  counts[pdfv1::PDF_FAMILY_PAGE_INVENTORY] = static_cast<uint64_t>(page_count);
  for (int i = begin; client_ok && i < end; ++i) {
    FPDF_PAGE page = pages[static_cast<size_t>(i)];
    if (page == nullptr) continue;
    pdfv1::ParseResponse page_msg;
    auto* chunk = page_msg.mutable_page();
    chunk->set_page_index(static_cast<uint32_t>(i));
    pdfv1::FontTableChunk new_fonts;
    std::vector<pdfv1::EmbeddedFont> embedded_fonts;
    if (want_text) {
      ExtractTextCells(page, chunk, &fonts, &new_fonts,
                       &counts[pdfv1::PDF_FAMILY_TEXT_CELLS]);
    }
    tier12::ExtractPageTier12(loaded.doc, page, form_handle, request, &fonts,
                              chunk, &new_fonts, &embedded_fonts);
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

  if (client_ok && facts.tagged &&
      tier12::WantFamily(request, pdfv1::PDF_FAMILY_STRUCT_TREE)) {
    client_ok = tier12::EmitStructTree(
        pages, emit, &counts[pdfv1::PDF_FAMILY_STRUCT_TREE]);
  }

  if (form_handle != nullptr) FPDFDOC_ExitFormFillEnvironment(form_handle);
  for (FPDF_PAGE p : pages) {
    if (p != nullptr) FPDF_ClosePage(p);
  }
  if (!client_ok) return false;

  pdfv1::ParseResponse trailer_msg;
  auto* trailer = trailer_msg.mutable_trailer();
  for (const auto& [family, count] : counts) {
    auto* entry = trailer->add_counts();
    entry->set_family(family);
    entry->set_count(count);
  }
  return emit(trailer_msg);
}

bool PdfiumEngine::Render(
    const pdfv1::RenderRequest& request,
    const std::function<bool(const pdfv1::RenderResponse&)>& emit,
    std::string* error_message) {
  if (request.dpi() <= 0.0) {
    *error_message = "dpi must be positive";
    return false;
  }
  LoadedDocument loaded;
  LoadDocument(request.document(), &loaded);
  if (loaded.status != pdfv1::LOAD_STATUS_OK) {
    *error_message = "document did not load: " +
                     pdfv1::LoadStatus_Name(loaded.status) +
                     (loaded.detail.empty() ? "" : " (" + loaded.detail + ")");
    return false;
  }

  int page_count = FPDF_GetPageCount(loaded.doc);
  int begin = 0;
  int end = page_count;
  if (request.has_pages()) {
    begin = std::min<int>(static_cast<int>(request.pages().begin()), page_count);
    end = std::min<int>(static_cast<int>(request.pages().end()), page_count);
  }

  const bool gray = request.pixel_format() == pdfv1::PIXEL_FORMAT_GRAY8;
  const double scale = request.dpi() / 72.0;
  for (int i = begin; i < end; ++i) {
    FPDF_PAGE page = FPDF_LoadPage(loaded.doc, i);
    if (page == nullptr) continue;
    int width = std::max(1, static_cast<int>(
                                std::lround(FPDF_GetPageWidthF(page) * scale)));
    int height = std::max(
        1, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * scale)));
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        width, height, gray ? FPDFBitmap_Gray : FPDFBitmap_BGR, nullptr, 0);
    if (bitmap == nullptr) {
      FPDF_ClosePage(page);
      *error_message = "bitmap allocation failed for page " + std::to_string(i);
      return false;
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
    FPDF_ClosePage(page);
    if (!emit(msg)) return false;
  }
  return true;
}

}  // namespace grpc_pdfium
