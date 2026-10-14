"""Stage 1 exports checked independently by qpdf, Poppler and PDFium."""
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, ImageChops, ImageDraw
import pypdfium2 as pdfium

CLI, CORPUS = sys.argv[1], Path(sys.argv[2])
sys.argv = [sys.argv[0]]


def run(*args):
    return subprocess.run(list(map(str, args)), capture_output=True, timeout=60)


def objects(path):
    result = run("qpdf", "--json", path)
    if result.returncode:
        raise RuntimeError(result.stderr.decode())
    table = json.loads(result.stdout)["qpdf"][1]
    return {key.removeprefix("obj:"): value.get("value", value.get("stream", {}).get("dict", {})) for key, value in table.items()}


class Stage1Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for command in ("qpdf", "pdftotext", "pdftoppm"):
            if not shutil.which(command):
                raise RuntimeError(f"Required independent verifier missing: {command}")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def cli(self, *args):
        result = run(CLI, *args)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return result.stdout

    def valid(self, path):
        result = run("qpdf", "--check", path)
        self.assertEqual(result.returncode, 0, result.stdout.decode() + result.stderr.decode())

    def render(self, path, renderer, page=0):
        if renderer == "PDFium":
            with pdfium.PdfDocument(path) as document:
                pdf_page = document[page]
                try:
                    bitmap = pdf_page.render(scale=2, draw_annots=True)
                    try:
                        return bitmap.to_pil().convert("RGB")
                    finally:
                        bitmap.close()
                finally:
                    pdf_page.close()
        target = self.root / ("poppler-" + str(page))
        result = run("pdftoppm", "-cropbox", "-f", page + 1, "-l", page + 1, "-singlefile", "-r", "144", "-png", path, target)
        self.assertEqual(result.returncode, 0, result.stderr)
        with Image.open(str(target) + ".png") as image:
            return image.convert("RGB")

    def physical_size(self, path, page=0):
        info = json.loads(self.cli("info", path))["pages"][page]
        box = info["cropBox"]
        size = ((box[2] - box[0]) * info["userUnit"], (box[3] - box[1]) * info["userUnit"])
        return size[::-1] if info["rotation"] % 180 else size

    def same_outside(self, before, after, bounds, physical_size):
        self.assertEqual(before.size, after.size)
        diff = ImageChops.difference(before, after)
        # Use each renderer's actual pixels per physical point. Some independent
        # renderers ignore UserUnit in their bitmap dimensions; do not conceal
        # that limitation by resizing their images.
        sx, sy = before.width / physical_size[0], before.height / physical_size[1]
        pixels = [math.floor(bounds[0] * sx) - 2, math.floor(bounds[1] * sy) - 2,
                  math.ceil(bounds[2] * sx) + 2, math.ceil(bounds[3] * sy) + 2]
        ImageDraw.Draw(diff).rectangle(pixels, fill=(0, 0, 0))
        self.assertIsNone(diff.getbbox(), "Pixels changed outside the edited bounds")

    def test_text_replacement_in_all_verifiers(self):
        for name, replacement in (("basic.pdf", "Hi yolopdf"), ("subset-latin.pdf", "éè"),
                                  ("subset-cjk.pdf", "日文"), ("subset-cff.pdf", "éè"), ("narrow-encoding.pdf", "Ω"),
                                  ("graphics-state-font.pdf", "Hi yolopdf"), ("rotated-text.pdf", "Hi yolopdf"),
                                  ("positioned-text.pdf", "Hi yolopdf"),
                                  ("marked.pdf", "Hi"), ("marked-crossing-graphics.pdf", "Hi"),
                                  ("nested.pdf", "Text")):
            with self.subTest(fixture=name):
                source = CORPUS / name
                original_hash = hashlib.sha256(source.read_bytes()).digest()
                before_run = json.loads(self.cli("text-runs", source))["runs"][0]
                output = self.root / (name + "-edited.pdf")
                self.cli("replace-text", source, output, "--text", replacement)
                self.valid(output)
                extracted = run("pdftotext", output, "-")
                self.assertEqual(extracted.returncode, 0, extracted.stderr)
                self.assertIn(replacement, extracted.stdout.decode())
                after_run = json.loads(self.cli("text-runs", output))["runs"][0]
                self.assertEqual(after_run["font"], before_run["font"])
                with pdfium.PdfDocument(output) as document:
                    page = document[0]
                    try:
                        text = page.get_textpage()
                        try:
                            self.assertIn(replacement, text.get_text_range())
                        finally:
                            text.close()
                    finally:
                        page.close()
                bounds = [min(before_run["bounds"][i], after_run["bounds"][i]) if i < 2 else
                          max(before_run["bounds"][i], after_run["bounds"][i]) for i in range(4)]
                for renderer in ("Poppler", "PDFium"):
                    with self.subTest(renderer=renderer):
                        self.same_outside(self.render(source, renderer), self.render(output, renderer), bounds, self.physical_size(source))
                        if name == "nested.pdf":
                            self.assertIsNone(ImageChops.difference(self.render(source, renderer, 1), self.render(output, renderer, 1)).getbbox())
                self.assertEqual(hashlib.sha256(source.read_bytes()).digest(), original_hash)

    def test_explicit_import_and_missing_glyphs(self):
        source = CORPUS / "subset-latin.pdf"
        output = self.root / "import.pdf"
        result = run(CLI, "replace-text", source, output, "--text", "Ω")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot encode", result.stderr.decode())
        self.assertFalse(output.exists())
        self.cli("replace-text", source, output, "--text", "Ω", "--font", CORPUS / "YoloTest-Regular.ttf")
        self.valid(output)
        self.assertIn("Ω", run("pdftotext", output, "-").stdout.decode())
        for fixture, font, index in (("subset-latin.pdf", "YoloTest-Regular.ttf", 0),
                                     ("graphics-state-font.pdf", "YoloTest-Regular.ttf", 0),
                                     ("subset-latin.pdf", "two-faces.ttc", 1)):
            with self.subTest(fixture=fixture, font=font):
                source = CORPUS / fixture
                before = json.loads(self.cli("text-runs", source))["runs"][0]
                self.cli("replace-text", source, output, "--text", "Ω", "--font", CORPUS / font,
                         "--font-index", index)
                self.valid(output)
                self.assertIn("Ω", run("pdftotext", output, "-").stdout.decode())
                after = json.loads(self.cli("text-runs", output))["runs"][0]
                bounds = [min(before["bounds"][i], after["bounds"][i]) if i < 2 else
                          max(before["bounds"][i], after["bounds"][i]) for i in range(4)]
                for renderer in ("Poppler", "PDFium"):
                    self.same_outside(self.render(source, renderer), self.render(output, renderer),
                                      bounds, self.physical_size(source))

    def test_image_occurrences_survive_export(self):
        for name in ("shared-image.pdf", "nested-image.pdf", "inline-image.pdf"):
            with self.subTest(fixture=name):
                source = CORPUS / name
                image = json.loads(self.cli("images", source))[0]
                output = self.root / (name + "-deleted.pdf")
                self.cli("delete-image", source, output)
                self.valid(output)
                for renderer in ("Poppler", "PDFium"):
                    self.same_outside(self.render(source, renderer), self.render(output, renderer), image["bounds"], self.physical_size(source))
                    if name == "nested-image.pdf":
                        self.assertIsNone(ImageChops.difference(self.render(source, renderer, 1), self.render(output, renderer, 1)).getbbox())
        source = CORPUS / "shared-image.pdf"
        output = self.root / "replacement.pdf"
        self.cli("replace-image", source, output, CORPUS / "blue.png", "--occurrence", 1)
        self.valid(output)
        occurrences = json.loads(self.cli("images", output))
        self.assertEqual([image["width"] for image in occurrences], [1, 2, 1])

    def test_physical_stamp_size_and_flatten(self):
        for page in (1, 2):
            for artwork in ("red-alpha.png", "vector.svg", "frames.gif"):
                with self.subTest(page=page, artwork=artwork):
                    source = CORPUS / "nested.pdf"
                    output = self.root / "stamp.pdf"
                    self.cli("place-stamp", source, output, CORPUS / artwork, "--page", page,
                             "--x", 10, "--y", 10, "--width", 40, "--height", 20)
                    self.valid(output)
                    table = objects(output)
                    annotation = next(value for value in table.values() if isinstance(value, dict) and value.get("/Subtype") == "/Stamp")
                    width = annotation["/Rect"][2] - annotation["/Rect"][0]
                    height = annotation["/Rect"][3] - annotation["/Rect"][1]
                    unit = 2 if page == 1 else 1
                    # Both fixture pages have 90/270 rotation, so visible axes swap.
                    self.assertAlmostEqual(height * unit, 40 * 72 / 25.4, places=3)
                    self.assertAlmostEqual(width * unit, 20 * 72 / 25.4, places=3)
                    if artwork == "vector.svg":
                        self.assertFalse(any(isinstance(value, dict) and value.get("/Subtype") == "/Image" for value in table.values()))
                    flat = self.root / "flattened.pdf"
                    self.cli("flatten-stamps", output, flat)
                    self.valid(flat)
                    self.assertFalse(any(isinstance(value, dict) and value.get("/Subtype") == "/Stamp" for value in objects(flat).values()))
                    for renderer in ("Poppler", "PDFium"):
                        before = self.render(output, renderer, page - 1)
                        after = self.render(flat, renderer, page - 1)
                        diff = ImageChops.difference(before, after)
                        self.assertLessEqual(max(channel[1] for channel in diff.getextrema()), 2)

    def test_page_insert_extract_and_source_preservation(self):
        source = CORPUS / "basic.pdf"
        other = CORPUS / "nested.pdf"
        hashes = [hashlib.sha256(path.read_bytes()).digest() for path in (source, other)]
        output = self.root / "inserted.pdf"
        self.cli("insert-pages", source, output, other, source, "--at", 2)
        self.valid(output)
        info = json.loads(self.cli("info", output))
        self.assertEqual(info["pageCount"], 4)
        self.assertEqual(info["pages"][1]["userUnit"], 2)
        for renderer in ("Poppler", "PDFium"):
            self.assertIsNone(ImageChops.difference(self.render(other, renderer), self.render(output, renderer, 1)).getbbox())
        extracted = self.root / "extracted.pdf"
        self.cli("extract-pages", output, extracted, "--pages", "3,1")
        self.valid(extracted)
        self.assertEqual(json.loads(self.cli("info", extracted))["pageCount"], 2)
        self.assertEqual([hashlib.sha256(path.read_bytes()).digest() for path in (source, other)], hashes)

    def test_form_field_collision_and_partial_widget_graft(self):
        output = self.root / "forms.pdf"
        self.cli("insert-pages", CORPUS / "form.pdf", output, CORPUS / "multi-widget-form.pdf", "--pages", 2, "--at", 2)
        self.valid(output)
        table = objects(output)
        fields = [value for value in table.values() if isinstance(value, dict) and "/T" in value]
        self.assertEqual({field["/T"] for field in fields}, {"u:Name", "u:Name_2"})
        imported = next(field for field in fields if field["/T"] == "u:Name_2")
        self.assertEqual(len(imported["/Kids"]), 1)
        widget = table[imported["/Kids"][0]]
        self.assertEqual(table[widget["/P"]]["/Type"], "/Page")
        self.assertIn("YoloMerge", imported["/DA"])

    def test_internal_link_destination_remapping(self):
        output = self.root / "linked.pdf"
        self.cli("insert-pages", CORPUS / "basic.pdf", output, CORPUS / "linked-pages.pdf", "--at", 2)
        self.valid(output)
        table = objects(output)
        link = next(value for value in table.values() if isinstance(value, dict) and value.get("/Subtype") == "/Link")
        self.assertEqual(table[link["/Dest"][0]]["/Type"], "/Page")
        result = run(CLI, "insert-pages", CORPUS / "basic.pdf", self.root / "bad-link.pdf", CORPUS / "linked-pages.pdf", "--pages", 1)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "bad-link.pdf").exists())


if __name__ == "__main__":
    unittest.main()
