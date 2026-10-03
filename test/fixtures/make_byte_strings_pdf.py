#!/usr/bin/env python3
"""Regenerates byte-strings.pdf: one Letter page whose raw byte strings are
not valid UTF-8, in every place the backend copies stored bytes into a
proto string field rather than decoding text:

  link URI           (https://example.com/caf\\351)  a Latin-1 e-acute
  outline URI        (https://example.com/\\300\\257) an overlong form
  signature /SubFilter  /adbe.x509.#CB#CE          GBK bytes in a name
  signature /M          (D:20260904110000\\377)     a stray 0xFF

Each must reach the client as valid UTF-8 (each ill-formed byte read as
Latin-1); without the conversion the message carrying it fails to parse
on the client and the whole Parse stream is lost.
"""

from pathlib import Path

content = b"BT /F1 24 Tf 100 700 Td (Byte strings) Tj ET"

objects = [
    b"<< /Type /Catalog /Pages 2 0 R /Outlines 8 0 R "
    b"/AcroForm << /Fields [6 0 R] /SigFlags 3 >> >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R "
    b"/Annots [6 0 R 10 0 R] >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n"
    + content + b"\nendstream",
    # 6: signature field widget
    b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature1) "
    b"/Rect [100 100 300 150] /F 4 /V 7 0 R >>",
    # 7: signature dictionary (placeholder cryptography)
    b"<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.x509.#CB#CE "
    b"/Contents <DEADBEEF> /ByteRange [0 10 20 30] "
    b"/M (D:20260904110000\\377) >>",
    # 8: outline root
    b"<< /Type /Outlines /First 9 0 R /Last 9 0 R /Count 1 >>",
    # 9: outline item with a URI action
    b"<< /Title (Site) /Parent 8 0 R "
    b"/A << /S /URI /URI (https://example.com/\\300\\257) >> >>",
    # 10: URI link annotation
    b"<< /Type /Annot /Subtype /Link /Rect [72 650 300 690] /F 4 "
    b"/A << /S /URI /URI (https://example.com/caf\\351) >> >>",
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

Path(__file__).with_name("byte-strings.pdf").write_bytes(bytes(out))
print(f"wrote byte-strings.pdf ({len(out)} bytes)")
