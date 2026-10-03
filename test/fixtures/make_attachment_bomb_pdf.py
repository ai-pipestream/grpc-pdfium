#!/usr/bin/env python3
"""Regenerates attachment-bomb.pdf: one blank Letter page and one embedded
file whose stream is zeros compressed twice (/Filter [/FlateDecode
/FlateDecode]), so about two kilobytes of PDF inflate to 1 GiB, the most
PDFium decodes from one stream. PDFium decodes the whole stream even to
report the attachment's size, so the bound that holds here is the worker's
address-space limit, not any check in the engine."""

import zlib
from pathlib import Path

INFLATED_MIB = 1024


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


inner = zlib.compressobj(9)
chunk = bytes(1 << 20)
once = b"".join(inner.compress(chunk) for _ in range(INFLATED_MIB)) + inner.flush()
twice = zlib.compress(once, 9)

objects = [
    # 1: catalog with the embedded-files name tree
    b"<< /Type /Catalog /Pages 2 0 R "
    b"/Names << /EmbeddedFiles << /Names [(zeros.bin) 4 0 R] >> >> >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    # 3: page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
    # 4: file specification
    b"<< /Type /Filespec /F (zeros.bin) /UF (zeros.bin) /EF << /F 5 0 R >> >>",
    # 5: the embedded file
    stream(b"/Type /EmbeddedFile /Filter [/FlateDecode /FlateDecode]", twice),
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

Path(__file__).with_name("attachment-bomb.pdf").write_bytes(bytes(out))
print(f"wrote attachment-bomb.pdf ({len(out)} bytes)")
