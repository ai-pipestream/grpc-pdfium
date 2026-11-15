#include "glyph_recovery.h"

#include <cstdint>
#include <string_view>

#include "fpdf_edit.h"
#include "glyph_names.h"

namespace grpc_pdfium {

const std::vector<std::string>& GlyphRecovery::NamesFor(FPDF_FONT font) {
  auto [it, inserted] = names_.try_emplace(font);
  if (!inserted) return it->second;
  size_t size = 0;
  if (FPDFFont_GetIsEmbedded(font) == 1 && FPDFFont_GetFontData(font, nullptr, 0, &size) && size > 0) {
    std::string data(size, '\0');
    if (FPDFFont_GetFontData(font, reinterpret_cast<uint8_t*>(data.data()), size, &size)) {
      it->second = CffEncodingNames(std::string_view(data.data(), size));
    }
  }
  return it->second;
}

std::u32string GlyphRecovery::Recover(FPDF_TEXTPAGE text_page, int index, unsigned int code) {
  if (code == 0 || code >= 0x20 || FPDFText_IsGenerated(text_page, index) == 1) return {};
  FPDF_PAGEOBJECT text_object = FPDFText_GetTextObject(text_page, index);
  FPDF_FONT font = text_object != nullptr ? FPDFTextObj_GetFont(text_object) : nullptr;
  if (font == nullptr) return {};
  const std::vector<std::string>& names = NamesFor(font);
  if (code >= names.size() || names[code].empty()) return {};
  std::u32string text = GlyphNameText(names[code]);
  for (char32_t c : text) {
    if (c < 0x20) return {};  // never hand back another control code
  }
  return text;
}

}  // namespace grpc_pdfium
