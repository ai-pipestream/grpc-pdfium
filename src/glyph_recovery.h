#pragma once

#include <map>
#include <string>
#include <vector>

#include "fpdf_text.h"
#include "fpdfview.h"

namespace grpc_pdfium {

// Text for characters pdfium reads as a bare control code. With no
// ToUnicode, pdfium maps a code through its glyph name and falls back to
// the code itself when the name is not one it knows: TeX ligatures named
// f_i, f_f_i and so on read as 0x1B-0x1F, and the word splitter takes them
// for spaces ("di erent"). This reads the glyph name from the embedded CFF
// program's built-in encoding instead. One instance per text page; it
// caches each font's encoding.
class GlyphRecovery {
 public:
  // The text for character `index` whose GetUnicode value is `code`, or
  // empty when nothing is recovered: only non-generated characters with a
  // code below 0x20 are looked at, and only in an embedded CFF font with a
  // custom encoding naming a glyph GlyphNameText knows.
  std::u32string Recover(FPDF_TEXTPAGE text_page, int index, unsigned int code);

 private:
  const std::vector<std::string>& NamesFor(FPDF_FONT font);

  std::map<FPDF_FONT, std::vector<std::string>> names_;
};

}  // namespace grpc_pdfium
