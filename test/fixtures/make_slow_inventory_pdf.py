#!/usr/bin/env python3
"""Regenerates slow-inventory.pdf: 100 blank Letter pages that share one
Flate content stream of 200,000 path operators (about 3 MB inflated, a few
kilobytes stored). PDFium parses a page's content when it loads the page,
so each load costs tens of milliseconds and a first Parse's page
inventory, which loads every page before the header can go out, takes
several seconds while forwarding nothing. The pool test runs it under a
one-second watchdog: the inventory's progress signals must keep the
worker alive, though no single step comes near the limit.
"""

import zlib
from pathlib import Path

PAGES = 100
content = zlib.compress(b"0 0 m 1 1 l S\n" * 200_000, 9)

kids = b" ".join(f"{5 + i} 0 R".encode() for i in range(PAGES))
objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [" + kids + b"] /Count " + str(PAGES).encode() + b" >>",
    b"<< /Length " + str(len(content)).encode() + b" /Filter /FlateDecode >>\n"
    b"stream\n" + content + b"\nendstream",
    b"<< >>",
]
for _ in range(PAGES):
    objects.append(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Contents 3 0 R /Resources 4 0 R >>"
    )

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

Path(__file__).with_name("slow-inventory.pdf").write_bytes(bytes(out))
print(f"wrote slow-inventory.pdf ({len(out)} bytes)")
