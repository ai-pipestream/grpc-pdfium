// Ligatures pdfium cannot name: GlyphNameText and CffEncodingNames on
// their own, then cff-ligatures.pdf through PdfiumEngine::Parse, where the
// codes 0x1B (f_i) and 0x1D (f_f) read as text, not as word breaks.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "glyph_names.h"
#include "pdfium_engine.h"

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

void CheckNames() {
  using grpc_pdfium::GlyphNameText;
  Check(GlyphNameText("f_i") == U"fi", "f_i is fi");
  Check(GlyphNameText("f_f_i") == U"ffi", "f_f_i is ffi");
  Check(GlyphNameText("fl") == U"fl", "fl is fl");
  Check(GlyphNameText("f_i.alt") == U"fi", "a suffix is dropped");
  Check(GlyphNameText("uni0041") == U"A", "uni0041 is A");
  Check(GlyphNameText("uni00410042") == U"AB", "uni with two code points");
  Check(GlyphNameText("u1D400") == U"\U0001D400", "u with five hex digits");
  Check(GlyphNameText("comma") == U",", "an ASCII glyph name");
  Check(GlyphNameText("emdash") == U"—", "a named glyph outside ASCII");
  Check(GlyphNameText("f_xyzzy").empty(), "an unknown component names nothing");
  Check(GlyphNameText(".notdef").empty(), ".notdef names nothing");
  Check(GlyphNameText("uniD800").empty(), "a surrogate names nothing");
}

void CheckParser() {
  using grpc_pdfium::CffEncodingNames;
  Check(CffEncodingNames("").empty(), "an empty program has no names");
  Check(CffEncodingNames(std::string("\x01\x00\x04\x01\xff\xff", 6)).empty(), "a broken INDEX has no names");
  Check(CffEncodingNames("OTTO and so on").empty(), "an OpenType wrapper has no names");
}

std::string CellTexts(const std::string& pdf) {
  pdfv1::ParseRequest request;
  request.mutable_document()->set_data(pdf);
  std::string all;
  grpc_pdfium::PdfiumEngine::Parse(request, [&](const pdfv1::ParseResponse& msg) {
    if (msg.has_page()) {
      for (const auto& cell : msg.page().text_cells()) all += (all.empty() ? "" : "|") + cell.text();
    }
    return true;
  });
  return all;
}

void CheckFixture(const std::string& dir) {
  const std::string pdf = ReadFile(dir + "/cff-ligatures.pdf");
  Check(!pdf.empty(), "cff-ligatures.pdf read");
  const std::string got = CellTexts(pdf);
  Check(got == "find|different", "cff-ligatures cells: got " + got + ", want find|different");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
    return 2;
  }
  grpc_pdfium::PdfiumEngine::InitProcess();
  CheckNames();
  CheckParser();
  CheckFixture(argv[1]);
  if (failures == 0) std::printf("glyph_names_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
