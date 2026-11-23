// Word cells around superscripts and subscripts: ScriptShiftBreak on its
// own, then superscript.pdf through PdfiumEngine::Parse, where a mark glued
// to its word ("ETT1", "1https") starts its own cell and small caps on one
// baseline stay one word.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pdfium_engine.h"
#include "word_breaks.h"

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::vector<std::string> CellTexts(const std::string& pdf) {
  pdfv1::ParseRequest request;
  request.mutable_document()->set_data(pdf);
  std::vector<std::string> texts;
  grpc_pdfium::PdfiumEngine::Parse(request, [&](const pdfv1::ParseResponse& msg) {
    if (msg.has_page()) {
      for (const auto& cell : msg.page().text_cells()) texts.push_back(cell.text());
    }
    return true;
  });
  return texts;
}

// A one-page PDF drawing content in Helvetica at /F1.
std::string OnePagePdf(const std::string& content) {
  const std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
          content + "\nendstream",
  };
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

// The TEXT_CELLS warnings a Parse of pdf ends with, as "page:message".
std::vector<std::string> TextWarnings(const std::string& pdf) {
  pdfv1::ParseRequest request;
  request.mutable_document()->set_data(pdf);
  std::vector<std::string> out;
  grpc_pdfium::PdfiumEngine::Parse(request, [&](const pdfv1::ParseResponse& msg) {
    if (msg.has_trailer()) {
      for (const auto& w : msg.trailer().warnings()) {
        if (w.has_family() && w.family() == pdfv1::PDF_FAMILY_TEXT_CELLS) {
          out.push_back((w.has_page_index() ? std::to_string(w.page_index()) : "-") +
                        ":" + w.message());
        }
      }
    }
    return true;
  });
  return out;
}

bool AnyStartsWith(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const auto& line : lines) {
    if (line.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

std::string Joined(const std::vector<std::string>& texts) {
  std::string out;
  for (const auto& t : texts) out += (out.empty() ? "" : "|") + t;
  return out;
}

void CheckRule() {
  using grpc_pdfium::CharPlacement;
  using grpc_pdfium::ScriptShiftBreak;
  const CharPlacement body{'T', 9.0, 100, 600};
  Check(ScriptShiftBreak(body, {'1', 6.5, 120, 603.25}), "a raised smaller digit after a word breaks");
  Check(ScriptShiftBreak({'1', 5.5, 100, 502.6}, {'h', 7.0, 104, 500}), "a word after a raised mark breaks");
  Check(ScriptShiftBreak(body, {0x2020, 6.5, 120, 603.25}), "a raised dagger breaks");
  Check(!ScriptShiftBreak({'L', 9.0, 100, 600}, {'t', 6.5, 106, 597}), "a lettered subscript stays joined");
  Check(!ScriptShiftBreak({'x', 9.0, 100, 600}, {'2', 6.5, 106, 597}), "a lowered digit stays joined");
  Check(!ScriptShiftBreak({'x', 9.0, 100, 600}, {'T', 6.5, 106, 603.25}), "a lettered superscript stays joined");
  Check(!ScriptShiftBreak({'T', 10.0, 100, 400}, {'E', 8.0, 107, 400}), "small caps on one baseline stay one word");
  Check(!ScriptShiftBreak(body, {'1', 9.0, 105, 602}), "a baseline shift at one size stays one word");
  Check(!ScriptShiftBreak(body, {'1', 0.0, 105, 610}), "an unknown size never breaks");
}

void CheckFixture(const std::string& dir) {
  const std::string pdf = ReadFile(dir + "/superscript.pdf");
  Check(!pdf.empty(), "superscript.pdf read");
  const std::vector<std::string> texts = CellTexts(pdf);
  const std::string all = Joined(texts);
  const std::string want = "ETT|1|contains|1|https://example.org/a|TEX";
  Check(all == want, "superscript cells: got " + all + ", want " + want);
}

// Text the cells do not carry is counted in the trailer, page by page:
// glyphs PDFium could not map to Unicode that the word splitter read as
// breaks, and words left out for want of a character box. Clean pages and
// glyphs recovered from the font's own names warn about nothing.
void CheckUnmappedGlyphs(const std::string& dir) {
  Check(TextWarnings(ReadFile(dir + "/hello.pdf")).empty(),
        "a clean page has no text warning");
  Check(TextWarnings(ReadFile(dir + "/cff-ligatures.pdf")).empty(),
        "ligatures recovered from glyph names are not counted as lost");

  // Eight control-code glyphs with no Unicode mapping, then a caption.
  const std::vector<std::string> control =
      TextWarnings(ReadFile(dir + "/control-glyphs.pdf"));
  Check(AnyStartsWith(control, "0:8 glyph(s) "),
        "eight unmapped control-code glyphs are counted for their page: got " +
            Joined(control));
  Check(control.size() == 1, "and nothing else is warned about on that page");

  // A glyph at code 0x02 that is not a line-break hyphen: PDFium's own
  // marker for a hyphen is the same number. It is not a soft hyphen, so
  // the words either side stay apart and the glyph is counted.
  const std::string code_two =
      OnePagePdf("BT /F1 12 Tf 72 700 Td (ab\\002cd) Tj ET");
  const std::string cells = Joined(CellTexts(code_two));
  Check(cells == "ab|cd", "a glyph at code 2 is not read as a hyphen: got " + cells);
  Check(AnyStartsWith(TextWarnings(code_two), "0:1 glyph(s) "),
        "the glyph at code 2 is counted");

  // A word drawn with a text matrix of no height has no character box and
  // cannot be a cell; it is counted, not dropped without a word.
  const std::string flat = OnePagePdf(
      "BT /F1 12 Tf 72 700 Td (seen) Tj ET "
      "BT /F1 12 Tf 1 0 0 0 72 600 Tm (flat) Tj ET");
  const std::string flat_cells = Joined(CellTexts(flat));
  Check(flat_cells == "seen", "a word with no box is not a cell: got " + flat_cells);
  const std::vector<std::string> flat_warnings = TextWarnings(flat);
  Check(AnyStartsWith(flat_warnings, "0:1 word(s) "),
        "the word left out for want of a box is counted: got " +
            Joined(flat_warnings));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
    return 2;
  }
  grpc_pdfium::PdfiumEngine::InitProcess();
  CheckRule();
  CheckFixture(argv[1]);
  CheckUnmappedGlyphs(argv[1]);
  if (failures == 0) std::printf("text_cells_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
