#!/usr/bin/env python3
"""Regenerates form-xobject.pdf: one Letter page whose images, path and one
font live inside Form XObjects, plus an invisible (3 Tr) word, the way OCR
layers and many generators lay pages out.

Page content:
  "Visible" (Helvetica 12, fill) at (72, 740)
  "Invisible" (Helvetica 12, 3 Tr, scoped by q/Q since Tr persists past
  ET) at (72, 720)
  q 1 0 0 1 200 300 cm /Fm0 Do Q

Fm0 (/Matrix [1 0 0 1 10 10]) draws, in its own space:
  the 2x2 image Im0 scaled to 50 x 40 at the origin -> page (210 310 260 350)
  a filled triangle (10 60)(90 60)(90 90)             -> page (220 370 300 400)
  "Inside" in Courier 10 at (0, 120)                  -> page baseline (210, 430)
  the nested form Fm1 (/Matrix [1 0 0 1 100 0]), which draws Im0 scaled
  to 20 x 20 at its origin                            -> page (310 310 330 330)

Courier is used only inside Fm0, so it reaches the font table through the
form walk alone when text cells are not requested.
"""

from pathlib import Path


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


page_content = (
    b"BT /F1 12 Tf 72 740 Td (Visible) Tj ET\n"
    b"q BT 3 Tr /F1 12 Tf 72 720 Td (Invisible) Tj ET Q\n"
    b"q 1 0 0 1 200 300 cm /Fm0 Do Q\n"
)
fm0_content = (
    b"q 50 0 0 40 0 0 cm /Im0 Do Q\n"
    b"0 0 1 rg 10 60 m 90 60 l 90 90 l h f\n"
    b"BT /F2 10 Tf 0 120 Td (Inside) Tj ET\n"
    b"/Fm1 Do\n"
)
fm1_content = b"q 20 0 0 20 0 0 cm /Im0 Do Q\n"
image_pixels = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255])

objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    # 3: page
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 4 0 R >> /XObject << /Fm0 6 0 R >> >> "
    b"/Contents 5 0 R >>",
    # 4: Helvetica
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    # 5: page content
    stream(b"", page_content),
    # 6: Fm0
    stream(
        b"/Type /XObject /Subtype /Form /BBox [0 0 200 200] "
        b"/Matrix [1 0 0 1 10 10] "
        b"/Resources << /Font << /F2 7 0 R >> "
        b"/XObject << /Im0 8 0 R /Fm1 9 0 R >> >>",
        fm0_content,
    ),
    # 7: Courier, used only inside Fm0
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>",
    # 8: Im0, 2x2 RGB
    stream(
        b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
        b"/ColorSpace /DeviceRGB /BitsPerComponent 8",
        image_pixels,
    ),
    # 9: Fm1, nested inside Fm0
    stream(
        b"/Type /XObject /Subtype /Form /BBox [0 0 50 50] "
        b"/Matrix [1 0 0 1 100 0] /Resources << /XObject << /Im0 8 0 R >> >>",
        fm1_content,
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

Path(__file__).with_name("form-xobject.pdf").write_bytes(bytes(out))
print(f"wrote form-xobject.pdf ({len(out)} bytes)")
