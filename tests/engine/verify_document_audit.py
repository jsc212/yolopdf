#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Verify opt-in document-audit outputs without printing private document text."""
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageDraw
import pypdfium2 as pdfium

source, folder, cli = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
original = hashlib.sha256(source.read_bytes()).digest()


def run(*args):
    result = subprocess.run([str(a) for a in args], capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(f"{Path(str(args[0])).name} failed for an audit output")
    return result.stdout


def raster(path, viewer, page=0):
    if viewer == "PDFium":
        with pdfium.PdfDocument(path) as document:
            p = document[page]
            bitmap = p.render(scale=1, draw_annots=True)
            try:
                return bitmap.to_pil().convert("RGB").copy()
            finally:
                bitmap.close()
                p.close()
    with tempfile.TemporaryDirectory() as temporary:
        output = Path(temporary) / "page"
        run("pdftoppm", "-f", page + 1, "-l", page + 1, "-singlefile", "-r", 72, "-png", path, output)
        return Image.open(output.with_suffix(".png")).convert("RGB").copy()


outputs = sorted(folder.glob("*.pdf"))
for output in outputs:
    run("qpdf", "--check", output)
    with pdfium.PdfDocument(output) as document:
        for index in range(len(document)):
            p = document[index]
            bitmap = p.render(scale=.5, draw_annots=True)
            assert bitmap.width and bitmap.height, "Empty independent render"
            bitmap.close()
            p.close()
    run("pdftotext", output, "-")

lines = json.loads(run(cli, "text-lines", source))
line = next(l for l in lines if l["status"] == "original-font" and len(l["text"]) > 4 and l["bounds"][2] - l["bounds"][0] > 30)
edited = folder / "edit-text-preview-undo-redo.pdf"
changed = json.loads(run(cli, "text-lines", edited))
after = next(l for l in changed if l["id"] == line["id"])
assert after["text"] == line["text"][:-1], "Saved text differs from the applied edit"
assert after["text"] in run("pdftotext", edited, "-").decode("utf-8"), "Poppler cannot extract the edit"
with pdfium.PdfDocument(edited) as document:
    p = document[0]
    text = p.get_textpage()
    assert after["text"] in text.get_text_range(), "PDFium cannot extract the edit"
    text.close()
    p.close()

with pdfium.PdfDocument(source) as document:
    page_count = len(document)
for viewer in ("Poppler", "PDFium"):
    for page in range(page_count):
        before_image, after_image = raster(source, viewer, page), raster(edited, viewer, page)
        difference = ImageChops.difference(before_image, after_image)
        if page == 0:
            a, b = line["bounds"], after["bounds"]
            region = [math.floor(min(a[0], b[0])) - 2, math.floor(min(a[1], b[1])) - 2,
                      math.ceil(max(a[2], b[2])) + 2, math.ceil(max(a[3], b[3])) + 2]
            ImageDraw.Draw(difference).rectangle(region, fill=(0, 0, 0))
        assert not difference.getbbox(), f"{viewer}: unrelated page content changed"
    marked = raster(folder / "flattened-copy-print.pdf", viewer)
    flattened = raster(folder / "flattened.pdf", viewer)
    assert max(channel[1] for channel in ImageChops.difference(marked, flattened).getextrema()) <= 3, f"{viewer}: flattening changed the appearance"
    printed = raster(folder / "print.pdf", viewer)
    roi = (35, 32, 105, 58)
    yellow = sum(1 for r, g, b in printed.crop(roi).get_flattened_data() if r > 180 and g > 140 and b < 210 and r - b > 30)
    assert yellow > 30, f"{viewer}: print copy lost the marker"

assert hashlib.sha256(source.read_bytes()).digest() == original, "Original source changed"
(folder / "independent-verification.json").write_text(json.dumps({
    "pdfOutputsChecked": len(outputs), "qpdf": "pass", "Poppler": "pass", "PDFium": "pass",
    "unrelatedContentUnchanged": True, "markingsVisibleInPrint": True,
    "flattenedAppearancePreserved": True, "originalUnchanged": True,
}, indent=2))
print(f"Verified {len(outputs)} PDF outputs: qpdf, Poppler, PDFium; source unchanged")
