// GlyphNameText: glyph names to text, the Adobe Glyph List rules for
// components and uni/u forms, with the ASCII names from the CFF standard
// strings and a short table of the names TeX fonts use outside ASCII.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "cff_standard_strings.h"
#include "glyph_names.h"

namespace grpc_pdfium {

namespace {

constexpr std::pair<std::string_view, std::u32string_view> kNamed[] = {
    {"ff", U"ff"},           {"fi", U"fi"},           {"fl", U"fl"},
    {"ffi", U"ffi"},         {"ffl", U"ffl"},         {"dotlessi", U"ı"},
    {"germandbls", U"ß"}, {"endash", U"–"}, {"emdash", U"—"},
    {"quoteleft", U"‘"}, {"quoteright", U"’"}, {"quotedblleft", U"“"},
    {"quotedblright", U"”"}, {"quotedblbase", U"„"}, {"bullet", U"•"},
    {"ellipsis", U"…"}, {"minus", U"−"},    {"fraction", U"⁄"},
    {"dagger", U"†"},   {"daggerdbl", U"‡"}, {"section", U"§"},
    {"paragraph", U"¶"}, {"ae", U"æ"},      {"oe", U"œ"},
    {"AE", U"Æ"},       {"OE", U"Œ"},       {"oslash", U"ø"},
    {"Oslash", U"Ø"},
};

bool Hex(std::string_view s, char32_t* out) {
  char32_t v = 0;
  for (char c : s) {
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    v = v * 16 + static_cast<char32_t>(d);
  }
  if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return false;
  *out = v;
  return true;
}

// One component (no '_' or '.'); false when it names nothing known.
bool Component(std::string_view c, std::u32string* out) {
  if (c.size() > 3 && c.substr(0, 3) == "uni" && (c.size() - 3) % 4 == 0) {
    for (size_t i = 3; i < c.size(); i += 4) {
      char32_t v;
      if (!Hex(c.substr(i, 4), &v)) return false;
      out->push_back(v);
    }
    return true;
  }
  if (c.size() >= 5 && c.size() <= 7 && c[0] == 'u') {
    char32_t v;
    if (Hex(c.substr(1), &v)) {
      out->push_back(v);
      return true;
    }
  }
  for (size_t sid = 1; sid <= 95; ++sid) {
    if (kCffStandardStrings[sid] == c) {
      out->push_back(static_cast<char32_t>(0x1F + sid));
      return true;
    }
  }
  for (const auto& [name, text] : kNamed) {
    if (name == c) {
      out->append(text);
      return true;
    }
  }
  return false;
}

}  // namespace

std::u32string GlyphNameText(std::string_view name) {
  name = name.substr(0, name.find('.'));
  std::u32string out;
  while (!name.empty()) {
    const size_t cut = name.find('_');
    if (!Component(name.substr(0, cut), &out)) return {};
    if (cut == std::string_view::npos) break;
    name.remove_prefix(cut + 1);
  }
  return out;
}

}  // namespace grpc_pdfium
