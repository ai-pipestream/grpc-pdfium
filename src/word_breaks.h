#pragma once

namespace grpc_pdfium {

// One character's size and baseline origin, in page space, as the text
// page reports them (FPDFText_GetFontSize, FPDFText_GetCharOrigin).
struct CharPlacement {
  unsigned int code = 0;
  double font_size = 0;
  double origin_x = 0;
  double origin_y = 0;
};

// True when `next` starts a new word although no space separates it from
// `prev`: a raised footnote or reference mark at either end of a word (the
// mark in "1https", "ETT1"). The smaller of the two is a mark character (a
// digit, comma, *, dagger, double dagger, section or pilcrow) drawn at
// least 20 percent smaller, with its baseline at least 15 percent of the
// larger size above the other's. Subscripts and lettered superscripts in
// math ("L_task", "x^T") stay joined, as do small caps on one baseline.
bool ScriptShiftBreak(const CharPlacement& prev, const CharPlacement& next);

}  // namespace grpc_pdfium
