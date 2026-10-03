#!/usr/bin/env python3
"""Regenerates legacy-strings.pdf: one Letter page whose byte strings other
than font names are in a legacy encoding, the GBK bytes CB CE CC E5
(SimSun's Chinese name, not valid UTF-8):

  a URI link annotation whose /URI is those bytes
  an outline item whose URI action's /URI is those bytes
  a signature field whose /SubFilter is /#CB#CE#CC#E5 and whose /M is
  those bytes

PDFium hands each back as raw bytes. Each must reach the client as valid
UTF-8 (Latin-1 per byte, U+00CB U+00CE U+00CC U+00E5), or the proto3 string
field holding it fails the client's parse of the whole stream. font-names.pdf
covers font names.
"""

from pathlib import Path

GBK = b"\xcb\xce\xcc\xe5"
content = b"BT /F1 24 Tf 72 700 Td (Legacy strings) Tj ET"

objects = [
    # 1: catalog with the outline and the signature form
    b"<< /Type /Catalog /Pages 2 0 R /Outlines 8 0 R "
    b"/AcroForm << /Fields [7 0 R] /SigFlags 3 >> >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    # 3: page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R "
    b"/Annots [6 0 R 7 0 R] >>",
    # 4: font
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    # 5: content
    b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n"
    + content + b"\nendstream",
    # 6: URI link
    b"<< /Type /Annot /Subtype /Link /Rect [72 690 300 730] /Border [0 0 0] "
    b"/A << /S /URI /URI (" + GBK + b") >> >>",
    # 7: signature field widget
    b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature1) "
    b"/Rect [100 100 300 150] /F 4 /V 10 0 R >>",
    # 8: outline root
    b"<< /Type /Outlines /First 9 0 R /Last 9 0 R /Count 1 >>",
    # 9: outline item with a URI action
    b"<< /Title (Legacy link) /Parent 8 0 R "
    b"/A << /S /URI /URI (" + GBK + b") >> >>",
    # 10: signature dictionary (placeholder cryptography)
    b"<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /#CB#CE#CC#E5 "
    b"/Contents <DEADBEEF> /ByteRange [0 10 20 10] /M (" + GBK + b") >>",
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

Path(__file__).with_name("legacy-strings.pdf").write_bytes(bytes(out))
print(f"wrote legacy-strings.pdf ({len(out)} bytes)")
