#!/usr/bin/env python3
"""Regenerates rich.pdf, the tier 1-2 fixture: one Letter page carrying
document metadata, a two-item outline, a URI link and a goto link, a
highlight and a sticky-note annotation, a text form field, document
JavaScript, an embedded CSV attachment, a placed 4x4 RGB image, a filled
and stroked path with a bezier, a page thumbnail, an embedded TrueType
font (UbuntuMono, UFL-licensed), and a tagged structure tree with one P
element over MCID 0."""

from pathlib import Path

FONT_PATH = Path("/usr/share/fonts/truetype/ubuntu/UbuntuMono[wght].ttf")
font_data = FONT_PATH.read_bytes()

content = (
    b"/P <</MCID 0>> BDC BT /F1 18 Tf 72 720 Td (Tagged Hello) Tj ET EMC\n"
    b"BT /F2 14 Tf 72 690 Td (Embedded) Tj ET\n"
    b"q 100 0 0 50 300 600 cm /Im0 Do Q\n"
    b"1 0 0 RG 0 0 1 rg 2 w 100 100 m 200 100 l 200 150 220 200 150 200 c h B\n"
)

image_pixels = bytes(
    [255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0] * 4  # 4x4 RGB rows
)
thumb_pixels = bytes(range(0, 256, 4))  # 8x8 gray gradient

widths = b" ".join(b"500" for _ in range(91))


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R /MarkInfo << /Marked true >> "
    b"/StructTreeRoot 15 0 R /AcroForm << /Fields [11 0 R] /DA (/Helv 0 Tf 0 g) "
    b"/DR << /Font << /Helv 4 0 R >> >> >> "
    b"/Names << /EmbeddedFiles << /Names [(report.csv) 13 0 R] >> "
    b"/JavaScript << /Names [(init) 14 0 R] >> >> >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    # 3: page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /StructParents 0 "
    b"/Resources << /Font << /F1 4 0 R /F2 20 0 R >> "
    b"/XObject << /Im0 19 0 R >> >> /Contents 5 0 R "
    b"/Annots [9 0 R 10 0 R 11 0 R 12 0 R 23 0 R] /Thumb 18 0 R >>",
    # 4: Helvetica
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    # 5: content
    stream(b"", content),
    # 6: outline root
    b"<< /Type /Outlines /First 7 0 R /Last 8 0 R /Count 2 >>",
    # 7: outline item 1 (goto)
    b"<< /Title (Chapter One) /Parent 6 0 R /Next 8 0 R "
    b"/Dest [3 0 R /XYZ 72 720 1.5] >>",
    # 8: outline item 2 (URI action)
    b"<< /Title (Project Site) /Parent 6 0 R /Prev 7 0 R "
    b"/A << /S /URI /URI (https://example.com/site) >> >>",
    # 9: URI link annotation
    b"<< /Type /Annot /Subtype /Link /Rect [72 700 200 740] /F 4 "
    b"/A << /S /URI /URI (https://example.com/spec) >> >>",
    # 10: highlight annotation
    b"<< /Type /Annot /Subtype /Highlight /Rect [70 640 210 660] "
    b"/QuadPoints [70 660 210 660 70 640 210 640] /C [1 1 0] "
    b"/T (reviewer) /Contents (looks right) /M (D:20260904120000Z) /F 4 >>",
    # 11: text form field widget
    b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (customer_name) "
    b"/V (Jordan Example) /TU (Customer name) /Rect [300 300 450 320] /F 4 >>",
    # 12: sticky note annotation
    b"<< /Type /Annot /Subtype /Text /Rect [500 700 520 720] "
    b"/Contents (remember this corner) /T (author-two) /F 4 >>",
    # 13: attachment filespec
    b"<< /Type /Filespec /F (report.csv) /UF (report.csv) "
    b"/Desc (tiny attachment) /EF << /F 22 0 R >> >>",
    # 14: javascript action
    b"<< /S /JavaScript /JS (app.beep\\(0\\);) >>",
    # 15: struct tree root
    b"<< /Type /StructTreeRoot /K 16 0 R /ParentTree 17 0 R >>",
    # 16: struct element
    b"<< /Type /StructElem /S /P /P 15 0 R /Pg 3 0 R /K 0 "
    b"/Alt (a tagged paragraph) /Lang (en) /T (para one) >>",
    # 17: parent tree
    b"<< /Nums [0 [16 0 R]] >>",
    # 18: thumbnail (8x8 gray)
    stream(
        b"/Type /XObject /Subtype /Image /Width 8 /Height 8 "
        b"/ColorSpace /DeviceGray /BitsPerComponent 8",
        thumb_pixels,
    ),
    # 19: placed image (4x4 RGB)
    stream(
        b"/Type /XObject /Subtype /Image /Width 4 /Height 4 "
        b"/ColorSpace /DeviceRGB /BitsPerComponent 8",
        image_pixels,
    ),
    # 20: embedded TrueType font
    b"<< /Type /Font /Subtype /TrueType /BaseFont /UbuntuMono "
    b"/FirstChar 32 /LastChar 122 /Widths [" + widths + b"] "
    b"/FontDescriptor 21 0 R >>",
    # 21: font descriptor
    b"<< /Type /FontDescriptor /FontName /UbuntuMono /Flags 33 "
    b"/FontBBox [0 -200 600 800] /ItalicAngle 0 /Ascent 800 /Descent -200 "
    b"/CapHeight 700 /StemV 80 /FontFile2 24 0 R >>",
    # 22: embedded file stream
    stream(
        b"/Type /EmbeddedFile /Subtype /text#2Fcsv "
        b"/Params << /Size 18 /CreationDate (D:20260904090000Z) >>",
        b"id,total\n1,999.99\n",
    ),
    # 23: internal goto link annotation
    b"<< /Type /Annot /Subtype /Link /Rect [72 660 200 690] /F 4 "
    b"/Dest [3 0 R /XYZ 100 500 2.0] >>",
    # 24: font program
    stream(b"/Length1 " + str(len(font_data)).encode(), font_data),
]

info = (
    b"<< /Title (Rich Fixture) /Author (Fixture Author) /Subject (tier 1-2) "
    b"/Keywords (rich fixture families) /Creator (make_rich_pdf.py) "
    b"/Producer (by hand) /CreationDate (D:20260904090000Z) "
    b"/ModDate (D:20260904100000Z) >>"
)
objects.append(info)  # 25

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
    b"trailer\n<< /Size " + str(len(objects) + 1).encode()
    + b" /Root 1 0 R /Info " + str(len(objects)).encode() + b" 0 R >>\n"
    b"startxref\n" + str(xref_pos).encode() + b"\n%%EOF\n"
)

Path(__file__).with_name("rich.pdf").write_bytes(bytes(out))
print(f"wrote rich.pdf ({len(out)} bytes, font {len(font_data)} bytes)")
