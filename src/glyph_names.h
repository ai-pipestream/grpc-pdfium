#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace grpc_pdfium {

// The glyph name for each code 0-255 of a bare CFF font program (a Type1C
// FontFile3) with a custom built-in encoding, read from its Encoding and
// charset; an empty string where the encoding maps nothing. Empty when the
// program is not a name-keyed CFF with a custom encoding (a CID-keyed font,
// the predefined Standard or Expert encoding, an OpenType wrapper) or is
// malformed.
std::vector<std::string> CffEncodingNames(std::string_view cff);

// The text a glyph name stands for, read the Adobe Glyph List way: a
// suffix after '.' is dropped, '_' joins components ("f_i" is "fi"), and
// each component is uniXXXX (one or more), uXXXX to uXXXXXX, an ASCII
// glyph name ("A", "comma"), a ligature (ff, fi, fl, ffi, ffl) or one of a
// few common names. Empty when any component is unknown.
std::u32string GlyphNameText(std::string_view name);

}  // namespace grpc_pdfium
