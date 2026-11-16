// CffEncodingNames: reads the header, the Name, Top DICT and String
// INDEXes, the CharStrings count, the charset and the Encoding of a bare
// CFF program (Adobe Technical Note #5176), bounds-checked throughout.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cff_standard_strings.h"
#include "glyph_names.h"

namespace grpc_pdfium {

namespace {

struct Index {
  std::vector<std::string_view> entries;
  size_t end = 0;
};

uint32_t Read(std::string_view d, size_t pos, int bytes) {
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) v = (v << 8) | static_cast<uint8_t>(d[pos + i]);
  return v;
}

std::optional<Index> ReadIndex(std::string_view d, size_t pos) {
  if (pos + 2 > d.size()) return std::nullopt;
  Index out;
  const uint32_t count = Read(d, pos, 2);
  if (count == 0) {
    out.end = pos + 2;
    return out;
  }
  if (pos + 3 > d.size()) return std::nullopt;
  const int off_size = static_cast<uint8_t>(d[pos + 2]);
  if (off_size < 1 || off_size > 4) return std::nullopt;
  const size_t offsets = pos + 3;
  const size_t data = offsets + (count + 1) * static_cast<size_t>(off_size) - 1;
  if (data >= d.size()) return std::nullopt;
  uint32_t prev = Read(d, offsets, off_size);
  for (uint32_t i = 1; i <= count; ++i) {
    const uint32_t next = Read(d, offsets + i * static_cast<size_t>(off_size), off_size);
    if (next < prev || data + next > d.size()) return std::nullopt;
    out.entries.push_back(d.substr(data + prev, next - prev));
    prev = next;
  }
  out.end = data + prev;
  return out;
}

struct TopDict {
  long charset = 0;
  long encoding = 0;
  long char_strings = -1;
  bool cid = false;
};

// The Top DICT operators this needs: charset (15), Encoding (16),
// CharStrings (17) and ROS (12 30, which marks a CID-keyed font).
std::optional<TopDict> ReadTopDict(std::string_view d) {
  TopDict top;
  std::vector<long> operands;
  for (size_t i = 0; i < d.size();) {
    const int b0 = static_cast<uint8_t>(d[i]);
    if (b0 <= 21) {
      int op = b0;
      if (b0 == 12) {
        if (++i >= d.size()) return std::nullopt;
        op = 1200 + static_cast<uint8_t>(d[i]);
      }
      const long last = operands.empty() ? 0 : operands.back();
      if (op == 15) top.charset = last;
      if (op == 16) top.encoding = last;
      if (op == 17) top.char_strings = last;
      if (op == 1230) top.cid = true;
      operands.clear();
      ++i;
    } else if (b0 == 28 || b0 == 29) {
      const int n = b0 == 28 ? 2 : 4;
      if (i + 1 + n > d.size()) return std::nullopt;
      const uint32_t raw = Read(d, i + 1, n);
      operands.push_back(n == 2 ? static_cast<int16_t>(raw) : static_cast<int32_t>(raw));
      i += 1 + n;
    } else if (b0 == 30) {  // a real number: nibbles up to an 0xf nibble
      operands.push_back(0);
      for (++i; i < d.size(); ++i) {
        const int b = static_cast<uint8_t>(d[i]);
        if ((b & 0x0f) == 0x0f || (b >> 4) == 0x0f) break;
      }
      ++i;
    } else if (b0 >= 32 && b0 <= 246) {
      operands.push_back(b0 - 139);
      ++i;
    } else if (b0 >= 247 && b0 <= 254) {
      if (i + 1 >= d.size()) return std::nullopt;
      const long b1 = static_cast<uint8_t>(d[i + 1]);
      operands.push_back(b0 <= 250 ? (b0 - 247) * 256 + b1 + 108 : -(b0 - 251) * 256 - b1 - 108);
      i += 2;
    } else {
      return std::nullopt;
    }
  }
  return top;
}

// The SID of each glyph (GID 0 is .notdef), from charset formats 0-2 or the
// predefined ISOAdobe charset (SID equals GID).
std::optional<std::vector<uint32_t>> ReadCharset(std::string_view d, long offset, size_t glyphs) {
  std::vector<uint32_t> sids(glyphs, 0);
  if (offset == 0) {
    for (size_t g = 0; g < glyphs; ++g) sids[g] = static_cast<uint32_t>(g);
    return sids;
  }
  if (offset < 3 || static_cast<size_t>(offset) >= d.size()) return std::nullopt;
  size_t pos = static_cast<size_t>(offset);
  const int format = static_cast<uint8_t>(d[pos++]);
  for (size_t g = 1; g < glyphs;) {
    if (format == 0) {
      if (pos + 2 > d.size()) return std::nullopt;
      sids[g++] = Read(d, pos, 2);
      pos += 2;
    } else if (format == 1 || format == 2) {
      const size_t step = format == 1 ? 3 : 4;
      if (pos + step > d.size()) return std::nullopt;
      const uint32_t first = Read(d, pos, 2);
      const uint32_t left = Read(d, pos + 2, static_cast<int>(step) - 2);
      pos += step;
      for (uint32_t k = 0; k <= left && g < glyphs; ++k) sids[g++] = first + k;
    } else {
      return std::nullopt;
    }
  }
  return sids;
}

}  // namespace

std::vector<std::string> CffEncodingNames(std::string_view d) {
  if (d.size() < 4 || d[0] != 1) return {};
  const auto names = ReadIndex(d, static_cast<uint8_t>(d[2]));
  if (!names) return {};
  const auto tops = ReadIndex(d, names->end);
  if (!tops || tops->entries.empty()) return {};
  const auto strings = ReadIndex(d, tops->end);
  const auto top = ReadTopDict(tops->entries[0]);
  if (!strings || !top || top->cid || top->encoding <= 1 || top->char_strings < 0) return {};
  const auto char_strings = ReadIndex(d, static_cast<size_t>(top->char_strings));
  if (!char_strings) return {};
  const auto sids = ReadCharset(d, top->charset, char_strings->entries.size());
  if (!sids || static_cast<size_t>(top->encoding) + 2 > d.size()) return {};

  auto name_of = [&](uint32_t sid) -> std::string {
    if (sid < kCffStandardStrings.size()) return std::string(kCffStandardStrings[sid]);
    const size_t at = sid - kCffStandardStrings.size();
    return at < strings->entries.size() ? std::string(strings->entries[at]) : std::string();
  };
  std::vector<std::string> out(256);
  size_t pos = static_cast<size_t>(top->encoding);
  const int format = static_cast<uint8_t>(d[pos]) & 0x7f;
  const bool supplements = (static_cast<uint8_t>(d[pos]) & 0x80) != 0;
  const int n = static_cast<uint8_t>(d[pos + 1]);
  pos += 2;
  size_t gid = 1;
  if (format == 0) {
    if (pos + n > d.size()) return {};
    for (int i = 0; i < n && gid < sids->size(); ++i) out[static_cast<uint8_t>(d[pos + i])] = name_of((*sids)[gid++]);
    pos += n;
  } else if (format == 1) {
    if (pos + 2 * static_cast<size_t>(n) > d.size()) return {};
    for (int r = 0; r < n; ++r, pos += 2) {
      const int first = static_cast<uint8_t>(d[pos]);
      const int left = static_cast<uint8_t>(d[pos + 1]);
      for (int code = first; code <= first + left && code < 256 && gid < sids->size(); ++code) {
        out[code] = name_of((*sids)[gid++]);
      }
    }
  } else {
    return {};
  }
  if (supplements && pos < d.size()) {
    const int count = static_cast<uint8_t>(d[pos++]);
    for (int i = 0; i < count && pos + 3 <= d.size(); ++i, pos += 3) {
      out[static_cast<uint8_t>(d[pos])] = name_of(Read(d, pos + 1, 2));
    }
  }
  return out;
}

}  // namespace grpc_pdfium
