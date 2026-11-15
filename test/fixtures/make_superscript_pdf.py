#!/usr/bin/env python3
"""Regenerates superscript.pdf: one Letter page with marks glued to their
words, no space between them. At y 600 "ETT" (9 pt) carries a reference
mark "1" (6.5 pt, raised 3.25 pt) and then " contains"; at y 500 a footnote
mark "1" (5.5 pt, raised 2.6 pt) sits right before "https://example.org/a"
(7 pt); at y 400 a small-caps "TEX" draws "T" at 10 pt and "EX" at 8 pt on
one baseline. The marks start their own cells; the small caps stay one.
"""

from pathlib import Path

content = (b"BT /F1 9 Tf 100 600 Td (ETT) Tj /F1 6.5 Tf 3.25 Ts (1) Tj /F1 9 Tf 0 Ts ( contains) Tj ET "
           b"BT /F1 5.5 Tf 100 500 Td 2.6 Ts (1) Tj /F1 7 Tf 0 Ts (https://example.org/a) Tj ET "
           b"BT /F1 10 Tf 100 400 Td (T) Tj /F1 8 Tf (EX) Tj ET")
objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n" + content + b"\nendstream",
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
Path(__file__).with_name("superscript.pdf").write_bytes(bytes(out))
print(f"wrote superscript.pdf ({len(out)} bytes)")
