"""Saved annotations and flattened copies must display and print independently."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from PIL import Image, ImageChops
import pypdfium2 as pdfium

CLI, CORPUS = sys.argv[1], Path(sys.argv[2])
sys.argv = [sys.argv[0]]


def command(*args):
    r = subprocess.run([str(a) for a in args], capture_output=True, timeout=60)
    if r.returncode:
        raise RuntimeError(r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace"))
    return r.stdout


def pdf_objects(path):
    raw = json.loads(command("qpdf", "--json", path))["qpdf"][1]
    return [v.get("value", v.get("stream", {}).get("dict", {})) for v in raw.values()]


def active_annotations(path):
    data = json.loads(command("qpdf", "--json", path))
    table = data["qpdf"][1]
    result = []
    for page in data["pages"]:
        refs = table["obj:" + page["object"]]["value"].get("/Annots", [])
        if isinstance(refs, str):
            refs = table["obj:" + refs]["value"]
        result.extend(table["obj:" + ref]["value"] for ref in refs)
    return result


class MarkingsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("qpdf", "pdftoppm", "pdftotext", "pdftocairo"):
            if not shutil.which(tool):
                raise RuntimeError(f"Required verifier missing: {tool}")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = CORPUS / "basic.pdf"

    def raster(self, path, renderer, hide=False):
        if renderer == "pdfium":
            with pdfium.PdfDocument(path) as doc:
                page = doc[0]
                bitmap = page.render(scale=2, draw_annots=not hide)
                try:
                    return bitmap.to_pil().convert("RGB").copy()
                finally:
                    bitmap.close()
                    page.close()
        prefix = self.root / (path.stem + ("-hidden" if hide else "") + "-poppler")
        args = ["pdftoppm", "-r", "144", "-singlefile", "-png"]
        if hide:
            args.append("-hide-annotations")
        command(*args, path, prefix)
        with Image.open(prefix.with_suffix(".png")) as image:
            return image.convert("RGB")

    def test_saved_markings_flatten_and_print(self):
        original_hash = hashlib.sha256(self.source.read_bytes()).digest()
        runs = json.loads(command(CLI, "text-runs", self.source))["runs"]
        bbox = runs[0]["bounds"]
        # CLI area dimensions are mm, marker points are displayed PDF points.
        x0, y0, x1, y1 = bbox
        bounds = ["--x", x0*25.4/72, "--y", y0*25.4/72,
                  "--width", (x1-x0)*25.4/72, "--height", (y1-y0)*25.4/72]
        current = self.source
        for kind in ("highlight", "strikeout", "marker"):
            output = self.root / f"{kind}.pdf"
            args = bounds if kind != "marker" else ["--points", "30,110;70,105;110,110", "--thickness", "8", "--opacity", ".4"]
            command(CLI, "annotate", current, output, "--type", kind, *args)
            command("qpdf", "--check", output)
            annots = active_annotations(output)
            self.assertEqual(len(annots), ("highlight", "strikeout", "marker").index(kind)+1)
            for annot in annots:
                self.assertTrue(annot["/F"] & 4, "Annotation must have the Print flag")
                self.assertIn("/N", annot["/AP"], "Annotation must save its normal appearance")
            for renderer in ("poppler", "pdfium"):
                before = self.raster(current, renderer)
                after = self.raster(output, renderer)
                self.assertIsNotNone(ImageChops.difference(before, after).getbbox(), f"{kind} invisible in {renderer}")
            current = output
        flat = self.root / "flattened.pdf"
        command(CLI, "flatten-annotations", current, flat)
        command("qpdf", "--check", flat)
        self.assertFalse(active_annotations(flat))
        self.assertIn("Hello yolopdf", command("pdftotext", flat, "-").decode())
        for renderer in ("poppler", "pdfium"):
            marked, flattened = self.raster(current, renderer), self.raster(flat, renderer)
            diff = ImageChops.difference(marked, flattened)
            self.assertLessEqual(max(high for low, high in diff.getextrema()), 3, f"Flatten changed appearance in {renderer}")
            self.assertIsNone(ImageChops.difference(flattened, self.raster(flat, renderer, hide=True)).getbbox())
        # Poppler's PDF print conversion observes annotation Print flags.
        printed = self.root / "printed.pdf"
        command("pdftocairo", "-pdf", current, printed)
        command("qpdf", "--check", printed)
        plain_printed = self.root / "plain-printed.pdf"
        command("pdftocairo", "-pdf", self.source, plain_printed)
        self.assertIsNotNone(ImageChops.difference(self.raster(printed,"poppler"), self.raster(plain_printed,"poppler")).getbbox())
        # The marker is away from text: inspect its printed colour at 70,105 pt.
        for renderer in ("poppler", "pdfium"):
            r,g,b = self.raster(printed, renderer).getpixel((140,210))
            self.assertGreater(r-b,30)
            self.assertGreater(g-b,30)
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).digest(), original_hash)

    def test_invalid_stroke_does_not_export(self):
        output = self.root / "invalid.pdf"
        result = subprocess.run([CLI,"annotate",str(self.source),str(output),"--type","marker","--points","1,2"],capture_output=True)
        self.assertNotEqual(result.returncode,0)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
