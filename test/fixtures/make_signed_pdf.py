#!/usr/bin/env python3
"""Regenerates signed.pdf: hello.pdf's page plus one signature field whose
value is a placeholder PKCS#7 blob with a plausible ByteRange. The engine
reports what is stored without verifying, which is exactly the contract's
SignatureInfo behavior under test."""

from pathlib import Path

content = b"BT /F1 24 Tf 100 700 Td (Signed Doc) Tj ET"

objects = [
    b"<< /Type /Catalog /Pages 2 0 R "
    b"/AcroForm << /Fields [6 0 R] /SigFlags 3 >> >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R "
    b"/Annots [6 0 R] >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n"
    + content + b"\nendstream",
    # 6: signature field widget
    b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature1) "
    b"/Rect [100 100 300 150] /F 4 /V 7 0 R >>",
    # 7: signature dictionary (placeholder cryptography)
    b"<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached "
    b"/Contents <DEADBEEFCAFEF00D> /ByteRange [0 840 856 120] "
    b"/Reason (fixture signature) /M (D:20260904110000Z) >>",
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

Path(__file__).with_name("signed.pdf").write_bytes(bytes(out))
print(f"wrote signed.pdf ({len(out)} bytes)")
