#include "word_breaks.h"

#include <algorithm>

namespace grpc_pdfium {

namespace {

constexpr double kSizeRatio = 1.2;
constexpr double kBaselineShare = 0.15;

bool IsMark(unsigned int code) {
  return (code >= '0' && code <= '9') || code == ',' || code == '*' || code == 0x2020 ||
         code == 0x2021 || code == 0xA7 || code == 0xB6;
}

}  // namespace

bool ScriptShiftBreak(const CharPlacement& prev, const CharPlacement& next) {
  if (prev.font_size <= 0 || next.font_size <= 0) return false;
  const double larger = std::max(prev.font_size, next.font_size);
  const double smaller = std::min(prev.font_size, next.font_size);
  if (larger < smaller * kSizeRatio) return false;
  const CharPlacement& mark = prev.font_size < next.font_size ? prev : next;
  const CharPlacement& word = prev.font_size < next.font_size ? next : prev;
  if (!IsMark(mark.code)) return false;
  return mark.origin_y - word.origin_y >= kBaselineShare * larger;
}

}  // namespace grpc_pdfium
