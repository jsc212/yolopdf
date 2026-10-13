"""Black-box checks of CLI output, errors, rendering and source preservation."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

CLI, CORPUS = sys.argv[1], Path(sys.argv[2])
sys.argv = [sys.argv[0]]


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([CLI, *map(str, args)], capture_output=True, timeout=30)

    def test_info(self):
        result = self.run_cli("info", CORPUS / "basic.pdf")
        self.assertEqual(result.returncode, 0, result.stderr)
        info = json.loads(result.stdout)
        self.assertEqual(info["schemaVersion"], 1)
        self.assertEqual(info["pageCount"], 1)
        self.assertEqual(info["pages"][0]["fonts"][0]["name"], "Helvetica")
        self.assertFalse(info["pages"][0]["fonts"][0]["embedded"])
        self.assertEqual(info["pages"][0]["imageResources"][0]["width"], 1)

    def test_render_and_preserve_source(self):
        source = CORPUS / "basic.pdf"
        before = hashlib.sha256(source.read_bytes()).digest()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "render.png"
            result = self.run_cli("render", source, output, "--dpi", 144)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = output.read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack(">II", data[16:24]), (600, 400))
        self.assertEqual(hashlib.sha256(source.read_bytes()).digest(), before)

    def test_invalid_usage(self):
        for args in [[], ["replace-text", CORPUS / "basic.pdf"], ["info"],
                     ["info", CORPUS / "basic.pdf", "extra"]]:
            self.assertEqual(self.run_cli(*args).returncode, 2)

    def test_bad_documents(self):
        for file in ["missing.pdf", "invalid.pdf"]:
            result = self.run_cli("info", CORPUS / file)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"PDF error:", result.stderr)
            self.assertEqual(result.stdout, b"")

    def test_render_rejects_input_as_output(self):
        source = CORPUS / "basic.pdf"
        before = source.read_bytes()
        self.assertEqual(self.run_cli("render", source, source).returncode, 2)
        self.assertEqual(source.read_bytes(), before)

    def test_invalid_page(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "render.png"
            result = self.run_cli("render", CORPUS / "basic.pdf", output, "--page", 2)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(output.exists())

    def test_render_preserves_hardlinked_input(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.pdf"
            source.write_bytes((CORPUS / "basic.pdf").read_bytes())
            before = source.read_bytes()
            output = Path(directory) / "hardlink.png"
            os.link(source, output)
            result = self.run_cli("render", source, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(output.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")


if __name__ == "__main__":
    unittest.main()
