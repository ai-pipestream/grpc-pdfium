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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
    return 2;
  }
  grpc_pdfium::PdfiumEngine::InitProcess();
  CheckRule();
  CheckFixture(argv[1]);
  if (failures == 0) std::printf("text_cells_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
