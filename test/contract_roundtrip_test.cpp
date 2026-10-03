// Tier 0 contract test: start the engine-backed service in process, dial
// it through the generated client stubs, and check Probe, Parse, and
// Render against the hello.pdf fixture (one Letter page, Helvetica 24pt
// "Hello PDF" at (100, 700)).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

std::string ReadFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// A one-page PDF with an empty page of the given size in points, with a
// correct cross-reference table.
std::string BlankPdf(int width_pts, int height_pts) {
  const std::string objects[] = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
          std::to_string(width_pts) + " " + std::to_string(height_pts) +
          "] >>"};
  std::string out = "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < std::size(objects); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(offsets.size() + 1) +
         "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    char line[24];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset);
    out += line;
  }
  out += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <hello.pdf>\n", argv[0]);
    return 2;
  }
  const std::string fixture = ReadFile(argv[1]);
  Check(!fixture.empty(), "fixture PDF read");

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

  // Probe the fixture: it loads, has one page, and every family has a
  // verdict.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(fixture);
    pdfv1::ProbeResponse response;
    grpc::Status status = stub->Probe(&ctx, request, &response);
    Check(status.ok(), "Probe RPC succeeded");
    const auto& caps = response.capabilities();
    Check(caps.backend_name() == "grpc-pdfium", "backend name reported");
    Check(caps.load_status() == pdfv1::LOAD_STATUS_OK, "fixture loads OK");
    Check(caps.page_count() == 1, "fixture has one page");
    Check(caps.families_size() == pdfv1::PdfFamily_MAX,
          "every family has a verdict");
    bool text_supported = false;
    for (const auto& f : caps.families()) {
      if (f.family() == pdfv1::PDF_FAMILY_TEXT_CELLS &&
          f.support() == pdfv1::FAMILY_SUPPORT_SUPPORTED) {
        text_supported = true;
      }
    }
    Check(text_supported, "text cells are a supported family");
    bool forms_absent = false;
    for (const auto& f : caps.families()) {
      if (f.family() == pdfv1::PDF_FAMILY_FORM_FIELDS &&
          f.support() == pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT) {
        forms_absent = true;
      }
    }
    Check(forms_absent, "a document without an AcroForm declares no widgets");
  }

  // Probe non-PDF bytes: typed NOT_PDF.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data("not a pdf");
    pdfv1::ProbeResponse response;
    grpc::Status status = stub->Probe(&ctx, request, &response);
    Check(status.ok(), "Probe RPC succeeded for non-PDF bytes");
    Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_NOT_PDF,
          "non-PDF bytes report LOAD_STATUS_NOT_PDF");
  }

  // GetServiceInfo: the document-independent identity, with the family UiInfo
  // block. backend_name and engine_version match what Probe reports.
  {
    grpc::ClientContext ctx;
    pdfv1::ServiceInfoRequest request;
    pdfv1::ServiceInfoResponse response;
    grpc::Status status = stub->GetServiceInfo(&ctx, request, &response);
    Check(status.ok(), "GetServiceInfo RPC succeeded");
    Check(response.backend_name() == "grpc-pdfium",
          "service info reports the backend name");
    Check(response.engine_version() == "pdfium 154.0.8035.0 (chromium/8035)",
          "service info reports the engine version");
    Check(!response.build_version().empty(),
          "service info reports a build version");
    Check(response.ui().title() == "PDFium", "service info carries UiInfo");
    Check(response.ui().path() == "/ui/pdfium", "UiInfo mounts under /ui");
    Check(!response.ui().description().empty(),
          "UiInfo carries a description");
  }

  // Parse the fixture: header with inventory, a font table entry, the text
  // cell, and trailer counts.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(fixture);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse message;

    Check(reader->Read(&message) && message.has_header(),
          "Parse starts with the header");
    const auto& header = message.header();
    Check(header.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
          "header capabilities show a loaded document");
    Check(header.pages_size() == 1, "header inventory lists one page");
    if (header.pages_size() == 1) {
      const auto& page = header.pages(0);
      Check(page.width_pts() > 611.0 && page.width_pts() < 613.0,
            "page width is Letter");
      Check(page.height_pts() > 791.0 && page.height_pts() < 793.0,
            "page height is Letter");
      Check(page.rotation_degrees() == 0, "page is unrotated");
      Check(page.has_media_box() && page.has_crop_box(),
            "page boxes are populated");
    }

    std::string all_text;
    bool saw_fonts = false;
    bool saw_trailer = false;
    bool bbox_sane = true;
    uint64_t trailer_cells = 0;
    while (reader->Read(&message)) {
      if (message.has_page()) {
        for (const auto& cell : message.page().text_cells()) {
          all_text += cell.text() + " ";
          if (cell.bbox().x0() < 0 || cell.bbox().x1() > 612 ||
              cell.bbox().y0() < 600 || cell.bbox().y1() > 792) {
            bbox_sane = false;
          }
        }
      } else if (message.has_fonts()) {
        for (const auto& font : message.fonts().fonts()) {
          if (font.base_name().find("Helvetica") != std::string::npos) {
            saw_fonts = true;
          }
        }
      } else if (message.has_trailer()) {
        saw_trailer = true;
        for (const auto& count : message.trailer().counts()) {
          if (count.family() == pdfv1::PDF_FAMILY_TEXT_CELLS) {
            trailer_cells = count.count();
          }
        }
      }
    }
    Check(reader->Finish().ok(), "Parse stream finished OK");
    Check(all_text.find("Hello PDF") != std::string::npos,
          "text cells contain the fixture text");
    Check(bbox_sane, "text cell bboxes sit where the fixture drew them");
    Check(saw_fonts, "font table names Helvetica");
    Check(saw_trailer, "Parse ends with the trailer");
    Check(trailer_cells >= 1, "trailer counts the text cells");
  }

  // Render at 72 DPI: one Letter-sized BGR8 raster with ink on it.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(fixture);
    request.set_dpi(72.0);
    request.set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse message;
    Check(reader->Read(&message), "Render produced a raster");
    const auto& raster = message.raster();
    Check(raster.width_px() == 612 && raster.height_px() == 792,
          "raster is Letter at 72 DPI");
    Check(raster.pixel_format() == pdfv1::PIXEL_FORMAT_BGR8,
          "raster is BGR8");
    Check(raster.stride_bytes() >= 612 * 3, "raster stride covers the row");
    Check(raster.pixels().size() ==
              static_cast<size_t>(raster.stride_bytes()) * raster.height_px(),
          "raster payload matches stride * height");
    bool has_ink = false;
    for (unsigned char b : raster.pixels()) {
      if (b != 0xFF) {
        has_ink = true;
        break;
      }
    }
    Check(has_ink, "raster has non-white pixels (the text)");
    Check(!reader->Read(&message), "single-page fixture renders one raster");
    Check(reader->Finish().ok(), "Render stream finished OK");
  }

  // Render with a non-positive DPI: INVALID_ARGUMENT.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(fixture);
    request.set_dpi(0.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse message;
    Check(!reader->Read(&message), "zero-DPI render produced nothing");
    Check(reader->Finish().error_code() == grpc::INVALID_ARGUMENT,
          "zero DPI is INVALID_ARGUMENT");
  }

  // DPI that is not finite, or above the cap, is INVALID_ARGUMENT before
  // any pixel arithmetic runs.
  for (double dpi : {std::nan(""), std::numeric_limits<double>::infinity(),
                     1.0e9}) {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(fixture);
    request.set_dpi(dpi);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse message;
    Check(!reader->Read(&message), "out-of-range DPI rendered nothing");
    Check(reader->Finish().error_code() == grpc::INVALID_ARGUMENT,
          "non-finite or excessive DPI is INVALID_ARGUMENT");
  }

  // Page ranges the contract forbids (end not above begin) are
  // INVALID_ARGUMENT before anything is loaded. Every other range is
  // served, bounds above INT_MAX included: they are clamped to the page
  // count before they become page indexes.
  {
    struct RangeCase {
      uint32_t begin;
      uint32_t end;
      const char* what;
    };
    const RangeCase bad_ranges[] = {
        {0xFFFFFFFFu, 0xFFFFFFFFu, "Parse range at UINT32_MAX is INVALID_ARGUMENT"},
        {0u, 0u, "empty Parse range is INVALID_ARGUMENT"},
        {1u, 0u, "inverted Parse range is INVALID_ARGUMENT"},
    };
    for (const RangeCase& bad : bad_ranges) {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(fixture);
      request.mutable_pages()->set_begin(bad.begin);
      request.mutable_pages()->set_end(bad.end);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      bool streamed = false;
      while (reader->Read(&message)) streamed = true;
      Check(!streamed && reader->Finish().error_code() == grpc::INVALID_ARGUMENT,
            bad.what);
    }
    // Bounds above INT_MAX: {0, UINT32_MAX} is "to the end" and selects
    // the one page; a range wholly above INT_MAX selects none.
    const RangeCase wide_ranges[] = {
        {0u, 0xFFFFFFFFu, "Parse range to UINT32_MAX serves the page"},
        {0u, 0x80000000u, "Parse range ending above INT_MAX serves the page"},
        {0x80000000u, 0x80000001u, "Parse range above INT_MAX serves no page"},
    };
    for (const RangeCase& wide : wide_ranges) {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(fixture);
      request.mutable_pages()->set_begin(wide.begin);
      request.mutable_pages()->set_end(wide.end);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      int pages = 0;
      bool saw_trailer = false;
      while (reader->Read(&message)) {
        if (message.has_page()) ++pages;
        if (message.has_trailer()) saw_trailer = true;
      }
      Check(reader->Finish().ok() && saw_trailer &&
                pages == (wide.begin == 0 ? 1 : 0),
            wide.what);
    }
    {
      grpc::ClientContext ctx;
      pdfv1::RenderRequest request;
      request.mutable_document()->set_data(fixture);
      request.set_dpi(72.0);
      request.mutable_pages()->set_begin(0);
      request.mutable_pages()->set_end(0xFFFFFFFFu);
      auto reader = stub->Render(&ctx, request);
      pdfv1::RenderResponse message;
      int rasters = 0;
      while (reader->Read(&message)) {
        if (message.has_raster()) ++rasters;
      }
      Check(reader->Finish().ok() && rasters == 1,
            "Render range to UINT32_MAX renders every page");
    }
    // A valid range past the last page is clamped: the header and trailer
    // arrive, no page chunk does.
    {
      grpc::ClientContext ctx;
      pdfv1::ParseRequest request;
      request.mutable_document()->set_data(fixture);
      request.mutable_pages()->set_begin(5);
      request.mutable_pages()->set_end(9);
      auto reader = stub->Parse(&ctx, request);
      pdfv1::ParseResponse message;
      bool saw_header = false;
      bool saw_page = false;
      bool saw_trailer = false;
      while (reader->Read(&message)) {
        if (message.has_header()) saw_header = message.header().pages_size() == 1;
        if (message.has_page()) saw_page = true;
        if (message.has_trailer()) saw_trailer = true;
      }
      Check(reader->Finish().ok(), "range past the last page parses OK");
      Check(saw_header && saw_trailer && !saw_page,
            "range past the last page streams the inventory and no page");
    }
  }

  // Render bytes that never loaded: the contract types the load failure in
  // one head message and ends the stream OK, never a bare gRPC error.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data("not a pdf");
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse message;
    Check(reader->Read(&message) && message.has_head() && !message.has_raster(),
          "unloadable render answers with the head");
    Check(message.head().load_status() == pdfv1::LOAD_STATUS_NOT_PDF,
          "unloadable render head reports LOAD_STATUS_NOT_PDF");
    Check(!reader->Read(&message), "unloadable render ends after the head");
    Check(reader->Finish().ok(), "unloadable document is not a gRPC error");
  }

  // A raster above the 512 MiB ceiling (an A0 page at 400 DPI is about
  // 744 MB of BGR) is RESOURCE_EXHAUSTED before any pixel is allocated.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(BlankPdf(2384, 3370));
    request.set_dpi(400.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse message;
    Check(!reader->Read(&message), "oversized raster is not sent");
    Check(reader->Finish().error_code() == grpc::RESOURCE_EXHAUSTED,
          "oversized raster is RESOURCE_EXHAUSTED");
  }

  server->Shutdown();
  if (failures == 0) {
    std::printf("contract_roundtrip: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "contract_roundtrip: %d check(s) failed\n", failures);
  return 1;
}
