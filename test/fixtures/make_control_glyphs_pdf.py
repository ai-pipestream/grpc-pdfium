#!/usr/bin/env python3
"""Regenerates control-glyphs.pdf: one Letter page that first draws eight
control-code glyphs (\\x03 \\x0c \\x0b, as figures converted from EPS draw
by the hundred) at y 600, then "Round-wise caption about source selec-" at
y 500 and "tion and more" under it. pdfium counts
the control glyphs as characters but FPDFText_GetText leaves some of them
out, so a backend that indexes the GetText string by character index gives
"Round-wise" the boxes of characters further back, up among the glyphs. The hyphen pdfium takes for a line break reads 0x02
per character and U+FFFE in GetText; the cell keeps U+FFFE, one word.
"""

from pathlib import Path

# Helvetica drawing control-code bytes (as EPS-converted figures do), then a caption word below.
content = b"BT /F1 10 Tf 100 600 Td (\x03\x03\x0c\x0b\x03\x03\x0c\x0b) Tj ET BT /F1 10 Tf 100 500 Td (Round-wise caption about source selec-) Tj 0 -12 Td (tion and more) Tj ET"
objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n" + content + b"\nendstream",
]
out = bytearray(b"%PDF-1.4\n"); offs=[]
for i,b in enumerate(objects,1):
    offs.append(len(out)); out += str(i).encode()+b" 0 obj\n"+b+b"\nendobj\n"
x=len(out); out += b"xref\n0 "+str(len(objects)+1).encode()+b"\n0000000000 65535 f \n"
for o in offs: out += f"{o:010d} 00000 n \n".encode()
out += b"trailer\n<< /Size "+str(len(objects)+1).encode()+b" /Root 1 0 R >>\nstartxref\n"+str(x).encode()+b"\n%%EOF\n"
Path(__file__).with_name("control-glyphs.pdf").write_bytes(bytes(out))
print(f"wrote control-glyphs.pdf ({len(out)} bytes)")
