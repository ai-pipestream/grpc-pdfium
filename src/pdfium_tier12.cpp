#include "pdfium_tier12.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "fpdf_annot.h"
#include "fpdf_attachment.h"
#include "fpdf_catalog.h"
#include "fpdf_doc.h"
#include "fpdf_edit.h"
#include "fpdf_formfill.h"
#include "fpdf_javascript.h"
#include "fpdf_signature.h"
#include "fpdf_structtree.h"
#include "fpdf_thumbnail.h"
#include "fpdf_transformpage.h"

namespace grpc_pdfium {

namespace pdfv1 = ai::pipestream::parse::pdf::v1;

uint32_t FontInterner::Intern(const std::string& name, int flags,
                              bool* is_new) {
  auto key = std::make_pair(name, flags);
  auto it = ids_.find(key);
  if (it != ids_.end()) {
    *is_new = false;
    return it->second;
  }
  uint32_t id = static_cast<uint32_t>(ids_.size());
  ids_.emplace(key, id);
  *is_new = true;
  return id;
}

namespace tier12 {
namespace {

// UTF-16LE little helper for the many two-call length-then-fill APIs.
// Returns UTF-8; empty when the value is absent.
std::string Utf16Field(
    const std::function<unsigned long(void*, unsigned long)>& fetch) {
  unsigned long bytes = fetch(nullptr, 0);
  if (bytes <= 2) return "";
  std::vector<unsigned char> raw(bytes);
  fetch(raw.data(), bytes);
  std::string out;
  out.reserve(bytes);
  // Units are UTF-16LE; convert with surrogate handling.
  auto unit = [&raw](size_t i) {
    return static_cast<uint32_t>(raw[2 * i]) |
           (static_cast<uint32_t>(raw[2 * i + 1]) << 8);
  };
  size_t units = bytes / 2;
  auto append = [&out](uint32_t cp) {
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
  for (size_t i = 0; i < units; ++i) {
    uint32_t u = unit(i);
    if (u == 0) break;
    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units) {
      uint32_t lo = unit(i + 1);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        append(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
        ++i;
        continue;
      }
    }
    append(u >= 0xD800 && u <= 0xDFFF ? 0xFFFD : u);
  }
  return out;
}

// Byte-string variant for ASCII-ish two-call APIs (URI paths, dates).
std::string ByteField(
    const std::function<unsigned long(void*, unsigned long)>& fetch) {
  unsigned long len = fetch(nullptr, 0);
  if (len <= 1) return "";
  std::string out(len, '\0');
  fetch(out.data(), len);
  while (!out.empty() && out.back() == '\0') out.pop_back();
  return out;
}

void SetBox(pdfv1::BoundingBox* box, float l, float b, float r, float t) {
  box->set_x0(std::min(l, r));
  box->set_y0(std::min(b, t));
  box->set_x1(std::max(l, r));
  box->set_y1(std::max(b, t));
}

void FillDestination(FPDF_DOCUMENT doc, FPDF_DEST dest,
                     pdfv1::PageDestination* out) {
  out->set_page_index(
      static_cast<uint32_t>(std::max(0, FPDFDest_GetDestPageIndex(doc, dest))));
  FPDF_BOOL has_x = 0;
  FPDF_BOOL has_y = 0;
  FPDF_BOOL has_zoom = 0;
  FS_FLOAT x = 0;
  FS_FLOAT y = 0;
  FS_FLOAT zoom = 0;
  if (FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y,
                                 &zoom)) {
    if (has_x) out->set_x(x);
    if (has_y) out->set_y(y);
    if (has_zoom) out->set_zoom(zoom);
  }
}

// ---------------------------------------------------------------------------
// Document-level families
// ---------------------------------------------------------------------------

void FillDocMeta(FPDF_DOCUMENT doc, const DocFacts& facts, pdfv1::DocMeta* meta) {
  auto info = [doc](const char* tag) {
    return Utf16Field([doc, tag](void* buf, unsigned long len) {
      return FPDF_GetMetaText(doc, tag, buf, len);
    });
  };
  std::string v;
  if (!(v = info("Title")).empty()) meta->set_title(v);
  if (!(v = info("Author")).empty()) meta->set_author(v);
  if (!(v = info("Subject")).empty()) meta->set_subject(v);
  if (!(v = info("Keywords")).empty()) meta->set_keywords(v);
  if (!(v = info("Creator")).empty()) meta->set_creator(v);
  if (!(v = info("Producer")).empty()) meta->set_producer(v);
  if (!(v = info("CreationDate")).empty()) meta->set_created_raw(v);
  if (!(v = info("ModDate")).empty()) meta->set_modified_raw(v);
  int version = 0;
  if (FPDF_GetFileVersion(doc, &version) && version > 0) {
    meta->set_pdf_version(std::to_string(version / 10) + "." +
                          std::to_string(version % 10));
  }
  meta->set_tagged(facts.tagged);
  // The engine exposes neither the raw info dictionary nor the XMP packet,
  // so custom keys and xmp_xml stay absent for this backend.
}

void FillEncryption(FPDF_DOCUMENT doc, pdfv1::EncryptionInfo* enc) {
  int revision = FPDF_GetSecurityHandlerRevision(doc);
  if (revision >= 0) enc->set_revision(static_cast<uint32_t>(revision));
  unsigned long perms = FPDF_GetDocPermissions(doc);
  enc->set_permissions_raw(static_cast<uint32_t>(perms));
  enc->set_can_print(perms & (1u << 2));
  enc->set_can_modify(perms & (1u << 3));
  enc->set_can_copy(perms & (1u << 4));
  enc->set_can_annotate(perms & (1u << 5));
  enc->set_can_fill_forms(perms & (1u << 8));
  enc->set_can_copy_for_accessibility(perms & (1u << 9));
  enc->set_can_assemble(perms & (1u << 10));
  enc->set_can_print_high_res(perms & (1u << 11));
}

void FillOutline(FPDF_DOCUMENT doc, FPDF_BOOKMARK bookmark,
                 pdfv1::OutlineNode* node, int depth) {
  node->set_title(Utf16Field([bookmark](void* buf, unsigned long len) {
    return FPDFBookmark_GetTitle(bookmark, buf, len);
  }));
  FPDF_DEST dest = FPDFBookmark_GetDest(doc, bookmark);
  if (dest != nullptr) {
    FillDestination(doc, dest, node->mutable_destination());
  } else if (FPDF_ACTION action = FPDFBookmark_GetAction(bookmark)) {
    unsigned long type = FPDFAction_GetType(action);
    if (type == PDFACTION_URI) {
      node->set_uri(ByteField([doc, action](void* buf, unsigned long len) {
        return FPDFAction_GetURIPath(doc, action, buf, len);
      }));
    } else if (type == PDFACTION_GOTO) {
      if (FPDF_DEST action_dest = FPDFAction_GetDest(doc, action)) {
        FillDestination(doc, action_dest, node->mutable_destination());
      }
    }
  }
  // Malformed outlines can cycle; a depth cap keeps the walk finite.
  if (depth >= 64) return;
  for (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, bookmark);
       child != nullptr; child = FPDFBookmark_GetNextSibling(doc, child)) {
    FillOutline(doc, child, node->add_children(), depth + 1);
  }
}

bool EmitAttachments(FPDF_DOCUMENT doc, bool include_data,
                     const std::function<bool(const pdfv1::ParseResponse&)>& emit) {
  int count = FPDFDoc_GetAttachmentCount(doc);
  for (int i = 0; i < count; ++i) {
    FPDF_ATTACHMENT att = FPDFDoc_GetAttachment(doc, i);
    if (att == nullptr) continue;
    pdfv1::ParseResponse msg;
    auto* meta = msg.mutable_attachment();
    meta->set_name(Utf16Field([att](void* buf, unsigned long len) {
      return FPDFAttachment_GetName(att, static_cast<FPDF_WCHAR*>(buf), len);
    }));
    std::string desc = Utf16Field([att](void* buf, unsigned long len) {
      return FPDFAttachment_GetDescription(att, static_cast<FPDF_WCHAR*>(buf), len);
    });
    if (!desc.empty()) meta->set_description(desc);
    for (const char* key : {"CreationDate", "ModDate"}) {
      if (FPDFAttachment_HasKey(att, key)) {
        std::string date = Utf16Field([att, key](void* buf, unsigned long len) {
          return FPDFAttachment_GetStringValue(att, key,
                                               static_cast<FPDF_WCHAR*>(buf), len);
        });
        if (date.empty()) continue;
        if (std::strcmp(key, "CreationDate") == 0) {
          meta->set_created_raw(date);
        } else {
          meta->set_modified_raw(date);
        }
      }
    }
    unsigned long size = 0;
    if (FPDFAttachment_GetFile(att, nullptr, 0, &size) && size > 0) {
      meta->set_size_bytes(size);
      if (include_data) {
        std::string data(size, '\0');
        unsigned long got = 0;
        if (FPDFAttachment_GetFile(att, data.data(), size, &got)) {
          data.resize(got);
          meta->set_data(data);
        }
      }
    }
    if (!emit(msg)) return false;
  }
  return true;
}

void FillSignatures(FPDF_DOCUMENT doc, pdfv1::SignatureChunk* chunk) {
  int count = FPDF_GetSignatureCount(doc);
  for (int i = 0; i < count; ++i) {
    FPDF_SIGNATURE sig = FPDF_GetSignatureObject(doc, i);
    if (sig == nullptr) continue;
    auto* info = chunk->add_signatures();
    std::string sub_filter = ByteField([sig](void* buf, unsigned long len) {
      return FPDFSignatureObj_GetSubFilter(sig, static_cast<char*>(buf), len);
    });
    if (!sub_filter.empty()) info->set_sub_filter(sub_filter);
    std::string reason = Utf16Field([sig](void* buf, unsigned long len) {
      return FPDFSignatureObj_GetReason(sig, buf, len);
    });
    if (!reason.empty()) info->set_reason(reason);
    std::string time = ByteField([sig](void* buf, unsigned long len) {
      return FPDFSignatureObj_GetTime(sig, static_cast<char*>(buf), len);
    });
    if (!time.empty()) info->set_signing_time_raw(time);
    unsigned int mdp = FPDFSignatureObj_GetDocMDPPermission(sig);
    if (mdp > 0) info->set_doc_mdp_permission(mdp);
    unsigned long blob = FPDFSignatureObj_GetContents(sig, nullptr, 0);
    if (blob > 0) {
      std::string contents(blob, '\0');
      FPDFSignatureObj_GetContents(sig, contents.data(), blob);
      info->set_contents(contents);
    }
    unsigned long ranges = FPDFSignatureObj_GetByteRange(sig, nullptr, 0);
    if (ranges >= 2) {
      std::vector<int> values(ranges);
      FPDFSignatureObj_GetByteRange(sig, values.data(), ranges);
      for (size_t r = 0; r + 1 < values.size(); r += 2) {
        auto* range = info->add_byte_ranges();
        range->set_offset(static_cast<uint64_t>(std::max(0, values[r])));
        range->set_length(static_cast<uint64_t>(std::max(0, values[r + 1])));
      }
    }
  }
}

void FillJavaScript(FPDF_DOCUMENT doc, pdfv1::JavaScriptChunk* chunk) {
  int count = FPDFDoc_GetJavaScriptActionCount(doc);
  for (int i = 0; i < count; ++i) {
    FPDF_JAVASCRIPT_ACTION action = FPDFDoc_GetJavaScriptAction(doc, i);
    if (action == nullptr) continue;
    auto* entry = chunk->add_entries();
    entry->set_name(Utf16Field([action](void* buf, unsigned long len) {
      return FPDFJavaScriptAction_GetName(action, static_cast<FPDF_WCHAR*>(buf), len);
    }));
    entry->set_script(Utf16Field([action](void* buf, unsigned long len) {
      return FPDFJavaScriptAction_GetScript(action, static_cast<FPDF_WCHAR*>(buf), len);
    }));
    FPDFDoc_CloseJavaScriptAction(action);
  }
}

// ---------------------------------------------------------------------------
// Page-level families
// ---------------------------------------------------------------------------

void FillHyperlinks(FPDF_DOCUMENT doc, FPDF_PAGE page, pdfv1::PageChunk* chunk) {
  int pos = 0;
  FPDF_LINK link = nullptr;
  while (FPDFLink_Enumerate(page, &pos, &link)) {
    if (link == nullptr) continue;
    FS_RECTF rect;
    if (!FPDFLink_GetAnnotRect(link, &rect)) continue;
    pdfv1::Hyperlink out;
    SetBox(out.mutable_bbox(), rect.left, rect.bottom, rect.right, rect.top);
    bool typed = false;
    if (FPDF_ACTION action = FPDFLink_GetAction(link)) {
      unsigned long type = FPDFAction_GetType(action);
      if (type == PDFACTION_URI) {
        out.set_uri(ByteField([doc, action](void* buf, unsigned long len) {
          return FPDFAction_GetURIPath(doc, action, buf, len);
        }));
        typed = true;
      } else if (type == PDFACTION_GOTO) {
        if (FPDF_DEST dest = FPDFAction_GetDest(doc, action)) {
          FillDestination(doc, dest, out.mutable_destination());
          typed = true;
        }
      }
    }
    if (!typed) {
      if (FPDF_DEST dest = FPDFLink_GetDest(doc, link)) {
        FillDestination(doc, dest, out.mutable_destination());
        typed = true;
      }
    }
    // A link whose action the engine cannot type is not emitted.
    if (typed) *chunk->add_hyperlinks() = out;
  }
}

struct SubtypeEntry {
  pdfv1::AnnotationKind kind;
  const char* name;
};

// Indexed by the FPDF_ANNOT_* subtype constants.
constexpr std::array<SubtypeEntry, 29> kSubtypes{{
    {pdfv1::ANNOTATION_KIND_UNSPECIFIED, "Unknown"},
    {pdfv1::ANNOTATION_KIND_TEXT, "Text"},
    {pdfv1::ANNOTATION_KIND_LINK, "Link"},
    {pdfv1::ANNOTATION_KIND_FREE_TEXT, "FreeText"},
    {pdfv1::ANNOTATION_KIND_LINE, "Line"},
    {pdfv1::ANNOTATION_KIND_SQUARE, "Square"},
    {pdfv1::ANNOTATION_KIND_CIRCLE, "Circle"},
    {pdfv1::ANNOTATION_KIND_POLYGON, "Polygon"},
    {pdfv1::ANNOTATION_KIND_POLYLINE, "PolyLine"},
    {pdfv1::ANNOTATION_KIND_HIGHLIGHT, "Highlight"},
    {pdfv1::ANNOTATION_KIND_UNDERLINE, "Underline"},
    {pdfv1::ANNOTATION_KIND_SQUIGGLY, "Squiggly"},
    {pdfv1::ANNOTATION_KIND_STRIKEOUT, "StrikeOut"},
    {pdfv1::ANNOTATION_KIND_STAMP, "Stamp"},
    {pdfv1::ANNOTATION_KIND_CARET, "Caret"},
    {pdfv1::ANNOTATION_KIND_INK, "Ink"},
    {pdfv1::ANNOTATION_KIND_POPUP, "Popup"},
    {pdfv1::ANNOTATION_KIND_FILE_ATTACHMENT, "FileAttachment"},
    {pdfv1::ANNOTATION_KIND_SOUND, "Sound"},
    {pdfv1::ANNOTATION_KIND_MOVIE, "Movie"},
    {pdfv1::ANNOTATION_KIND_WIDGET, "Widget"},
    {pdfv1::ANNOTATION_KIND_SCREEN, "Screen"},
    {pdfv1::ANNOTATION_KIND_PRINTER_MARK, "PrinterMark"},
    {pdfv1::ANNOTATION_KIND_TRAP_NET, "TrapNet"},
    {pdfv1::ANNOTATION_KIND_WATERMARK, "Watermark"},
    {pdfv1::ANNOTATION_KIND_THREE_D, "3D"},
    {pdfv1::ANNOTATION_KIND_UNSPECIFIED, "RichMedia"},
    {pdfv1::ANNOTATION_KIND_UNSPECIFIED, "XFAWidget"},
    {pdfv1::ANNOTATION_KIND_REDACT, "Redact"},
}};

void FillAnnotations(FPDF_PAGE page, pdfv1::PageChunk* chunk) {
  int count = FPDFPage_GetAnnotCount(page);
  for (int i = 0; i < count; ++i) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
    if (annot == nullptr) continue;
    auto* out = chunk->add_annotations();
    int subtype = FPDFAnnot_GetSubtype(annot);
    if (subtype >= 0 && subtype < static_cast<int>(kSubtypes.size())) {
      out->set_kind(kSubtypes[static_cast<size_t>(subtype)].kind);
      out->set_subtype_raw(kSubtypes[static_cast<size_t>(subtype)].name);
    } else {
      out->set_kind(pdfv1::ANNOTATION_KIND_UNSPECIFIED);
      out->set_subtype_raw("Unknown");
    }
    FS_RECTF rect;
    if (FPDFAnnot_GetRect(annot, &rect)) {
      SetBox(out->mutable_rect(), rect.left, rect.bottom, rect.right, rect.top);
    }
    auto str = [annot](const char* key) {
      return Utf16Field([annot, key](void* buf, unsigned long len) {
        return FPDFAnnot_GetStringValue(annot, key,
                                        static_cast<FPDF_WCHAR*>(buf), len);
      });
    };
    std::string v;
    if (!(v = str("Contents")).empty()) out->set_contents(v);
    if (!(v = str("T")).empty()) out->set_author(v);
    if (!(v = str("M")).empty()) out->set_modified_raw(v);
    out->set_flags(static_cast<uint32_t>(FPDFAnnot_GetFlags(annot)));
    size_t quad_count = FPDFAnnot_CountAttachmentPoints(annot);
    for (size_t q = 0; q < quad_count; ++q) {
      FS_QUADPOINTSF pts;
      if (!FPDFAnnot_GetAttachmentPoints(annot, q, &pts)) continue;
      auto* quad = out->add_quads();
      quad->set_x0(pts.x3);
      quad->set_y0(pts.y3);
      quad->set_x1(pts.x4);
      quad->set_y1(pts.y4);
      quad->set_x2(pts.x2);
      quad->set_y2(pts.y2);
      quad->set_x3(pts.x1);
      quad->set_y3(pts.y1);
    }
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    unsigned int a = 0;
    if (FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &r, &g, &b, &a)) {
      auto* color = out->mutable_color();
      color->set_red(r / 255.0);
      color->set_green(g / 255.0);
      color->set_blue(b / 255.0);
      color->set_alpha(a / 255.0);
    }
    FPDFPage_CloseAnnot(annot);
  }
}

void FillFormFields(FPDF_FORMHANDLE handle, FPDF_PAGE page,
                    pdfv1::PageChunk* chunk) {
  int count = FPDFPage_GetAnnotCount(page);
  for (int i = 0; i < count; ++i) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
    if (annot == nullptr) continue;
    if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_WIDGET) {
      FPDFPage_CloseAnnot(annot);
      continue;
    }
    auto* field = chunk->add_form_fields();
    static constexpr std::array<pdfv1::FormFieldKind, 8> kFieldKinds{
        pdfv1::FORM_FIELD_KIND_UNSPECIFIED, pdfv1::FORM_FIELD_KIND_PUSH_BUTTON,
        pdfv1::FORM_FIELD_KIND_CHECK_BOX,   pdfv1::FORM_FIELD_KIND_RADIO_BUTTON,
        pdfv1::FORM_FIELD_KIND_COMBO_BOX,   pdfv1::FORM_FIELD_KIND_LIST_BOX,
        pdfv1::FORM_FIELD_KIND_TEXT,        pdfv1::FORM_FIELD_KIND_SIGNATURE};
    int type = FPDFAnnot_GetFormFieldType(handle, annot);
    if (type >= 0 && type < static_cast<int>(kFieldKinds.size())) {
      field->set_kind(kFieldKinds[static_cast<size_t>(type)]);
    }
    auto str = [handle, annot](auto fn) {
      return Utf16Field([handle, annot, fn](void* buf, unsigned long len) {
        return fn(handle, annot, static_cast<FPDF_WCHAR*>(buf), len);
      });
    };
    field->set_name(str(FPDFAnnot_GetFormFieldName));
    std::string v = str(FPDFAnnot_GetFormFieldValue);
    if (!v.empty()) field->set_value(v);
    std::string alt = str(FPDFAnnot_GetFormFieldAlternateName);
    if (!alt.empty()) field->set_alternate_name(alt);
    int flags = FPDFAnnot_GetFormFieldFlags(handle, annot);
    field->set_flags(static_cast<uint32_t>(flags));
    field->set_read_only(flags & 1);
    int options = FPDFAnnot_GetOptionCount(handle, annot);
    for (int o = 0; o < options; ++o) {
      field->add_options(Utf16Field([handle, annot, o](void* buf,
                                                       unsigned long len) {
        return FPDFAnnot_GetOptionLabel(handle, annot, o,
                                        static_cast<FPDF_WCHAR*>(buf), len);
      }));
    }
    FS_RECTF rect;
    if (FPDFAnnot_GetRect(annot, &rect)) {
      SetBox(field->mutable_rect(), rect.left, rect.bottom, rect.right, rect.top);
    }
    FPDFPage_CloseAnnot(annot);
  }
}

void FillQuadFromRotatedBounds(FPDF_PAGEOBJECT obj, pdfv1::Quad* quad,
                               const pdfv1::BoundingBox& fallback) {
  FS_QUADPOINTSF pts;
  if (FPDFPageObj_GetRotatedBounds(obj, &pts)) {
    // Rotated bounds arrive as the four corners in drawing order; map to
    // the contract's lower-left-first convention via the bounding order.
    quad->set_x0(pts.x1);
    quad->set_y0(pts.y1);
    quad->set_x1(pts.x2);
    quad->set_y1(pts.y2);
    quad->set_x2(pts.x3);
    quad->set_y2(pts.y3);
    quad->set_x3(pts.x4);
    quad->set_y3(pts.y4);
    return;
  }
  quad->set_x0(fallback.x0());
  quad->set_y0(fallback.y0());
  quad->set_x1(fallback.x1());
  quad->set_y1(fallback.y0());
  quad->set_x2(fallback.x1());
  quad->set_y2(fallback.y1());
  quad->set_x3(fallback.x0());
  quad->set_y3(fallback.y1());
}

const char* ColorspaceName(int cs) {
  switch (cs) {
    case FPDF_COLORSPACE_DEVICEGRAY: return "DeviceGray";
    case FPDF_COLORSPACE_DEVICERGB: return "DeviceRGB";
    case FPDF_COLORSPACE_DEVICECMYK: return "DeviceCMYK";
    case FPDF_COLORSPACE_CALGRAY: return "CalGray";
    case FPDF_COLORSPACE_CALRGB: return "CalRGB";
    case FPDF_COLORSPACE_LAB: return "Lab";
    case FPDF_COLORSPACE_ICCBASED: return "ICCBased";
    case FPDF_COLORSPACE_SEPARATION: return "Separation";
    case FPDF_COLORSPACE_DEVICEN: return "DeviceN";
    case FPDF_COLORSPACE_INDEXED: return "Indexed";
    case FPDF_COLORSPACE_PATTERN: return "Pattern";
    default: return nullptr;
  }
}

// Copies a PDFium bitmap into an EncodedImage as decoded pixels.
bool BitmapToEncodedImage(FPDF_BITMAP bitmap, pdfv1::EncodedImage* out) {
  int format = FPDFBitmap_GetFormat(bitmap);
  int width = FPDFBitmap_GetWidth(bitmap);
  int height = FPDFBitmap_GetHeight(bitmap);
  int stride = FPDFBitmap_GetStride(bitmap);
  const auto* buffer = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
  if (buffer == nullptr || width <= 0 || height <= 0) return false;
  pdfv1::PixelFormat pixel_format;
  int bytes_per_pixel;
  switch (format) {
    case FPDFBitmap_Gray:
      pixel_format = pdfv1::PIXEL_FORMAT_GRAY8;
      bytes_per_pixel = 1;
      break;
    case FPDFBitmap_BGR:
      pixel_format = pdfv1::PIXEL_FORMAT_BGR8;
      bytes_per_pixel = 3;
      break;
    case FPDFBitmap_BGRA:
      pixel_format = pdfv1::PIXEL_FORMAT_BGRA8;
      bytes_per_pixel = 4;
      break;
    case FPDFBitmap_BGRx: {
      // Drop the padding byte so the payload is honest BGR8.
      std::string pixels;
      pixels.reserve(static_cast<size_t>(width) * height * 3);
      for (int y = 0; y < height; ++y) {
        const unsigned char* row = buffer + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
          pixels.append(reinterpret_cast<const char*>(row + x * 4), 3);
        }
      }
      out->set_encoding(pdfv1::IMAGE_ENCODING_PIXELS);
      out->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
      out->set_width_px(static_cast<uint32_t>(width));
      out->set_height_px(static_cast<uint32_t>(height));
      out->set_stride_bytes(static_cast<uint32_t>(width * 3));
      out->set_data(pixels);
      return true;
    }
    default:
      return false;
  }
  out->set_encoding(pdfv1::IMAGE_ENCODING_PIXELS);
  out->set_pixel_format(pixel_format);
  out->set_width_px(static_cast<uint32_t>(width));
  out->set_height_px(static_cast<uint32_t>(height));
  out->set_stride_bytes(static_cast<uint32_t>(width * bytes_per_pixel));
  std::string pixels;
  pixels.reserve(static_cast<size_t>(width) * height * bytes_per_pixel);
  for (int y = 0; y < height; ++y) {
    pixels.append(
        reinterpret_cast<const char*>(buffer + static_cast<size_t>(y) * stride),
        static_cast<size_t>(width) * bytes_per_pixel);
  }
  out->set_data(pixels);
  return true;
}

void FillImage(FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_PAGEOBJECT obj,
               bool include_data, pdfv1::PageChunk* chunk) {
  (void)doc;
  auto* image = chunk->add_images();
  float l = 0;
  float b = 0;
  float r = 0;
  float t = 0;
  if (FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) {
    SetBox(image->mutable_bbox(), l, b, r, t);
  }
  FillQuadFromRotatedBounds(obj, image->mutable_quad(), image->bbox());
  FPDF_IMAGEOBJ_METADATA meta;
  if (FPDFImageObj_GetImageMetadata(obj, page, &meta)) {
    image->set_source_width_px(meta.width);
    image->set_source_height_px(meta.height);
    if (meta.bits_per_pixel > 0) image->set_bits_per_component(meta.bits_per_pixel);
    if (const char* cs = ColorspaceName(meta.colorspace)) {
      image->set_colorspace(cs);
    }
  }
  if (include_data) {
    FPDF_BITMAP bitmap = FPDFImageObj_GetBitmap(obj);
    if (bitmap != nullptr) {
      BitmapToEncodedImage(bitmap, image->mutable_image());
      FPDFBitmap_Destroy(bitmap);
    }
  }
}

void FillShape(FPDF_PAGEOBJECT obj, pdfv1::PageChunk* chunk) {
  auto* shape = chunk->add_shapes();
  FS_MATRIX m{1, 0, 0, 1, 0, 0};
  FPDFPageObj_GetMatrix(obj, &m);
  auto transform = [&m](float x, float y, pdfv1::PathPoint* out) {
    out->set_x(m.a * x + m.c * y + m.e);
    out->set_y(m.b * x + m.d * y + m.f);
  };
  int segments = FPDFPath_CountSegments(obj);
  int bezier_phase = 0;
  pdfv1::CubicBezier* pending_bezier = nullptr;
  for (int s = 0; s < segments; ++s) {
    FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(obj, s);
    if (seg == nullptr) continue;
    float x = 0;
    float y = 0;
    FPDFPathSegment_GetPoint(seg, &x, &y);
    int type = FPDFPathSegment_GetType(seg);
    if (type == FPDF_SEGMENT_BEZIERTO) {
      // A cubic arrives as three BEZIERTO segments: two control points
      // then the end point.
      if (bezier_phase == 0) {
        pending_bezier = shape->add_segments()->mutable_cubic_to();
        transform(x, y, pending_bezier->mutable_control1());
        bezier_phase = 1;
      } else if (bezier_phase == 1) {
        transform(x, y, pending_bezier->mutable_control2());
        bezier_phase = 2;
      } else {
        transform(x, y, pending_bezier->mutable_end());
        bezier_phase = 0;
        pending_bezier = nullptr;
      }
      continue;
    }
    bezier_phase = 0;
    pending_bezier = nullptr;
    auto* out_seg = shape->add_segments();
    if (type == FPDF_SEGMENT_MOVETO) {
      transform(x, y, out_seg->mutable_move_to());
    } else {
      transform(x, y, out_seg->mutable_line_to());
    }
    if (FPDFPathSegment_GetClose(seg)) {
      shape->add_segments()->set_close(true);
    }
  }
  int fill_mode = 0;
  FPDF_BOOL stroke = 0;
  if (FPDFPath_GetDrawMode(obj, &fill_mode, &stroke)) {
    bool fills = fill_mode != FPDF_FILLMODE_NONE;
    if (fills && stroke) {
      shape->set_paint_mode(pdfv1::PATH_PAINT_MODE_FILL_STROKE);
    } else if (fills) {
      shape->set_paint_mode(pdfv1::PATH_PAINT_MODE_FILL);
    } else if (stroke) {
      shape->set_paint_mode(pdfv1::PATH_PAINT_MODE_STROKE);
    } else {
      shape->set_paint_mode(pdfv1::PATH_PAINT_MODE_CLIP);
    }
    if (fills) {
      shape->set_fill_rule(fill_mode == FPDF_FILLMODE_ALTERNATE
                               ? pdfv1::FILL_RULE_EVEN_ODD
                               : pdfv1::FILL_RULE_NONZERO);
    }
  }
  unsigned int cr = 0;
  unsigned int cg = 0;
  unsigned int cb = 0;
  unsigned int ca = 0;
  if (FPDFPageObj_GetFillColor(obj, &cr, &cg, &cb, &ca)) {
    auto* c = shape->mutable_fill_color();
    c->set_red(cr / 255.0);
    c->set_green(cg / 255.0);
    c->set_blue(cb / 255.0);
    c->set_alpha(ca / 255.0);
  }
  if (FPDFPageObj_GetStrokeColor(obj, &cr, &cg, &cb, &ca)) {
    auto* c = shape->mutable_stroke_color();
    c->set_red(cr / 255.0);
    c->set_green(cg / 255.0);
    c->set_blue(cb / 255.0);
    c->set_alpha(ca / 255.0);
  }
  float width = 0;
  if (FPDFPageObj_GetStrokeWidth(obj, &width)) shape->set_line_width(width);
  float l = 0;
  float b = 0;
  float r = 0;
  float t = 0;
  if (FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) {
    SetBox(shape->mutable_bbox(), l, b, r, t);
  }
}

void FillEmbeddedFontsFromObject(
    FPDF_PAGEOBJECT obj, FontInterner* fonts,
    pdfv1::FontTableChunk* new_fonts,
    std::vector<pdfv1::EmbeddedFont>* embedded_fonts) {
  FPDF_FONT font = FPDFTextObj_GetFont(obj);
  if (font == nullptr) return;
  char name_buf[256];
  size_t name_len = FPDFFont_GetBaseFontName(font, name_buf, sizeof(name_buf));
  if (name_len == 0) return;
  std::string name(name_buf);
  int flags = FPDFFont_GetFlags(font);
  bool is_new = false;
  uint32_t id = fonts->Intern(name, flags, &is_new);
  if (!is_new) return;
  auto* ref = new_fonts->add_fonts();
  ref->set_font_id(id);
  ref->set_base_name(name);
  if (flags >= 0) ref->set_descriptor_flags(static_cast<uint32_t>(flags));
  char family_buf[256];
  if (FPDFFont_GetFamilyName(font, family_buf, sizeof(family_buf)) > 1) {
    ref->set_family(family_buf);
  }
  ref->set_embedded(FPDFFont_GetIsEmbedded(font) != 0);
  if (ref->embedded()) {
    size_t size = 0;
    if (FPDFFont_GetFontData(font, nullptr, 0, &size) && size > 0) {
      std::vector<uint8_t> data(size);
      if (FPDFFont_GetFontData(font, data.data(), size, &size)) {
        pdfv1::EmbeddedFont program;
        program.set_font_id(id);
        program.set_program(std::string(data.begin(), data.end()));
        embedded_fonts->push_back(std::move(program));
      }
    }
  }
}

void FillThumbnail(FPDF_PAGE page, pdfv1::PageChunk* chunk) {
  FPDF_BITMAP bitmap = FPDFPage_GetThumbnailAsBitmap(page);
  if (bitmap == nullptr) return;
  BitmapToEncodedImage(bitmap, chunk->mutable_thumbnail());
  FPDFBitmap_Destroy(bitmap);
}

void FillStructElement(FPDF_STRUCTELEMENT elem, uint32_t page_index,
                       pdfv1::StructNode* node, int depth, uint64_t* count) {
  ++*count;
  auto str = [elem](auto fn) {
    return Utf16Field([elem, fn](void* buf, unsigned long len) {
      return fn(elem, buf, len);
    });
  };
  node->set_role(str(FPDF_StructElement_GetType));
  std::string v;
  if (!(v = str(FPDF_StructElement_GetTitle)).empty()) node->set_title(v);
  if (!(v = str(FPDF_StructElement_GetAltText)).empty()) node->set_alt_text(v);
  if (!(v = str(FPDF_StructElement_GetActualText)).empty()) {
    node->set_actual_text(v);
  }
  if (!(v = str(FPDF_StructElement_GetLang)).empty()) node->set_lang(v);
  int mcid_count = FPDF_StructElement_GetMarkedContentIdCount(elem);
  for (int m = 0; m < mcid_count; ++m) {
    int mcid = FPDF_StructElement_GetMarkedContentIdAtIndex(elem, m);
    if (mcid < 0) continue;
    auto* ref = node->add_content();
    ref->set_page_index(page_index);
    ref->set_mcid(static_cast<uint32_t>(mcid));
  }
  if (depth >= 64) return;
  int children = FPDF_StructElement_CountChildren(elem);
  for (int c = 0; c < children; ++c) {
    FPDF_STRUCTELEMENT child = FPDF_StructElement_GetChildAtIndex(elem, c);
    if (child == nullptr) continue;
    FillStructElement(child, page_index, node->add_children(), depth + 1, count);
  }
}

}  // namespace

DocFacts GatherDocFacts(FPDF_DOCUMENT doc) {
  DocFacts facts;
  facts.tagged = FPDFCatalog_IsTagged(doc) != 0;
  facts.encrypted = FPDF_GetSecurityHandlerRevision(doc) >= 0;
  facts.has_outline = FPDFBookmark_GetFirstChild(doc, nullptr) != nullptr;
  facts.signature_count = FPDF_GetSignatureCount(doc);
  facts.javascript_count = FPDFDoc_GetJavaScriptActionCount(doc);
  facts.attachment_count = FPDFDoc_GetAttachmentCount(doc);
  return facts;
}

bool WantFamily(const pdfv1::ParseRequest& request, pdfv1::PdfFamily family) {
  if (request.families().empty()) return true;
  return std::find(request.families().begin(), request.families().end(),
                   family) != request.families().end();
}

bool EmitDocLevelFamilies(
    FPDF_DOCUMENT doc, const pdfv1::ParseRequest& request, const DocFacts& facts,
    const std::function<bool(const pdfv1::ParseResponse&)>& emit) {
  if (WantFamily(request, pdfv1::PDF_FAMILY_DOC_METADATA)) {
    pdfv1::ParseResponse msg;
    FillDocMeta(doc, facts, msg.mutable_doc_meta());
    if (!emit(msg)) return false;
  }
  if (facts.encrypted && WantFamily(request, pdfv1::PDF_FAMILY_ENCRYPTION_INFO)) {
    pdfv1::ParseResponse msg;
    FillEncryption(doc, msg.mutable_encryption());
    if (!emit(msg)) return false;
  }
  if (facts.has_outline && WantFamily(request, pdfv1::PDF_FAMILY_OUTLINE)) {
    pdfv1::ParseResponse msg;
    auto* chunk = msg.mutable_outline();
    for (FPDF_BOOKMARK bookmark = FPDFBookmark_GetFirstChild(doc, nullptr);
         bookmark != nullptr;
         bookmark = FPDFBookmark_GetNextSibling(doc, bookmark)) {
      FillOutline(doc, bookmark, chunk->add_roots(), 0);
    }
    if (!emit(msg)) return false;
  }
  if (facts.attachment_count > 0 &&
      WantFamily(request, pdfv1::PDF_FAMILY_ATTACHMENTS)) {
    if (!EmitAttachments(doc, request.options().include_attachment_data(), emit)) {
      return false;
    }
  }
  if (facts.signature_count > 0 &&
      WantFamily(request, pdfv1::PDF_FAMILY_SIGNATURES)) {
    pdfv1::ParseResponse msg;
    FillSignatures(doc, msg.mutable_signatures());
    if (!emit(msg)) return false;
  }
  if (facts.javascript_count > 0 &&
      WantFamily(request, pdfv1::PDF_FAMILY_JAVASCRIPT)) {
    pdfv1::ParseResponse msg;
    FillJavaScript(doc, msg.mutable_javascript());
    if (!emit(msg)) return false;
  }
  return true;
}

void ExtractPageTier12(FPDF_DOCUMENT doc, FPDF_PAGE page,
                       FPDF_FORMHANDLE form_handle,
                       const pdfv1::ParseRequest& request, FontInterner* fonts,
                       pdfv1::PageChunk* chunk, pdfv1::FontTableChunk* new_fonts,
                       std::vector<pdfv1::EmbeddedFont>* embedded_fonts) {
  const bool want_images = WantFamily(request, pdfv1::PDF_FAMILY_PLACED_IMAGES);
  const bool want_shapes = WantFamily(request, pdfv1::PDF_FAMILY_VECTOR_SHAPES);
  const bool want_fonts = WantFamily(request, pdfv1::PDF_FAMILY_FONTS) ||
                          WantFamily(request, pdfv1::PDF_FAMILY_EMBEDDED_FONTS);
  if (want_images || want_shapes || want_fonts) {
    int objects = FPDFPage_CountObjects(page);
    for (int i = 0; i < objects; ++i) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
      if (obj == nullptr) continue;
      switch (FPDFPageObj_GetType(obj)) {
        case FPDF_PAGEOBJ_IMAGE:
          if (want_images) {
            FillImage(doc, page, obj, request.options().include_image_data(),
                      chunk);
          }
          break;
        case FPDF_PAGEOBJ_PATH:
          if (want_shapes) FillShape(obj, chunk);
          break;
        case FPDF_PAGEOBJ_TEXT:
          if (want_fonts) {
            FillEmbeddedFontsFromObject(obj, fonts, new_fonts, embedded_fonts);
          }
          break;
        default:
          break;
      }
    }
  }
  if (WantFamily(request, pdfv1::PDF_FAMILY_HYPERLINKS)) {
    FillHyperlinks(doc, page, chunk);
  }
  if (WantFamily(request, pdfv1::PDF_FAMILY_ANNOTATIONS)) {
    FillAnnotations(page, chunk);
  }
  if (form_handle != nullptr &&
      WantFamily(request, pdfv1::PDF_FAMILY_FORM_FIELDS)) {
    FillFormFields(form_handle, page, chunk);
  }
  if (WantFamily(request, pdfv1::PDF_FAMILY_THUMBNAILS)) {
    FillThumbnail(page, chunk);
  }
}

bool EmitStructTree(const std::vector<FPDF_PAGE>& pages,
                    const std::function<bool(const pdfv1::ParseResponse&)>& emit,
                    uint64_t* node_count) {
  pdfv1::ParseResponse msg;
  auto* chunk = msg.mutable_struct_tree();
  for (size_t p = 0; p < pages.size(); ++p) {
    if (pages[p] == nullptr) continue;
    FPDF_STRUCTTREE tree = FPDF_StructTree_GetForPage(pages[p]);
    if (tree == nullptr) continue;
    int children = FPDF_StructTree_CountChildren(tree);
    for (int c = 0; c < children; ++c) {
      FPDF_STRUCTELEMENT elem = FPDF_StructTree_GetChildAtIndex(tree, c);
      if (elem == nullptr) continue;
      FillStructElement(elem, static_cast<uint32_t>(p), chunk->add_roots(), 0,
                        node_count);
    }
    FPDF_StructTree_Close(tree);
  }
  if (chunk->roots().empty()) return true;
  return emit(msg);
}

}  // namespace tier12
}  // namespace grpc_pdfium
