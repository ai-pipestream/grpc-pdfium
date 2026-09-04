#!/usr/bin/env python3
"""Regenerates hello.pdf: one Letter page, Helvetica 24pt "Hello PDF" at
(100, 700). Kept as a generator so the fixture is reproducible; the checked
in hello.pdf is the test input."""

from pathlib import Path

objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
]
content = b"BT /F1 24 Tf 100 700 Td (Hello PDF) Tj ET"
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

Path(__file__).with_name("hello.pdf").write_bytes(bytes(out))
print(f"wrote hello.pdf ({len(out)} bytes)")
