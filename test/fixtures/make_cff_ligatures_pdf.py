#!/usr/bin/env python3
"""Regenerates cff-ligatures.pdf (needs fonttools): one Letter page in an
embedded Type1C font with a custom built-in encoding and no ToUnicode, the
way TeX's LinLibertine subsets arrive. Codes 27 and 29 are the ligatures
f_i and f_f; the font's /Encoding repeats them in /Differences over
WinAnsiEncoding. The page draws "\\x1bnd" and "di\\x1derent" at y 600.
pdfium cannot map the names f_i and f_f, reads codes 0x1B and 0x1D, and a
word split on control codes gives "nd", "di" and "erent". The cells read
"find" and "different".
"""

from io import BytesIO
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString

GLYPHS = [".notdef", "d", "e", "f", "i", "n", "r", "t", "f_i", "f_f"]
CODES = {27: "f_i", 29: "f_f", 0x64: "d", 0x65: "e", 0x66: "f", 0x69: "i", 0x6E: "n", 0x72: "r", 0x74: "t"}
WIDTH = 500


def box() -> T2CharString:
    return T2CharString(program=[WIDTH, 50, 0, "rmoveto", 400, 0, "rlineto", 0, 700, "rlineto", -400, 0, "rlineto", "endchar"])


def cff_program() -> bytes:
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(GLYPHS)
    fb.setupCharacterMap({})
    fb.setupCFF("ABCDEF+Lig", {"FullName": "Lig"}, {g: box() for g in GLYPHS}, {})
    cff = fb.font["CFF "].cff
    cff[cff.fontNames[0]].Encoding = [CODES.get(code, ".notdef") for code in range(256)]
    out = BytesIO()
    cff.compile(out, fb.font)
    return out.getvalue()


program = cff_program()
content = b"BT /F1 12 Tf 100 600 Td (\x1bnd di\x1derent) Tj ET"
widths = " ".join(str(WIDTH if code in CODES else 0) for code in range(27, 117)).encode()
objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+Lig /FirstChar 27 /LastChar 116 /Widths [" + widths + b"]"
    b" /Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [27 /f_i 29 /f_f] >>"
    b" /FontDescriptor 6 0 R >>",
    b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n" + content + b"\nendstream",
    b"<< /Type /FontDescriptor /FontName /ABCDEF+Lig /Flags 4 /FontBBox [0 0 500 700] /ItalicAngle 0 /Ascent 700"
    b" /Descent 0 /CapHeight 700 /StemV 80 /FontFile3 7 0 R >>",
    b"<< /Subtype /Type1C /Length " + str(len(program)).encode() + b" >>\nstream\n" + program + b"\nendstream",
]
out = bytearray(b"%PDF-1.4\n")
offsets = []
for number, body in enumerate(objects, 1):
    offsets.append(len(out))
    out += str(number).encode() + b" 0 obj\n" + body + b"\nendobj\n"
xref = len(out)
out += b"xref\n0 " + str(len(objects) + 1).encode() + b"\n0000000000 65535 f \n"
for offset in offsets:
    out += f"{offset:010d} 00000 n \n".encode()
out += b"trailer\n<< /Size " + str(len(objects) + 1).encode() + b" /Root 1 0 R >>\nstartxref\n" + str(xref).encode() + b"\n%%EOF\n"
Path(__file__).with_name("cff-ligatures.pdf").write_bytes(bytes(out))
print(f"wrote cff-ligatures.pdf ({len(out)} bytes)")
