// The M2 gate: every family the backend claims in Probe is exercised by a
// fixture. rich.pdf carries the page-scoped and document-scoped families,
// signed.pdf the signature field, and the encrypted pair the encryption
// info and the password load statuses.

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "pdf_backend_service_impl.h"

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

pdfv1::FamilySupport SupportOf(const pdfv1::BackendCapabilities& caps,
                               pdfv1::PdfFamily family) {
  for (const auto& f : caps.families()) {
    if (f.family() == family) return f.support();
  }
  return pdfv1::FAMILY_SUPPORT_UNSPECIFIED;
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
  Check(!rich.empty() && !signed_doc.empty() && !enc_open.empty() &&
            !enc_locked.empty(),
        "fixtures read");

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

  server->Shutdown();
  if (failures == 0) {
    std::printf("tier12_fixture: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "tier12_fixture: %d check(s) failed\n", failures);
  return 1;
}
