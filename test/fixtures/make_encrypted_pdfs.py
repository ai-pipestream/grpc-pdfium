#!/usr/bin/env python3
"""Regenerates the encryption fixtures from hello.pdf with pikepdf:
encrypted-open.pdf (empty user password, restricted permissions, opens
without credentials) and encrypted-locked.pdf (user password "secret").

Run: uv run --with pikepdf python test/fixtures/make_encrypted_pdfs.py
"""

from pathlib import Path

import pikepdf

here = Path(__file__).parent
src = here / "hello.pdf"

perms = pikepdf.Permissions(extract=False, modify_annotation=False)
with pikepdf.open(src) as pdf:
    pdf.save(
        here / "encrypted-open.pdf",
        encryption=pikepdf.Encryption(user="", owner="owner-pass", R=4, allow=perms),
    )
with pikepdf.open(src) as pdf:
    pdf.save(
        here / "encrypted-locked.pdf",
        encryption=pikepdf.Encryption(user="secret", owner="owner-pass", R=4),
    )
print("wrote encrypted-open.pdf and encrypted-locked.pdf")
