#!/usr/bin/env python3
"""Regenerates huge-image.pdf: one Letter page placing an image whose
dictionary declares 60000 x 60000 RGB pixels (about 10.8 GB decoded) while
its stream carries three bytes, the shape of a decompression bomb. The
engine must report the placement and leave the pixels out, never decode
them."""

from pathlib import Path


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    # 3: page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>",
    # 4: content: the image placed at 72 x 72 points
    stream(b"", b"q 72 0 0 72 100 600 cm /Im0 Do Q"),
    # 5: the image
    stream(
        b"/Type /XObject /Subtype /Image /Width 60000 /Height 60000 "
        b"/ColorSpace /DeviceRGB /BitsPerComponent 8",
        b"\xff\x00\x00",
    ),
]

out = bytearray(b"%PDF-1.7\n")
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

Path(__file__).with_name("huge-image.pdf").write_bytes(bytes(out))
print(f"wrote huge-image.pdf ({len(out)} bytes)")
