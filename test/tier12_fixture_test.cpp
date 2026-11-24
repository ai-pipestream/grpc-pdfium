// The M2 gate: every family the backend claims in Probe is exercised by a
// fixture. rich.pdf carries the page-scoped and document-scoped families,
// signed.pdf the signature field, the encrypted pair the encryption info
// and the password load statuses, page-tree.pdf the page geometry a page
// inherits through the page tree, form-xobject.pdf content nested in Form
// XObjects and invisible text, huge-image.pdf the image decode limit,
// font-names.pdf font names that are not valid UTF-8, and byte-strings.pdf
// link and outline URIs and signature fields that are not valid UTF-8.

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "page_space.h"
#include "parse_stream_checks.h"
#include "pdf_backend_service_impl.h"
#include "pdfium_engine.h"
#include "sha256.h"
#include "utf8.h"

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// Writes numbered objects into a PDF with a classic cross-reference table.
std::string AssemblePdf(const std::vector<std::string>& objects) {
  std::string out = "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) +
         "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    char line[32];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset);
    out += line;
  }
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return out;
}

// One blank page and an outline whose sibling chain is a loop: item A's
// /Next is B and B's /Next is A again, and A's /First is A itself. A
// walker that follows /Next and /First without remembering what it has
// seen never finishes.
std::string OutlineCyclePdf() {
  return AssemblePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
      "<< /Type /Outlines /First 5 0 R /Last 6 0 R /Count 2 >>",
      "<< /Title (A) /Parent 4 0 R /Next 6 0 R /First 5 0 R /Last 5 0 R "
      "/Count 1 >>",
      "<< /Title (B) /Parent 4 0 R /Prev 5 0 R /Next 5 0 R >>",
  });
}

// Parses the outline of pdf in a child process and reports how many roots
// it produced, or -1 when the child did not finish within the limit. The
// child runs the engine directly, before this process starts a server, so
// a walk that never ends costs the child alone: its alarm ends it.
int OutlineRootsInChild(const std::string& pdf, unsigned limit_seconds) {
  const pid_t child = fork();
  if (child == 0) {
    alarm(limit_seconds);
    grpc_pdfium::PdfiumEngine::InitProcess();
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(pdf);
    request.add_families(pdfv1::PDF_FAMILY_OUTLINE);
    int roots = 0;
    grpc_pdfium::PdfiumEngine::Parse(request, [&roots](const pdfv1::ParseResponse& msg) {
      if (msg.has_outline()) roots = msg.outline().roots_size();
      return true;
    });
    _exit(roots >= 0 && roots < 100 ? roots : 100);
  }
  if (child < 0) return -1;
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

// A page tree whose /Count says 3 while it holds one page: PDFium reports
// three pages and loads only page 0. Two spare objects keep the count
// under the number of objects.
std::string MissingPagesPdf() {
  const std::string content = "BT /F1 24 Tf 72 700 Td (Hello) Tj ET";
  return AssemblePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 3 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
          content + "\nendstream",
      "<< >>",
      "<< >>",
  });
}

pdfv1::FamilySupport SupportOf(const pdfv1::BackendCapabilities& caps,
                               pdfv1::PdfFamily family) {
  for (const auto& f : caps.families()) {
    if (f.family() == family) return f.support();
  }
  return pdfv1::FAMILY_SUPPORT_UNSPECIFIED;
}

// Two pages, plus a page object (10) that is in no page tree, the way a
// file looks after pages were extracted from a larger one. The outline has
// item "Gone" pointing at the orphan and item "Kept" pointing at page 1.
// Page 0 carries four links: a GoTo action to the orphan, a direct /Dest
// to the orphan, a /Dest naming page number 7 of a two-page file, and a
// GoTo action to page 1.
std::string DanglingDestinationsPdf() {
  return AssemblePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Annots [8 0 R 9 0 R 11 0 R 12 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
      "<< /Type /Outlines /First 6 0 R /Last 7 0 R /Count 2 >>",
      "<< /Title (Gone) /Parent 5 0 R /Next 7 0 R "
      "/Dest [10 0 R /XYZ 10 20 0] >>",
      "<< /Title (Kept) /Parent 5 0 R /Prev 6 0 R "
      "/Dest [4 0 R /XYZ 30 40 0] >>",
      "<< /Type /Annot /Subtype /Link /Rect [10 10 50 30] "
      "/A << /S /GoTo /D [10 0 R /Fit] >> >>",
      "<< /Type /Annot /Subtype /Link /Rect [10 40 50 60] "
      "/Dest [10 0 R /Fit] >>",
      "<< /Type /Page /MediaBox [0 0 612 792] >>",
      "<< /Type /Annot /Subtype /Link /Rect [10 70 50 90] /Dest [7 /Fit] >>",
      "<< /Type /Annot /Subtype /Link /Rect [10 100 50 120] "
      "/A << /S /GoTo /D [4 0 R /XYZ 30 40 0] >> >>",
  });
}


std::string StreamObject(const std::string& dict, const std::string& data) {
  return "<< " + dict + " /Length " + std::to_string(data.size()) +
         " >>\nstream\n" + data + "\nendstream";
}

// One page that draws image /Im1 and has a /Thumb. Both are DCT streams
// that hold no JPEG, so PDFium lists the image and has no pixels for
// either.
std::string UndecodableImagesPdf() {
  const std::string image =
      "/Type /XObject /Subtype /Image /Width 4 /Height 4 "
      "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode";
  return AssemblePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /XObject << /Im1 5 0 R >> >> /Contents 4 0 R "
      "/Thumb 6 0 R >>",
      StreamObject("", "q 100 0 0 100 50 50 cm /Im1 Do Q"),
      StreamObject(image, "this is not a JPEG"),
      StreamObject(image, "neither is this one"),
  });
}

// Three entries in the embedded files name tree: a file specification with
// no embedded stream (it names an external file), a reference to an object
// the file does not hold, and a file whose stream is empty.
std::string AttachmentsWithoutDataPdf() {
  return AssemblePdf({
      "<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles << /Names "
      "[(empty.txt) 5 0 R (external.txt) 4 0 R (gone.txt) 99 0 R] >> >> >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
      "<< /Type /Filespec /F (external.txt) /UF (external.txt) >>",
      "<< /Type /Filespec /F (empty.txt) /UF (empty.txt) "
      "/EF << /F 6 0 R >> >>",
      StreamObject("/Type /EmbeddedFile", ""),
  });
}

// An outline that is one chain: every item is the only child of the one
// before it, levels items deep.
std::string DeepOutlinePdf(int levels) {
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
      "<< /Type /Outlines /First 5 0 R /Last 5 0 R /Count 1 >>",
  };
  for (int i = 0; i < levels; ++i) {
    const int self = 5 + i;
    std::string item = "<< /Title (L" + std::to_string(i) + ") /Parent " +
                       std::to_string(self - 1) + " 0 R";
    if (i + 1 < levels) {
      const std::string child = std::to_string(self + 1) + " 0 R";
      item += " /First " + child + " /Last " + child + " /Count 1";
    }
    objects.push_back(item + " >>");
  }
  return AssemblePdf(objects);
}

// One page that draws form /F0, which draws /F1, and so on, levels forms
// deep; the innermost draws a 1x1 gray image.
std::string NestedFormsPdf(int levels) {
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /XObject << /F0 6 0 R >> >> /Contents 4 0 R >>",
      StreamObject("", "q /F0 Do Q"),
      StreamObject("/Type /XObject /Subtype /Image /Width 1 /Height 1 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 8",
                   "\x80"),
  };
  for (int i = 0; i < levels; ++i) {
    const bool last = i + 1 == levels;
    const std::string name = last ? "Im1" : "F" + std::to_string(i + 1);
    const std::string target = std::to_string(last ? 5 : 7 + i) + " 0 R";
    objects.push_back(StreamObject(
        "/Type /XObject /Subtype /Form /BBox [0 0 612 792] "
        "/Resources << /XObject << /" + name + " " + target + " >> >>",
        last ? "q 10 0 0 10 20 20 cm /Im1 Do Q" : "q /" + name + " Do Q"));
  }
  return AssemblePdf(objects);
}

// A tagged page whose one marked-content run belongs to the innermost of
// levels structure elements nested one inside the other.
std::string DeepStructTreePdf(int levels) {
  const std::string content =
      "/P << /MCID 0 >> BDC BT /F1 12 Tf 72 700 Td (Deep) Tj ET EMC";
  const int first = 8;
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R /MarkInfo << /Marked true >> "
      "/StructTreeRoot 6 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R "
      "/StructParents 0 >>",
      StreamObject("", content),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /StructTreeRoot /K " + std::to_string(first) +
          " 0 R /ParentTree 7 0 R >>",
      "<< /Nums [0 [" + std::to_string(first + levels - 1) + " 0 R]] >>",
  };
  for (int i = 0; i < levels; ++i) {
    const int self = first + i;
    const std::string parent = std::to_string(i == 0 ? 6 : self - 1) + " 0 R";
    const bool last = i + 1 == levels;
    objects.push_back("<< /Type /StructElem /S /" +
                      std::string(last ? "P" : "Sect") + " /P " + parent +
                      " /Pg 3 0 R /K " +
                      (last ? std::string("0")
                            : std::to_string(self + 1) + " 0 R") +
                      " >>");
  }
  return AssemblePdf(objects);
}

// One page with three annotations: a subtype PDFium has no number for, one
// it does, and one with no /Subtype at all.
std::string AnnotationSubtypesPdf() {
  return AssemblePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Annots [4 0 R 5 0 R 6 0 R] >>",
      "<< /Type /Annot /Subtype /Projection /Rect [10 10 50 30] >>",
      "<< /Type /Annot /Subtype /Square /Rect [10 40 50 60] >>",
      "<< /Type /Annot /Rect [10 70 50 90] >>",
  });
}


struct ParsedStream {
  pdfv1::ParseHeader header;
  pdfv1::DocMeta doc_meta;
  bool has_doc_meta = false;
  pdfv1::EncryptionInfo encryption;
  bool has_encryption = false;
  pdfv1::OutlineChunk outline;
  pdfv1::PageChunk page;
  bool has_page = false;
  std::vector<pdfv1::AttachmentMeta> attachments;
  pdfv1::SignatureChunk signatures;
  pdfv1::JavaScriptChunk javascript;
  pdfv1::StructTreeChunk struct_tree;
  std::vector<pdfv1::EmbeddedFont> embedded_fonts;
  std::vector<pdfv1::FontRef> fonts;
  std::map<int, uint64_t> counts;
  std::vector<pdfv1::ParseWarning> warnings;

  // Warnings of one family whose message holds text; page -1 means the
  // warning names no page.
  int Warned(pdfv1::PdfFamily family, int page, const char* text) const {
    int found = 0;
    for (const auto& w : warnings) {
      if (w.family() != family) continue;
      if (page < 0 ? w.has_page_index()
                   : !w.has_page_index() ||
                         w.page_index() != static_cast<uint32_t>(page)) {
        continue;
      }
      if (w.message().find(text) != std::string::npos) ++found;
    }
    return found;
  }
};

ParsedStream ParseAll(pdfv1::PdfBackendService::Stub* stub,
                      const std::string& data, bool heavy) {
  ParsedStream out;
  grpc::ClientContext ctx;
  pdfv1::ParseRequest request;
  request.mutable_document()->set_data(data);
  if (heavy) {
    request.mutable_options()->set_include_image_data(true);
    request.mutable_options()->set_include_attachment_data(true);
  }
  auto reader = stub->Parse(&ctx, request);
  pdfv1::ParseResponse msg;
  while (reader->Read(&msg)) {
    switch (msg.payload_case()) {
      case pdfv1::ParseResponse::kHeader: out.header = msg.header(); break;
      case pdfv1::ParseResponse::kDocMeta:
        out.doc_meta = msg.doc_meta();
        out.has_doc_meta = true;
        break;
      case pdfv1::ParseResponse::kEncryption:
        out.encryption = msg.encryption();
        out.has_encryption = true;
        break;
      case pdfv1::ParseResponse::kOutline: out.outline = msg.outline(); break;
      case pdfv1::ParseResponse::kPage:
        out.page = msg.page();
        out.has_page = true;
        break;
      case pdfv1::ParseResponse::kAttachment:
        out.attachments.push_back(msg.attachment());
        break;
      case pdfv1::ParseResponse::kSignatures:
        out.signatures = msg.signatures();
        break;
      case pdfv1::ParseResponse::kJavascript:
        out.javascript = msg.javascript();
        break;
      case pdfv1::ParseResponse::kStructTree:
        out.struct_tree = msg.struct_tree();
        break;
      case pdfv1::ParseResponse::kEmbeddedFont:
        out.embedded_fonts.push_back(msg.embedded_font());
        break;
      case pdfv1::ParseResponse::kFonts:
        for (const auto& f : msg.fonts().fonts()) out.fonts.push_back(f);
        break;
      case pdfv1::ParseResponse::kTrailer:
        for (const auto& c : msg.trailer().counts()) {
          out.counts[c.family()] = c.count();
        }
        for (const auto& w : msg.trailer().warnings()) {
          out.warnings.push_back(w);
        }
        break;
      default: break;
    }
  }
  Check(reader->Finish().ok(), "Parse stream finished OK");
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const std::string rich = ReadFile(dir + "/rich.pdf");
  const std::string signed_doc = ReadFile(dir + "/signed.pdf");
  const std::string enc_open = ReadFile(dir + "/encrypted-open.pdf");
  const std::string enc_locked = ReadFile(dir + "/encrypted-locked.pdf");
  const std::string page_tree = ReadFile(dir + "/page-tree.pdf");
  const std::string form_xobject = ReadFile(dir + "/form-xobject.pdf");
  const std::string huge_image = ReadFile(dir + "/huge-image.pdf");
  const std::string font_names = ReadFile(dir + "/font-names.pdf");
  const std::string byte_strings = ReadFile(dir + "/byte-strings.pdf");
  const std::string control_glyphs = ReadFile(dir + "/control-glyphs.pdf");
  Check(!rich.empty() && !signed_doc.empty() && !enc_open.empty() &&
            !enc_locked.empty() && !page_tree.empty() && !form_xobject.empty() &&
            !huge_image.empty() && !font_names.empty() && !byte_strings.empty() && !control_glyphs.empty(),
        "fixtures read");

  // An outline whose /Next chain loops (A -> B -> A) and whose item A
  // names itself as its own /First: the walk visits each item once, so the
  // outline comes back with its two roots and nothing under A, instead of
  // growing until the process runs out of memory. Run before the server
  // starts, in a child the alarm cuts short.
  {
    const int roots = OutlineRootsInChild(OutlineCyclePdf(), 20);
    Check(roots == 2, "an outline with a sibling loop lists each item once");
  }

  // ValidUtf8: well-formed UTF-8 passes through; each byte of an
  // ill-formed sequence (a GBK name, a truncated sequence, an overlong
  // form, a surrogate) is read as Latin-1.
  {
    using grpc_pdfium::ValidUtf8;
    Check(ValidUtf8("Helvetica") == "Helvetica", "ASCII passes through");
    Check(ValidUtf8("\xE5\xAE\x8B\xE4\xBD\x93") == "\xE5\xAE\x8B\xE4\xBD\x93",
          "well-formed UTF-8 passes through");
    Check(ValidUtf8("\xCB\xCE\xCC\xE5") ==
              "\xC3\x8B\xC3\x8E\xC3\x8C\xC3\xA5",
          "GBK bytes read as Latin-1");
    Check(ValidUtf8("A\xE5\xAE") == "A\xC3\xA5\xC2\xAE",
          "a truncated sequence is read as Latin-1");
    Check(ValidUtf8("\xC0\xAF") == "\xC3\x80\xC2\xAF",
          "an overlong form is read as Latin-1");
    Check(ValidUtf8("\xED\xA0\x80") == "\xC3\xAD\xC2\xA0\xC2\x80",
          "a surrogate is read as Latin-1");
  }

  grpc_pdfium::PdfBackendServiceImpl service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  Check(server != nullptr && port != 0, "server started");
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = pdfv1::PdfBackendService::NewStub(channel);

  // Probe verdicts on rich.pdf: everything supported except what the
  // document lacks (encryption, signatures) and what the engine cannot
  // type (deep resources).
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(rich);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "rich Probe OK");
    const auto& caps = response.capabilities();
    Check(SupportOf(caps, pdfv1::PDF_FAMILY_STRUCT_TREE) ==
              pdfv1::FAMILY_SUPPORT_SUPPORTED,
          "tagged fixture claims the struct tree");
    Check(SupportOf(caps, pdfv1::PDF_FAMILY_OUTLINE) ==
              pdfv1::FAMILY_SUPPORT_SUPPORTED,
          "fixture claims the outline");
    Check(SupportOf(caps, pdfv1::PDF_FAMILY_SIGNATURES) ==
              pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT,
          "unsigned fixture declares signatures absent");
    Check(SupportOf(caps, pdfv1::PDF_FAMILY_ENCRYPTION_INFO) ==
              pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT,
          "unencrypted fixture declares encryption absent");
    Check(SupportOf(caps, pdfv1::PDF_FAMILY_FORM_FIELDS) ==
              pdfv1::FAMILY_SUPPORT_SUPPORTED,
          "fixture with an AcroForm declares form fields supported");
    Check(SupportOf(caps, pdfv1::PDF_FAMILY_DEEP_RESOURCES) ==
              pdfv1::FAMILY_SUPPORT_UNSUPPORTED_BY_BACKEND,
          "deep resources declared unsupported by this engine");
  }

  // Without include_attachment_data the attachment is listed but never
  // decoded: PDFium can only size it by inflating the whole stream, so
  // size_bytes and data are both left unset.
  {
    ParsedStream s = ParseAll(stub.get(), rich, false);
    Check(s.attachments.size() == 1, "attachment listed on a light Parse");
    if (s.attachments.size() == 1) {
      Check(s.attachments[0].name() == "report.csv",
            "light Parse attachment name");
      Check(!s.attachments[0].has_size_bytes() && s.attachments[0].data().empty(),
            "light Parse leaves the attachment undecoded");
    }
  }

  // font-names.pdf: a GBK /BaseFont reaches the font table as valid UTF-8
  // (Latin-1 per byte) and a UTF-8 one unchanged, so the stream parses on
  // the client (a proto3 string with invalid UTF-8 fails the whole parse).
  // Text cells and the font table both name fonts, through both paths.
  // The deadline turns the old failure (the client stalls on the message
  // it cannot parse) into a failed check.
  {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(font_names);
    request.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
    request.add_families(pdfv1::PDF_FAMILY_FONTS);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    std::vector<std::string> names;
    while (reader->Read(&msg)) {
      if (msg.has_fonts()) {
        for (const auto& f : msg.fonts().fonts()) names.push_back(f.base_name());
      }
    }
    Check(reader->Finish().ok(), "font-names Parse OK");
    auto has = [&names](const std::string& name) {
      return std::find(names.begin(), names.end(), name) != names.end();
    };
    Check(has("\xC3\x8B\xC3\x8E\xC3\x8C\xC3\xA5"),
          "GBK base font name arrives as Latin-1 UTF-8");
    Check(has("\xE5\xAE\x8B\xE4\xBD\x93"),
          "UTF-8 base font name arrives unchanged");
  }

  // byte-strings.pdf: the backend copies a URI, a signature's /SubFilter
  // name and its /M date as stored bytes, not decoded text, so each is
  // made valid UTF-8 (Latin-1 per ill-formed byte) like a font name. The
  // deadline turns the old failure (a client stalled on a message it
  // cannot parse) into a failed check.
  {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(byte_strings);
    request.add_families(pdfv1::PDF_FAMILY_HYPERLINKS);
    request.add_families(pdfv1::PDF_FAMILY_OUTLINE);
    request.add_families(pdfv1::PDF_FAMILY_SIGNATURES);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    std::vector<std::string> link_uris;
    std::vector<std::string> outline_uris;
    pdfv1::SignatureChunk signatures;
    while (reader->Read(&msg)) {
      if (msg.has_page()) {
        for (const auto& link : msg.page().hyperlinks()) {
          link_uris.push_back(link.uri());
        }
      }
      if (msg.has_outline()) {
        for (const auto& node : msg.outline().roots()) {
          outline_uris.push_back(node.uri());
        }
      }
      if (msg.has_signatures()) signatures = msg.signatures();
    }
    Check(reader->Finish().ok(), "byte-strings Parse OK");
    Check(link_uris == std::vector<std::string>{"https://example.com/caf\xC3\xA9"},
          "a Latin-1 link URI arrives as UTF-8");
    Check(outline_uris ==
              std::vector<std::string>{"https://example.com/\xC3\x80\xC2\xAF"},
          "an overlong outline URI arrives as Latin-1 UTF-8");
    Check(signatures.signatures_size() == 1, "byte-strings signature arrived");
    if (signatures.signatures_size() == 1) {
      const auto& sig = signatures.signatures(0);
      Check(sig.sub_filter() == "adbe.x509.\xC3\x8B\xC3\x8E",
            "a GBK signature sub filter arrives as Latin-1 UTF-8");
      Check(sig.signing_time_raw() == "D:20260904110000\xC3\xBF",
            "a signing time with a stray byte arrives as Latin-1 UTF-8");
    }
  }

  // Parse rich.pdf with heavy payloads: every claimed family delivers.
  {
    ParsedStream s = ParseAll(stub.get(), rich, true);

    Check(s.has_doc_meta, "doc metadata arrived");
    Check(s.doc_meta.title() == "Rich Fixture", "info title extracted");
    Check(s.doc_meta.author() == "Fixture Author", "info author extracted");
    Check(s.doc_meta.tagged(), "tagged flag set");
    Check(s.doc_meta.pdf_version() == "1.7", "pdf version reported");
    Check(!s.doc_meta.created_raw().empty(), "creation date kept raw");

    Check(s.outline.roots_size() == 2, "outline lists both items");
    if (s.outline.roots_size() == 2) {
      Check(s.outline.roots(0).title() == "Chapter One" &&
                s.outline.roots(0).has_destination(),
            "goto outline item typed with a destination");
      Check(s.outline.roots(1).uri() == "https://example.com/site",
            "URI outline item typed with its target");
    }

    Check(s.attachments.size() == 1, "attachment arrived");
    if (s.attachments.size() == 1) {
      Check(s.attachments[0].name() == "report.csv", "attachment name");
      Check(s.attachments[0].size_bytes() == 18, "attachment size");
      Check(s.attachments[0].data() == "id,total\n1,999.99\n",
            "attachment bytes round-tripped");
    }

    Check(s.javascript.entries_size() == 1, "javascript entry arrived");
    if (s.javascript.entries_size() == 1) {
      Check(s.javascript.entries(0).name() == "init", "javascript name");
      Check(s.javascript.entries(0).script() == "app.beep(0);",
            "javascript source");
    }

    Check(s.struct_tree.roots_size() == 1, "struct tree arrived");
    if (s.struct_tree.roots_size() == 1) {
      const auto& node = s.struct_tree.roots(0);
      Check(node.role() == "P", "struct role");
      Check(node.alt_text() == "a tagged paragraph", "struct alt text");
      Check(node.content_size() == 1 && node.content(0).mcid() == 0,
            "struct node references MCID 0");
    }

    Check(s.embedded_fonts.size() == 1, "embedded font program arrived");
    if (s.embedded_fonts.size() == 1) {
      Check(s.embedded_fonts[0].program().size() == 172232,
            "font program is the complete TTF");
    }
    bool ubuntu_ref = false;
    for (const auto& f : s.fonts) {
      if (f.base_name().find("UbuntuMono") != std::string::npos && f.embedded()) {
        ubuntu_ref = true;
      }
    }
    Check(ubuntu_ref, "font table marks UbuntuMono embedded");
    // One font, one id: the text cells and the page-object walk read the
    // same font handle, so the cell that draws in UbuntuMono points at the
    // entry that carries the program, and no font is listed twice.
    std::map<std::string, int> names;
    for (const auto& f : s.fonts) ++names[f.base_name()];
    bool listed_once = !names.empty();
    for (const auto& [name, n] : names) listed_once = listed_once && n == 1;
    Check(listed_once, "every font is listed once");
    const pdfv1::TextCell* embedded_cell = nullptr;
    for (const auto& cell : s.page.text_cells()) {
      if (cell.text() == "Embedded") embedded_cell = &cell;
    }
    Check(embedded_cell != nullptr && embedded_cell->has_font_id(),
          "the UbuntuMono cell names a font");
    if (embedded_cell != nullptr && embedded_cell->has_font_id()) {
      const pdfv1::FontRef* ref = nullptr;
      for (const auto& f : s.fonts) {
        if (f.font_id() == embedded_cell->font_id()) ref = &f;
      }
      Check(ref != nullptr && ref->base_name() == "UbuntuMono" &&
                ref->embedded() && ref->descriptor_flags() == 33,
            "the cell's font_id resolves to the embedded UbuntuMono entry");
      Check(s.embedded_fonts.size() == 1 &&
                s.embedded_fonts[0].font_id() == embedded_cell->font_id(),
            "the embedded program belongs to the cell's font");
    }
    bool all_fill = s.page.text_cells_size() > 0;
    for (const auto& cell : s.page.text_cells()) {
      all_fill = all_fill &&
                 cell.rendering_mode() == pdfv1::TEXT_RENDERING_MODE_FILL;
    }
    Check(all_fill, "fill-mode text reports TEXT_RENDERING_MODE_FILL");

    Check(s.has_page, "page chunk arrived");
    Check(s.page.images_size() == 1, "placed image arrived");
    if (s.page.images_size() == 1) {
      const auto& img = s.page.images(0);
      Check(img.source_width_px() == 4 && img.source_height_px() == 4,
            "image intrinsic size");
      Check(img.colorspace() == "DeviceRGB", "image colorspace");
      Check(img.has_image() && !img.image().data().empty(),
            "image pixels included on request");
      Check(img.bbox().x1() > img.bbox().x0(), "image bbox spans");
    }
    Check(s.page.hyperlinks_size() == 2, "both links typed");
    bool saw_uri = false;
    bool saw_dest = false;
    for (const auto& link : s.page.hyperlinks()) {
      if (link.uri() == "https://example.com/spec") saw_uri = true;
      if (link.has_destination()) saw_dest = true;
    }
    Check(saw_uri && saw_dest, "URI and goto link targets present");
    Check(s.page.annotations_size() == 6, "all annotations listed");
    bool highlight_ok = false;
    for (const auto& a : s.page.annotations()) {
      if (a.kind() == pdfv1::ANNOTATION_KIND_HIGHLIGHT) {
        highlight_ok = a.author() == "reviewer" && a.quads_size() == 1 &&
                       a.has_color() && a.contents() == "looks right";
      }
    }
    Check(highlight_ok, "highlight has author, quad, color, contents");
    Check(s.page.form_fields_size() == 2, "both form field widgets arrived");
    if (s.page.form_fields_size() == 2) {
      const auto& f = s.page.form_fields(0);
      Check(f.kind() == pdfv1::FORM_FIELD_KIND_TEXT, "field kind");
      Check(f.name() == "customer_name", "field name");
      Check(f.value() == "Jordan Example", "field value");
      Check(f.alternate_name() == "Customer name", "field tooltip");
      Check(f.has_flags() && f.flags() == 0 && !f.read_only(),
            "text field flags are empty");
      Check(!f.has_appearance_state(), "text widget has no /AS");
      // The check box widget inherits /FT and /Ff (ReadOnly) from its
      // parent field; /AS is the widget's own.
      const auto& box = s.page.form_fields(1);
      Check(box.kind() == pdfv1::FORM_FIELD_KIND_CHECK_BOX,
            "check box kind inherited from the parent field");
      Check(box.name() == "agree", "check box takes the parent's name");
      Check(box.has_flags() && box.flags() == 1, "/Ff inherited from the parent");
      Check(box.read_only(), "read-only follows the inherited /Ff");
      Check(box.appearance_state() == "/Yes", "/AS keeps the leading slash");
      Check(box.alternate_name() == "I agree", "check box tooltip inherited");
      Check(box.value() == "Yes", "button value is the bare state name");
      Check(box.rect().x0() == 300 && box.rect().y1() == 265,
            "check box widget rect");
    }
    Check(s.page.shapes_size() == 1, "vector shape arrived");
    if (s.page.shapes_size() == 1) {
      const auto& shape = s.page.shapes(0);
      bool has_cubic = false;
      bool has_close = false;
      for (const auto& seg : shape.segments()) {
        if (seg.has_cubic_to()) has_cubic = true;
        if (seg.has_close()) has_close = true;
      }
      Check(has_cubic && has_close, "path keeps the bezier and the close");
      Check(shape.paint_mode() == pdfv1::PATH_PAINT_MODE_FILL_STROKE,
            "path paint mode is fill+stroke");
      Check(shape.has_fill_color() && shape.fill_color().blue() > 0.9,
            "fill color survives");
    }
    Check(s.page.has_thumbnail(), "thumbnail arrived");
    Check(s.counts[pdfv1::PDF_FAMILY_THUMBNAILS] == 1 &&
              s.counts[pdfv1::PDF_FAMILY_EMBEDDED_FONTS] == 1 &&
              s.counts[pdfv1::PDF_FAMILY_STRUCT_TREE] >= 1,
          "trailer counts the tier 1-2 families");
  }

  // page-tree.pdf: boxes and rotation inherited through /Pages nodes reach
  // the inventory. crop_box is the box the renderer uses (inherited, clipped
  // to the MediaBox) and stays as stored; every geometry family is reported
  // relative to it (PAGE_SPACE_CROP_BOX), before /Rotate, on the offset and
  // the rotated pages alike.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(page_tree);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    pdfv1::ParseHeader header;
    pdfv1::OutlineChunk outline;
    std::map<uint32_t, pdfv1::PageChunk> pages;
    while (reader->Read(&msg)) {
      if (msg.has_header()) header = msg.header();
      if (msg.has_outline()) outline = msg.outline();
      if (msg.has_page()) pages[msg.page().page_index()] = msg.page();
    }
    Check(reader->Finish().ok(), "page-tree Parse OK");
    auto box_is = [](const pdfv1::BoundingBox& box, double x0, double y0,
                     double x1, double y1) {
      return box.x0() == x0 && box.y0() == y0 && box.x1() == x1 &&
             box.y1() == y1;
    };
    auto near = [](double v, double want) { return v > want - 1 && v < want + 1; };
    Check(header.pages_size() == 4, "page-tree inventory lists four pages");
    if (header.pages_size() == 4) {
      const auto& p0 = header.pages(0);
      Check(p0.has_media_box() && box_is(p0.media_box(), 0, 0, 600, 800),
            "MediaBox inherited from the root /Pages node");
      Check(p0.has_crop_box() && box_is(p0.crop_box(), 0, 0, 600, 800),
            "no CropBox anywhere: crop_box equals the inherited MediaBox");
      const auto& p1 = header.pages(1);
      Check(box_is(p1.media_box(), 0, 0, 600, 800) &&
                box_is(p1.crop_box(), 50, 100, 550, 700),
            "own CropBox over an inherited MediaBox");
      Check(p1.width_pts() == 500 && p1.height_pts() == 600,
            "page size is the CropBox");
      const auto& p2 = header.pages(2);
      Check(p2.rotation_degrees() == 90, "/Rotate inherited from a /Pages node");
      Check(box_is(p2.media_box(), 0, 0, 600, 800) &&
                box_is(p2.crop_box(), 10, 20, 590, 780),
            "CropBox inherited from an intermediate /Pages node");
      Check(p2.width_pts() == 760 && p2.height_pts() == 580,
            "rotated page size swaps the CropBox sides");
      const auto& p3 = header.pages(3);
      Check(p3.rotation_degrees() == 0, "own /Rotate overrides the inherited one");
      Check(box_is(p3.media_box(), 0, 0, 300, 400) &&
                box_is(p3.crop_box(), 10, 20, 300, 400),
            "inherited CropBox clipped to the page's own MediaBox");
      bool all_crop_space = true;
      for (const auto& page : header.pages()) {
        if (page.page_space() != pdfv1::PAGE_SPACE_CROP_BOX) {
          all_crop_space = false;
        }
      }
      Check(all_crop_space, "every page says PAGE_SPACE_CROP_BOX");
    }
    Check(pages.size() == 4, "every page-tree page has a chunk");
    auto first_cell = [&pages](uint32_t index) {
      const auto& chunk = pages[index];
      return chunk.text_cells_size() > 0 ? chunk.text_cells(0)
                                         : pdfv1::TextCell();
    };
    // Text drawn at user x = 100, 100, 100, 50.
    Check(first_cell(0).text() == "inherited" &&
              near(first_cell(0).bbox().x0(), 100),
          "zero-origin CropBox: the cell is where user space put it");
    const pdfv1::TextCell cropped = first_cell(1);
    Check(cropped.text() == "cropped" && near(cropped.bbox().x0(), 50) &&
              near(cropped.quad().x0(), 50) && cropped.bbox().y0() < 500 &&
              cropped.bbox().y1() > 500,
          "offset CropBox: the cell is shifted by the CropBox origin");
    const pdfv1::TextCell rotated = first_cell(2);
    Check(rotated.text() == "rotated" && near(rotated.bbox().x0(), 90) &&
              rotated.bbox().y0() < 480 && rotated.bbox().y1() > 480,
          "rotated page: the cell is shifted, still before /Rotate");
    Check(first_cell(3).text() == "own" && near(first_cell(3).bbox().x0(), 40),
          "clipped inherited CropBox: the cell is shifted by its origin");
    const auto& p1 = pages[1];
    Check(p1.shapes_size() == 1 &&
              box_is(p1.shapes(0).bbox(), 100, 100, 140, 130) &&
              p1.shapes(0).segments_size() > 0 &&
              p1.shapes(0).segments(0).has_move_to() &&
              near(p1.shapes(0).segments(0).move_to().x(), 100) &&
              near(p1.shapes(0).segments(0).move_to().y(), 100),
          "offset CropBox: the shape and its path points are shifted");
    Check(p1.hyperlinks_size() == 1 &&
              box_is(p1.hyperlinks(0).bbox(), 150, 200, 210, 220),
          "offset CropBox: the link region is shifted");
    if (p1.hyperlinks_size() == 1) {
      const auto& dest = p1.hyperlinks(0).destination();
      Check(dest.page_index() == 2 && dest.has_x() && dest.has_y() &&
                near(dest.x(), 100) && near(dest.y(), 490),
            "link destination is shifted by its target page's CropBox");
    }
    Check(p1.annotations_size() == 1 &&
              box_is(p1.annotations(0).rect(), 150, 200, 210, 220),
          "offset CropBox: the annotation rect is shifted");
    const auto& p2 = pages[2];
    Check(p2.images_size() == 1 &&
              box_is(p2.images(0).bbox(), 190, 280, 230, 310) &&
              near(p2.images(0).quad().x0(), 190) &&
              near(p2.images(0).quad().y0(), 280),
          "rotated page: the image placement is shifted, before /Rotate");
    Check(outline.roots_size() == 1 &&
              outline.roots(0).destination().page_index() == 2 &&
              near(outline.roots(0).destination().x(), 100) &&
              near(outline.roots(0).destination().y(), 490),
          "outline destination is shifted by its target page's CropBox");
  }

  // ShiftToCropSpace moves every geometry field of every page family,
  // including form widgets and annotation quads the fixtures above do not
  // place on an offset page, and leaves unset destination coordinates unset.
  {
    google::protobuf::RepeatedPtrField<pdfv1::PageInfo> inventory;
    for (uint32_t i = 0; i < 2; ++i) {
      auto* info = inventory.Add();
      info->set_page_index(i);
      auto* crop = info->mutable_crop_box();
      crop->set_x0(10.0 * (i + 1));
      crop->set_y0(20.0 * (i + 1));
      crop->set_x1(500);
      crop->set_y1(700);
    }
    const grpc_pdfium::CropOrigins origins =
        grpc_pdfium::CropOriginsOf(inventory);
    auto set_box = [](pdfv1::BoundingBox* box) {
      box->set_x0(100);
      box->set_y0(200);
      box->set_x1(110);
      box->set_y1(220);
    };
    auto set_quad = [](pdfv1::Quad* q) {
      q->set_x0(100); q->set_y0(200); q->set_x1(110); q->set_y1(200);
      q->set_x2(110); q->set_y2(220); q->set_x3(100); q->set_y3(220);
    };
    pdfv1::PageChunk chunk;
    chunk.set_page_index(0);
    auto* cell = chunk.add_text_cells();
    set_box(cell->mutable_bbox());
    set_quad(cell->mutable_quad());
    auto* image = chunk.add_images();
    set_box(image->mutable_bbox());
    set_quad(image->mutable_quad());
    auto* link = chunk.add_hyperlinks();
    set_box(link->mutable_bbox());
    link->mutable_destination()->set_page_index(1);
    link->mutable_destination()->set_y(300);
    auto* annot = chunk.add_annotations();
    set_box(annot->mutable_rect());
    set_quad(annot->add_quads());
    auto* field = chunk.add_form_fields();
    set_box(field->mutable_rect());
    auto* shape = chunk.add_shapes();
    set_box(shape->mutable_bbox());
    auto* move = shape->add_segments()->mutable_move_to();
    move->set_x(100);
    move->set_y(200);
    auto* cubic = shape->add_segments()->mutable_cubic_to();
    cubic->mutable_control1()->set_x(100);
    cubic->mutable_control1()->set_y(200);
    cubic->mutable_control2()->set_x(100);
    cubic->mutable_control2()->set_y(200);
    cubic->mutable_end()->set_x(100);
    cubic->mutable_end()->set_y(200);
    shape->add_segments()->set_close(true);
    grpc_pdfium::ShiftToCropSpace(origins, &chunk);
    auto box_ok = [](const pdfv1::BoundingBox& b) {
      return b.x0() == 90 && b.y0() == 180 && b.x1() == 100 && b.y1() == 200;
    };
    auto quad_ok = [](const pdfv1::Quad& q) {
      return q.x0() == 90 && q.y0() == 180 && q.x1() == 100 && q.y1() == 180 &&
             q.x2() == 100 && q.y2() == 200 && q.x3() == 90 && q.y3() == 200;
    };
    auto point_ok = [](const pdfv1::PathPoint& p) {
      return p.x() == 90 && p.y() == 180;
    };
    Check(box_ok(cell->bbox()) && quad_ok(cell->quad()), "shift: text cell");
    Check(box_ok(image->bbox()) && quad_ok(image->quad()), "shift: image");
    Check(box_ok(link->bbox()), "shift: link region");
    Check(!link->destination().has_x() && link->destination().y() == 260,
          "shift: destination uses the target page, unset x stays unset");
    Check(box_ok(annot->rect()) && quad_ok(annot->quads(0)),
          "shift: annotation rect and quads");
    Check(box_ok(field->rect()), "shift: form widget");
    Check(box_ok(shape->bbox()) && point_ok(*move) &&
              point_ok(cubic->control1()) && point_ok(cubic->control2()) &&
              point_ok(cubic->end()) && shape->segments(2).has_close(),
          "shift: shape bbox and every path point");
    pdfv1::OutlineChunk outline;
    auto* root = outline.add_roots();
    root->mutable_destination()->set_page_index(0);
    root->mutable_destination()->set_x(50);
    auto* child = root->add_children();
    child->mutable_destination()->set_page_index(1);
    child->mutable_destination()->set_x(50);
    child->mutable_destination()->set_y(50);
    grpc_pdfium::ShiftToCropSpace(origins, &outline);
    Check(root->destination().x() == 40 && !root->destination().has_y() &&
              child->destination().x() == 30 && child->destination().y() == 10,
          "shift: nested outline destinations use their target pages");
  }

  // Parse loads only the pages it needs. The header still lists every page,
  // but the inventory costs a load per page once per document hash, and a
  // page range costs one load per page in it. gRParse parses a long
  // document one page per call, so this is N page loads instead of N^2.
  {
    struct Ranged {
      int inventory = 0;
      std::vector<uint32_t> pages;
      uint64_t loads = 0;
    };
    auto parse_page = [&stub](const std::string& data, const std::string* sha,
                              uint32_t page) {
      Ranged out;
      const uint64_t before = grpc_pdfium::PdfiumEngine::PageLoads();
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(data);
      if (sha != nullptr) request.mutable_document()->set_sha256(*sha);
      request.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
      request.mutable_pages()->set_begin(page);
      request.mutable_pages()->set_end(page + 1);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse msg;
      while (reader->Read(&msg)) {
        if (msg.has_header()) out.inventory = msg.header().pages_size();
        if (msg.has_page()) out.pages.push_back(msg.page().page_index());
      }
      Check(reader->Finish().ok(), "ranged Parse OK");
      out.loads = grpc_pdfium::PdfiumEngine::PageLoads() - before;
      return out;
    };
    const std::string sha = grpc_pdfium::Sha256Hex(page_tree);
    const Ranged first = parse_page(page_tree, &sha, 2);
    Check(first.inventory == 4 && first.pages == std::vector<uint32_t>{2},
          "ranged Parse lists every page and emits only the range");
    Check(first.loads == 5, "first ranged Parse loads each page once, plus the range");
    const Ranged second = parse_page(page_tree, &sha, 3);
    Check(second.inventory == 4 && second.pages == std::vector<uint32_t>{3},
          "cached inventory still lists every page");
    Check(second.loads == 1, "later ranged Parse of the same hash loads one page");
    const Ranged unhashed = parse_page(page_tree, nullptr, 0);
    Check(unhashed.inventory == 4 && unhashed.loads == 5,
          "without a hash the inventory is rebuilt");
    // A hash that does not fit the bytes (another size, another page count)
    // is never served the cached inventory.
    const std::string hello = ReadFile(dir + "/hello.pdf");
    const Ranged mismatched = parse_page(hello, &sha, 0);
    Check(mismatched.inventory == 1 && mismatched.loads == 2,
          "cached inventory is keyed to the bytes it was built from");

    // The progress hook runs once per page the inventory loads (what keeps
    // the pool's watchdog off a long first Parse), and never when the
    // inventory comes from the cache. The hash's entry now holds hello.pdf
    // (the mismatch above replaced it), so the first call rebuilds.
    int signals = 0;
    grpc_pdfium::PdfiumEngine::SetProgressHook([&signals] { ++signals; });
    parse_page(page_tree, &sha, 0);
    Check(signals == 4, "the inventory signals progress once per page");
    signals = 0;
    parse_page(page_tree, &sha, 0);
    Check(signals == 0, "a cached inventory signals nothing");
    grpc_pdfium::PdfiumEngine::SetProgressHook(nullptr);
  }

  // A link or bookmark whose destination names no page of the document is
  // never reported as a jump to page 0. The bookmark keeps its title with
  // no destination, the link is not emitted (the contract: a link the
  // engine cannot type is not emitted), and the trailer says how many of
  // each were found.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(DanglingDestinationsPdf());
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    pdfv1::OutlineChunk outline;
    std::vector<pdfv1::PageChunk> pages;
    pdfv1::ParseTrailer trailer;
    while (reader->Read(&msg)) {
      if (msg.has_outline()) outline = msg.outline();
      if (msg.has_page()) pages.push_back(msg.page());
      if (msg.has_trailer()) trailer = msg.trailer();
    }
    Check(reader->Finish().ok(), "dangling destinations: Parse finishes OK");
    Check(outline.roots_size() == 2 && outline.roots(0).title() == "Gone" &&
              outline.roots(1).title() == "Kept",
          "dangling destinations: both bookmarks are listed");
    Check(outline.roots_size() == 2 && !outline.roots(0).has_destination() &&
              !outline.roots(0).has_uri(),
          "dangling destinations: a bookmark to a missing page has no destination");
    Check(outline.roots_size() == 2 && outline.roots(1).has_destination() &&
              outline.roots(1).destination().page_index() == 1 &&
              outline.roots(1).destination().x() == 30,
          "dangling destinations: a bookmark to a real page keeps its destination");
    Check(pages.size() == 2 && pages[0].hyperlinks_size() == 1 &&
              pages[0].hyperlinks(0).has_destination() &&
              pages[0].hyperlinks(0).destination().page_index() == 1 &&
              pages[0].hyperlinks(0).bbox().y0() == 100,
          "dangling destinations: only the link to a real page is emitted");
    int outline_warnings = 0;
    int link_warnings = 0;
    for (const auto& w : trailer.warnings()) {
      if (!w.has_family() || w.message().empty()) continue;
      if (w.family() == pdfv1::PDF_FAMILY_OUTLINE && !w.has_page_index() &&
          w.message().rfind("1 ", 0) == 0) {
        ++outline_warnings;
      }
      if (w.family() == pdfv1::PDF_FAMILY_HYPERLINKS && w.has_page_index() &&
          w.page_index() == 0 && w.message().rfind("3 ", 0) == 0) {
        ++link_warnings;
      }
    }
    Check(outline_warnings == 1,
          "dangling destinations: one outline warning counts the bookmark");
    Check(link_warnings == 1,
          "dangling destinations: one page warning counts the three links");
  }

  // form-xobject.pdf: text drawn with 3 Tr is the invisible OCR underlay,
  // and the cells say so; text inside a Form XObject is a cell too.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(form_xobject);
    request.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    std::map<std::string, pdfv1::TextRenderingMode> modes;
    while (reader->Read(&msg)) {
      if (!msg.has_page()) continue;
      for (const auto& cell : msg.page().text_cells()) {
        modes[cell.text()] = cell.rendering_mode();
      }
    }
    Check(reader->Finish().ok(), "form-xobject text Parse OK");
    Check(modes["Visible"] == pdfv1::TEXT_RENDERING_MODE_FILL,
          "filled text reports FILL");
    Check(modes["Invisible"] == pdfv1::TEXT_RENDERING_MODE_INVISIBLE,
          "3 Tr text reports INVISIBLE");
    Check(modes["Inside"] == pdfv1::TEXT_RENDERING_MODE_FILL,
          "text inside a Form XObject reports its own mode");
  }

  // form-xobject.pdf: images, paths and fonts inside Form XObjects (one
  // nested in another) belong to the page, in page space. Text cells are
  // not requested, so Courier, drawn only inside the form, can reach the
  // font table through the form walk alone.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(form_xobject);
    request.add_families(pdfv1::PDF_FAMILY_PLACED_IMAGES);
    request.add_families(pdfv1::PDF_FAMILY_VECTOR_SHAPES);
    request.add_families(pdfv1::PDF_FAMILY_FONTS);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    pdfv1::PageChunk page;
    std::vector<std::string> font_names;
    while (reader->Read(&msg)) {
      if (msg.has_page()) page = msg.page();
      if (msg.has_fonts()) {
        for (const auto& f : msg.fonts().fonts()) font_names.push_back(f.base_name());
      }
    }
    Check(reader->Finish().ok(), "form-xobject Parse OK");
    auto near = [](double a, double b) { return a > b - 0.01 && a < b + 0.01; };
    auto box_near = [&near](const pdfv1::BoundingBox& box, double x0, double y0,
                            double x1, double y1) {
      return near(box.x0(), x0) && near(box.y0(), y0) && near(box.x1(), x1) &&
             near(box.y1(), y1);
    };
    Check(page.images_size() == 2, "both images inside the forms are placed");
    if (page.images_size() == 2) {
      Check(box_near(page.images(0).bbox(), 210, 310, 260, 350),
            "image in a form maps through the form matrix and the CTM");
      Check(box_near(page.images(1).bbox(), 310, 310, 330, 330),
            "image in a nested form maps through both forms");
      Check(near(page.images(1).quad().x0(), 310) &&
                near(page.images(1).quad().y2(), 330),
            "nested image quad is in page space");
    }
    Check(page.shapes_size() == 1, "the path inside the form is a shape");
    if (page.shapes_size() == 1) {
      const auto& shape = page.shapes(0);
      Check(box_near(shape.bbox(), 220, 370, 300, 400),
            "shape bounds are in page space");
      Check(shape.segments_size() > 0 && shape.segments(0).has_move_to() &&
                near(shape.segments(0).move_to().x(), 220) &&
                near(shape.segments(0).move_to().y(), 370),
            "shape points are in page space");
    }
    Check(std::find(font_names.begin(), font_names.end(), "Courier") !=
              font_names.end(),
          "a font used only inside a form reaches the table");
  }

  // huge-image.pdf: a 9000 x 9000 image (81 megapixels, above the
  // 64-megapixel decode limit) whose zero rows sit under two Flate filters
  // in about a kilobyte. PDFium decodes it when asked, so without the limit
  // its pixels would be in the chunk. With image data requested the
  // placement is reported, the pixels are left out before anything is
  // decoded, and the trailer says why.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(huge_image);
    request.mutable_options()->set_include_image_data(true);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    pdfv1::PageChunk page;
    pdfv1::ParseTrailer trailer;
    while (reader->Read(&msg)) {
      if (msg.has_page()) page = msg.page();
      if (msg.has_trailer()) trailer = msg.trailer();
    }
    Check(reader->Finish().ok(), "huge-image Parse OK");
    Check(page.images_size() == 1 && page.images(0).source_width_px() == 9000 &&
              page.images(0).source_height_px() == 9000,
          "the oversized image's placement is reported");
    Check(page.images_size() == 1 && !page.images(0).has_image(),
          "the oversized image's pixels are left out");
    Check(trailer.warnings_size() == 1 && trailer.warnings(0).has_page_index() &&
              trailer.warnings(0).page_index() == 0 &&
              trailer.warnings(0).family() == pdfv1::PDF_FAMILY_PLACED_IMAGES,
          "the trailer warns about the left-out image data");
  }

  // control-glyphs.pdf: GetText leaves out some of the control-code glyphs
  // drawn above the caption, so its string is shorter than the character
  // count. Each cell is read per character index: "Round-wise" keeps its
  // own box at y 500, not one up among the glyphs at y 600.
  {
    ParsedStream s = ParseAll(stub.get(), control_glyphs, false);
    const pdfv1::TextCell* word = nullptr;
    for (const auto& cell : s.page.text_cells()) {
      if (cell.text() == "Round-wise") word = &cell;
    }
    Check(word != nullptr, "control-glyphs: the caption word is a cell");
    if (word != nullptr) {
      const double low = std::min(word->bbox().y0(), word->bbox().y1());
      const double high = std::max(word->bbox().y0(), word->bbox().y1());
      Check(low > 490 && high < 520, "control-glyphs: the caption word keeps its own box");
      Check(word->font_size() > 9.5 && word->font_size() < 10.5,
            "control-glyphs: the caption word keeps its own font size");
    }
    bool hyphenated = false;
    for (const auto& cell : s.page.text_cells()) {
      if (cell.text() == "selec\xEF\xBF\xBEtion") hyphenated = true;
    }
    Check(hyphenated, "control-glyphs: a line-break hyphen stays U+FFFE inside one cell");
  }

  // Render draws form widgets unless the request omits annotations. In
  // rich.pdf the check box at [300 250 315 265] shows its /Yes appearance
  // (a filled 9 pt square) and the text field at [300 300 450 320] holds
  // "Jordan Example" with no appearance stream, which the engine writes
  // from the value. FPDF_RenderPageBitmap alone draws neither, so a filled
  // form came back with empty fields.
  {
    struct Raster {
      grpc::Status status;
      pdfv1::PageRaster raster;
    };
    auto render = [&stub, &rich](pdfv1::PixelFormat format, bool omit) {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.mutable_document()->set_data(rich);
      request.set_dpi(72.0);
      request.set_pixel_format(format);
      request.set_omit_annotations(omit);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      Raster out;
      while (reader->Read(&message)) {
        if (message.has_raster()) out.raster = message.raster();
      }
      out.status = reader->Finish();
      return out;
    };
    // Dark pixels inside a rectangle given in page points (origin bottom
    // left), on a 72 dpi raster of the 612 x 792 page.
    auto ink = [](const pdfv1::PageRaster& raster, int x0, int y0, int x1,
                  int y1) {
      const int channels = raster.pixel_format() == pdfv1::PIXEL_FORMAT_GRAY8 ? 1 : 3;
      const int height = static_cast<int>(raster.height_px());
      int dark = 0;
      for (int y = height - y1; y < height - y0; ++y) {
        for (int x = x0; x < x1; ++x) {
          const size_t at = static_cast<size_t>(y) * raster.stride_bytes() +
                            static_cast<size_t>(x) * channels;
          if (at + channels > raster.pixels().size()) return -1;
          for (int c = 0; c < channels; ++c) {
            if (static_cast<unsigned char>(raster.pixels()[at + c]) < 128) {
              ++dark;
              break;
            }
          }
        }
      }
      return dark;
    };
    for (const pdfv1::PixelFormat format :
         {pdfv1::PIXEL_FORMAT_BGR8, pdfv1::PIXEL_FORMAT_GRAY8}) {
      const Raster drawn = render(format, false);
      Check(drawn.status.ok() && drawn.raster.width_px() == 612 &&
                drawn.raster.height_px() == 792,
            "form render: the page rasterizes at 72 dpi");
      Check(ink(drawn.raster, 300, 250, 315, 265) >= 60,
            "form render: the checked box is drawn");
      Check(ink(drawn.raster, 300, 300, 450, 320) >= 100,
            "form render: the text field's value is drawn");
      // No field highlight or border is painted over the page: the corner
      // of the text field stays the background colour.
      Check(ink(drawn.raster, 440, 302, 448, 304) == 0,
            "form render: an empty part of the field stays background");
      const Raster omitted = render(format, true);
      Check(omitted.status.ok() &&
                ink(omitted.raster, 300, 250, 315, 265) == 0 &&
                ink(omitted.raster, 300, 300, 450, 320) == 0,
            "form render: omit_annotations leaves the widgets out");
    }
  }

  // signed.pdf: the signature family delivers what is stored.
  {
    ParsedStream s = ParseAll(stub.get(), signed_doc, false);
    Check(s.signatures.signatures_size() == 1, "signature arrived");
    if (s.signatures.signatures_size() == 1) {
      const auto& sig = s.signatures.signatures(0);
      Check(sig.sub_filter() == "adbe.pkcs7.detached", "signature sub filter");
      Check(sig.reason() == "fixture signature", "signature reason");
      Check(sig.contents().size() == 8, "signature blob bytes");
      Check(sig.byte_ranges_size() == 2 && sig.byte_ranges(1).offset() == 856,
            "signature byte ranges");
    }
  }

  // Encryption: the open variant loads and reports its scheme, the locked
  // variant walks the typed password statuses.
  {
    ParsedStream s = ParseAll(stub.get(), enc_open, false);
    Check(s.header.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
          "empty-user-password document opens");
    Check(s.has_encryption, "encryption info arrived");
    if (s.has_encryption) {
      Check(s.encryption.revision() == 4, "security handler revision");
      Check(!s.encryption.can_copy(), "copy permission bit is off");
      Check(s.encryption.can_print(), "print permission bit is on");
    }
  }
  {
    auto probe = [&stub](const std::string& data, const char* password) {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_data(data);
      if (password != nullptr) request.mutable_document()->set_password(password);
      pdfv1::ProbeResponse response;
      Check(stub->Probe(&ctx, request, &response).ok(), "Probe RPC OK");
      return response.capabilities().load_status();
    };
    Check(probe(enc_locked, nullptr) == pdfv1::LOAD_STATUS_PASSWORD_REQUIRED,
          "locked document without password: PASSWORD_REQUIRED");
    Check(probe(enc_locked, "wrong") == pdfv1::LOAD_STATUS_PASSWORD_INCORRECT,
          "locked document with wrong password: PASSWORD_INCORRECT");
    Check(probe(enc_locked, "secret") == pdfv1::LOAD_STATUS_OK,
          "locked document with the password: OK");
  }

  // Render types the same password verdicts in its head (one message, then
  // the stream ends OK), so a client can tell "needs a password" from a
  // server fault; with the password it rasterizes.
  {
    auto render = [&stub](const std::string& data, const char* password,
                          pdfv1::RenderResponse* first, int* messages) {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.mutable_document()->set_data(data);
      if (password != nullptr) request.mutable_document()->set_password(password);
      request.set_dpi(36.0);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      *messages = 0;
      while (reader->Read(&message)) {
        if (++*messages == 1) *first = message;
      }
      return reader->Finish();
    };
    pdfv1::RenderResponse first;
    int messages = 0;
    Check(render(enc_locked, nullptr, &first, &messages).ok() && messages == 1 &&
              first.has_head() &&
              first.head().load_status() == pdfv1::LOAD_STATUS_PASSWORD_REQUIRED,
          "locked Render without password: PASSWORD_REQUIRED in the head");
    Check(render(enc_locked, "wrong", &first, &messages).ok() && messages == 1 &&
              first.head().load_status() ==
                  pdfv1::LOAD_STATUS_PASSWORD_INCORRECT,
          "locked Render with a wrong password: PASSWORD_INCORRECT in the head");
    Check(render(enc_locked, "secret", &first, &messages).ok() && messages == 1 &&
              first.has_raster() && !first.has_head(),
          "locked Render with the password rasterizes");
  }

  // Pages the document counts but PDFium cannot load are never left out
  // without a word. Parse lists the pages that loaded, counts those, and
  // names each missing one in a page-inventory warning (again on a second
  // call, when the inventory comes from the worker's cache). Render of a
  // range that holds such a page fails INTERNAL after the rasters before
  // it, and over ParseStream the request ends ENGINE_ERROR.
  {
    const std::string missing = MissingPagesPdf();
    const std::string missing_sha = grpc_pdfium::Sha256Hex(missing);
    for (int round = 0; round < 2; ++round) {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(missing);
      request.mutable_document()->set_sha256(missing_sha);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse msg;
      pdfv1::ParseHeader header;
      pdfv1::ParseTrailer trailer;
      std::vector<uint32_t> chunks;
      while (reader->Read(&msg)) {
        if (msg.has_header()) header = msg.header();
        if (msg.has_page()) chunks.push_back(msg.page().page_index());
        if (msg.has_trailer()) trailer = msg.trailer();
      }
      Check(reader->Finish().ok(), "missing pages: Parse finishes OK");
      Check(header.capabilities().load_status() == pdfv1::LOAD_STATUS_OK &&
                header.capabilities().page_count() == 3,
            "missing pages: the document loads and counts three pages");
      Check(header.pages_size() == 1 && header.pages(0).page_index() == 0,
            "missing pages: the inventory lists only the page that loads");
      Check(chunks == std::vector<uint32_t>{0},
            "missing pages: one page chunk, for page 0");
      uint64_t inventory_count = 99;
      for (const auto& c : trailer.counts()) {
        if (c.family() == pdfv1::PDF_FAMILY_PAGE_INVENTORY) {
          inventory_count = c.count();
        }
      }
      Check(inventory_count == 1,
            "missing pages: the trailer counts the pages listed, not /Count");
      std::vector<uint32_t> warned;
      for (const auto& w : trailer.warnings()) {
        if (w.has_family() && w.family() == pdfv1::PDF_FAMILY_PAGE_INVENTORY &&
            w.has_page_index() && !w.message().empty()) {
          warned.push_back(w.page_index());
        }
      }
      Check(warned == std::vector<uint32_t>({1, 2}),
            round == 0
                ? "missing pages: one inventory warning per page that did not load"
                : "missing pages: the warnings come back with a cached inventory");
    }

    auto render = [&stub, &missing](uint32_t begin, uint32_t end, int* rasters) {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.mutable_document()->set_data(missing);
      request.set_dpi(36.0);
      request.mutable_pages()->set_begin(begin);
      request.mutable_pages()->set_end(end);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      *rasters = 0;
      while (reader->Read(&message)) {
        if (message.has_raster()) ++*rasters;
      }
      return reader->Finish();
    };
    int rasters = 0;
    Check(render(0, 1, &rasters).ok() && rasters == 1,
          "missing pages: Render of the page that loads is OK");
    grpc::Status status = render(0, 3, &rasters);
    Check(status.error_code() == grpc::StatusCode::INTERNAL && rasters == 1,
          "missing pages: Render fails INTERNAL at the first page it cannot load");
    Check(status.error_message().find("page 1") != std::string::npos,
          "missing pages: the Render status names the page");
    status = render(2, 3, &rasters);
    Check(status.error_code() == grpc::StatusCode::INTERNAL && rasters == 0,
          "missing pages: Render of only a missing page is INTERNAL, not an empty OK");

    namespace psc = parse_stream_checks;
    pdfv1::ParseStreamOpen open;
    open.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
    open.set_sha256(missing_sha);
    open.set_declared_size(missing.size());
    pdfv1::ParseStreamRequest render_all = psc::RenderMessage(7, 36.0);
    render_all.mutable_render()->mutable_pages()->set_end(3);
    const psc::StreamRun run =
        psc::Run(stub.get(), psc::Upload(open, missing, missing.size(),
                                         {{0, render_all}}));
    Check(run.status.ok(), "missing pages: the ParseStream itself finishes OK");
    Check(run.DoneOf(7) != nullptr &&
              run.DoneOf(7)->outcome() == pdfv1::REQUEST_OUTCOME_ENGINE_ERROR,
          "missing pages: a ParseStream render over a missing page ends ENGINE_ERROR");
    Check(run.RenderOf(7).size() == 1,
          "missing pages: the raster before the missing page was sent");
  }

  // A placed image and a thumbnail PDFium cannot decode: each is said, and
  // neither leaves an empty EncodedImage behind.
  {
    const ParsedStream s = ParseAll(stub.get(), UndecodableImagesPdf(), true);
    Check(s.has_page && s.page.images_size() == 1,
          "undecodable image: the placed image is still listed");
    Check(s.page.images_size() == 1 && !s.page.images(0).has_image(),
          "undecodable image: no empty EncodedImage is sent for it");
    Check(s.Warned(pdfv1::PDF_FAMILY_PLACED_IMAGES, 0, "could not decode") == 1,
          "undecodable image: one warning names the page and the family");
    Check(!s.page.has_thumbnail(),
          "undecodable thumbnail: no empty EncodedImage is sent for it");
    Check(s.Warned(pdfv1::PDF_FAMILY_THUMBNAILS, 0, "could not decode") == 1,
          "undecodable thumbnail: one warning names the page and the family");
    auto count = s.counts.find(pdfv1::PDF_FAMILY_THUMBNAILS);
    Check(count == s.counts.end() || count->second == 0,
          "undecodable thumbnail: it is not counted as sent");
    const ParsedStream rich_heavy = ParseAll(stub.get(), rich, true);
    Check(rich_heavy.Warned(pdfv1::PDF_FAMILY_THUMBNAILS, 0, "") == 0 &&
              rich_heavy.Warned(pdfv1::PDF_FAMILY_PLACED_IMAGES, 0, "") == 0,
          "a page with no thumbnail and readable images gets neither warning");
  }

  // Attachments whose bytes cannot be had: said when the bytes were asked
  // for, and an entry PDFium cannot open is said either way.
  {
    const std::string pdf = AttachmentsWithoutDataPdf();
    const ParsedStream heavy = ParseAll(stub.get(), pdf, true);
    Check(heavy.attachments.size() == 2,
          "attachments without data: the two PDFium can open are listed");
    Check(heavy.Warned(pdfv1::PDF_FAMILY_ATTACHMENTS, -1, "external.txt") == 1,
          "attachments without data: a file with no embedded stream is said");
    Check(heavy.Warned(pdfv1::PDF_FAMILY_ATTACHMENTS, -1, "3 of 3") == 1,
          "attachments without data: an entry PDFium cannot open is said");
    Check(heavy.warnings.size() == 2,
          "attachments without data: an empty file is not a warning");
    for (const auto& a : heavy.attachments) {
      if (a.name() == "empty.txt") {
        Check(a.has_size_bytes() && a.size_bytes() == 0 && a.data().empty(),
              "attachments without data: an empty file reports size 0");
      } else {
        Check(!a.has_size_bytes() && a.data().empty(),
              "attachments without data: a file with no stream has no size");
      }
    }
    const ParsedStream light = ParseAll(stub.get(), pdf, false);
    Check(light.attachments.size() == 2 && light.warnings.size() == 1 &&
              light.Warned(pdfv1::PDF_FAMILY_ATTACHMENTS, -1, "3 of 3") == 1,
          "attachments without data: a light Parse only says the entry it "
          "could not open");
  }

  // The outline walk stops at a fixed depth; what it left out is said.
  {
    auto deepest = [](const pdfv1::OutlineChunk& outline) {
      int levels = 0;
      const pdfv1::OutlineNode* node =
          outline.roots_size() > 0 ? &outline.roots(0) : nullptr;
      while (node != nullptr) {
        ++levels;
        node = node->children_size() > 0 ? &node->children(0) : nullptr;
      }
      return levels;
    };
    const ParsedStream deep = ParseAll(stub.get(), DeepOutlinePdf(70), false);
    Check(deepest(deep.outline) == 65, "deep outline: 65 levels are sent");
    Check(deep.Warned(pdfv1::PDF_FAMILY_OUTLINE, -1, "level 65") == 1,
          "deep outline: the cut is a warning");
    const ParsedStream whole = ParseAll(stub.get(), DeepOutlinePdf(65), false);
    Check(deepest(whole.outline) == 65 && whole.warnings.empty(),
          "an outline that fits the depth limit gets no warning");
  }

  // Annotation subtypes are the document's own names.
  {
    const ParsedStream s = ParseAll(stub.get(), AnnotationSubtypesPdf(), false);
    Check(s.page.annotations_size() == 3, "annotation subtypes: all three listed");
    if (s.page.annotations_size() == 3) {
      Check(s.page.annotations(0).subtype_raw() == "Projection" &&
                s.page.annotations(0).kind() ==
                    pdfv1::ANNOTATION_KIND_UNSPECIFIED,
            "annotation subtypes: an unlisted subtype keeps its real name");
      Check(s.page.annotations(1).subtype_raw() == "Square" &&
                s.page.annotations(1).kind() == pdfv1::ANNOTATION_KIND_SQUARE,
            "annotation subtypes: a listed subtype is unchanged");
      Check(s.page.annotations(2).subtype_raw().empty(),
            "annotation subtypes: no /Subtype is not given a made-up name");
    }
    Check(s.Warned(pdfv1::PDF_FAMILY_ANNOTATIONS, 0, "no /Subtype") == 1,
          "annotation subtypes: a missing /Subtype is a warning");
  }

  // PDFium reads no content of a form XObject nested more than 40 forms
  // deep and says nothing. The image the innermost form draws is then
  // missing, and so is any text: every family the forms feed says so.
  {
    const ParsedStream whole = ParseAll(stub.get(), NestedFormsPdf(40), false);
    Check(whole.page.images_size() == 1 && whole.warnings.empty(),
          "nested forms: 40 levels are read whole, with no warning");
    const ParsedStream cut = ParseAll(stub.get(), NestedFormsPdf(41), false);
    Check(cut.page.images_size() == 0,
          "nested forms: pdfium reads nothing below 40 levels");
    for (pdfv1::PdfFamily family :
         {pdfv1::PDF_FAMILY_TEXT_CELLS, pdfv1::PDF_FAMILY_PLACED_IMAGES,
          pdfv1::PDF_FAMILY_VECTOR_SHAPES, pdfv1::PDF_FAMILY_FONTS}) {
      Check(cut.Warned(family, 0, "more than 40 forms deep") == 1,
            "nested forms: each family the unread form feeds gets a warning");
    }
    Check(cut.warnings.size() == 4,
          "nested forms: no other family is warned about");

    // Only the families that were asked for.
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(NestedFormsPdf(70));
    request.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    pdfv1::ParseTrailer trailer;
    while (reader->Read(&msg)) {
      if (msg.has_trailer()) trailer = msg.trailer();
    }
    Check(reader->Finish().ok(), "nested forms: text-only Parse OK");
    Check(trailer.warnings_size() == 1 &&
              trailer.warnings(0).family() == pdfv1::PDF_FAMILY_TEXT_CELLS &&
              trailer.warnings(0).page_index() == 0,
          "nested forms: a text-only Parse is told its text may be short");
  }

  // PDFium leaves out structure nested deeper than it follows and says
  // nothing; the page's marked content with no element is how it shows.
  {
    auto depth_of = [](const pdfv1::StructTreeChunk& tree) {
      int depth = 0;
      const pdfv1::StructNode* node =
          tree.roots_size() > 0 ? &tree.roots(0) : nullptr;
      while (node != nullptr) {
        ++depth;
        node = node->children_size() > 0 ? &node->children(0) : nullptr;
      }
      return depth;
    };
    const ParsedStream whole = ParseAll(stub.get(), DeepStructTreePdf(33), false);
    Check(depth_of(whole.struct_tree) == 33 && whole.warnings.empty(),
          "deep structure: 33 levels arrive whole, with no warning");
    const ParsedStream cut = ParseAll(stub.get(), DeepStructTreePdf(34), false);
    Check(cut.struct_tree.roots_size() == 0,
          "deep structure: pdfium returns nothing for 34 levels");
    Check(cut.Warned(pdfv1::PDF_FAMILY_STRUCT_TREE, 0, "MCID 0") == 1 &&
              cut.warnings.size() == 1,
          "deep structure: the page's marked content with no element is a "
          "warning");
    const ParsedStream rich_light = ParseAll(stub.get(), rich, false);
    Check(rich_light.Warned(pdfv1::PDF_FAMILY_STRUCT_TREE, 0, "") == 0,
          "a tagged page whose marked content all has elements gets no "
          "warning");
  }

  server->Shutdown();
  if (failures == 0) {
    std::printf("tier12_fixture: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "tier12_fixture: %d check(s) failed\n", failures);
  return 1;
}
