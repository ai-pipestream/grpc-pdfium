#!/usr/bin/env python3
"""Regenerates font-names.pdf: one Letter page with a word in each of two
non-embedded fonts whose /BaseFont names are not ASCII.

  F1 /#CB#CE#CC#E5        the GBK bytes of SimSun's Chinese name, not
                          valid UTF-8: the font table must carry it as
                          Latin-1 per byte (U+00CB U+00CE U+00CC U+00E5)
  F2 /#E5#AE#8B#E4#BD#93  the same name in UTF-8, which passes unchanged

Without the conversion the first name puts invalid UTF-8 in a proto3
string field, and the client cannot parse the stream at all.
"""

from pathlib import Path

objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R /F2 5 0 R >> >> /Contents 6 0 R >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /#CB#CE#CC#E5 >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /#E5#AE#8B#E4#BD#93 >>",
]
content = (
    b"BT /F1 24 Tf 100 700 Td (Legacy) Tj ET\n"
    b"BT /F2 24 Tf 100 650 Td (Unicode) Tj ET"
)
objects.append(
    b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n"
    + content + b"\nendstream"
)

out = bytearray(b"%PDF-1.4\n")
offsets = []
for i, body in enumerate(objects, start=1):
    offsets.append(len(out))
    out += str(i).encode() + b" 0 obj\n" + body + b"\nendobj\n"

xref_pos = len(out)
out += b"xref\n0 " + str(len(objects) + 1).encode() + b"\n"
out += b"0000000000 65535 f \n"
for off in offsets:
    out += f"{off:010d} 00000 n \n".encode()
out += (
    b"trailer\n<< /Size " + str(len(objects) + 1).encode() + b" /Root 1 0 R >>\n"
    b"startxref\n" + str(xref_pos).encode() + b"\n%%EOF\n"
)

Path(__file__).with_name("font-names.pdf").write_bytes(bytes(out))
print(f"wrote font-names.pdf ({len(out)} bytes)")
