#!/usr/bin/env python3
"""Regenerates page-tree.pdf, the page-geometry fixture: four Helvetica
pages whose boxes, rotation and resources come through the page tree
(ISO 32000-1 7.7.3.4) rather than from each page's own dictionary.

  page 0  inherits /MediaBox [0 0 600 800] from the root /Pages node;
          "inherited" at (100, 700)
  page 1  inherits that MediaBox and sets its own /CropBox
          [50 100 550 700]; "cropped" at (100, 600), a filled rectangle
          [150 200 190 230] and a link /Rect [200 300 260 320] to page 2
          at (110, 510)
  page 2  sits under an intermediate /Pages node carrying /CropBox
          [10 20 590 780] and /Rotate 90; "rotated" at (100, 500) and an
          image placed on [200 300 240 330]
  page 3  under the same node, with its own /MediaBox [0 0 300 400]
          (the inherited CropBox is clipped to it) and its own /Rotate 0;
          "own" at (50, 300)

The outline has one item, also to page 2 at (110, 510). Every position
above is in user space; the backend reports them relative to each page's
CropBox origin.
"""

from pathlib import Path


def stream(data: bytes) -> bytes:
    return (
        b"<< /Length " + str(len(data)).encode() + b" >>\nstream\n"
        + data + b"\nendstream"
    )


def show(text: bytes, x: int, y: int) -> bytes:
    return b"BT /F1 24 Tf " + f"{x} {y}".encode() + b" Td (" + text + b") Tj ET"


objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R /Outlines 15 0 R >>",
    # 2: root page tree node: the MediaBox and resources every page inherits
    b"<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 4 "
    b"/MediaBox [0 0 600 800] /Resources << /Font << /F1 8 0 R >> "
    b"/XObject << /Im1 14 0 R >> >> >>",
    # 3: page 0, no boxes of its own
    b"<< /Type /Page /Parent 2 0 R /Contents 9 0 R >>",
    # 4: page 1, its own CropBox
    b"<< /Type /Page /Parent 2 0 R /CropBox [50 100 550 700] /Contents 10 0 R "
    b"/Annots [13 0 R] >>",
    # 5: intermediate node: CropBox and Rotate for its kids
    b"<< /Type /Pages /Parent 2 0 R /Kids [6 0 R 7 0 R] /Count 2 "
    b"/CropBox [10 20 590 780] /Rotate 90 >>",
    # 6: page 2, everything inherited
    b"<< /Type /Page /Parent 5 0 R /Contents 11 0 R >>",
    # 7: page 3, its own MediaBox and Rotate
    b"<< /Type /Page /Parent 5 0 R /MediaBox [0 0 300 400] /Rotate 0 "
    b"/Contents 12 0 R >>",
    # 8: Helvetica
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    # 9-12: page contents
    stream(show(b"inherited", 100, 700)),
    stream(show(b"cropped", 100, 600) + b"\n150 200 40 30 re f"),
    stream(show(b"rotated", 100, 500) + b"\nq 40 0 0 30 200 300 cm /Im1 Do Q"),
    stream(show(b"own", 50, 300)),
    # 13: page 1's link to page 2
    b"<< /Type /Annot /Subtype /Link /Rect [200 300 260 320] /Border [0 0 0] "
    b"/Dest [6 0 R /XYZ 110 510 0] >>",
    # 14: a 2x2 gray image
    b"<< /Type /XObject /Subtype /Image /Width 2 /Height 2 "
    b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Length 4 >>\nstream\n"
    b"\x00\xff\xff\x00\nendstream",
    # 15-16: the outline
    b"<< /Type /Outlines /First 16 0 R /Last 16 0 R /Count 1 >>",
    b"<< /Title (rotated) /Parent 15 0 R /Dest [6 0 R /XYZ 110 510 0] >>",
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

Path(__file__).with_name("page-tree.pdf").write_bytes(bytes(out))
print(f"wrote page-tree.pdf ({len(out)} bytes)")
