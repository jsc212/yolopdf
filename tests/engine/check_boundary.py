"""Reject MuPDF includes outside its adapter to keep future engine ports possible."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
for directory in ("src", "tools", "tests"):
    for file in (root / directory).rglob("*"):
        if file.suffix not in (".cpp", ".h", ".hpp", ".c"):
            continue
        if file.is_relative_to(root / "src/engine/mupdf"):
            continue
        if re.search(r'^\s*#\s*include\s*[<"]mupdf/', file.read_text(), re.MULTILINE):
            sys.exit(f"MuPDF header outside adapter: {file.relative_to(root)}")
print("Engine boundary verified")
