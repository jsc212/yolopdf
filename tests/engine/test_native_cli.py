import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

cli, corpus = sys.argv[1], Path(sys.argv[2])
with tempfile.TemporaryDirectory() as directory:
    for name, new_text in [("basic.pdf", "Hi"), ("subset-latin.pdf", "éè"), ("subset-cjk.pdf", "日文")]:
        source = corpus / name
        digest = hashlib.sha256(source.read_bytes()).digest()
        output = Path(directory) / name
        for args in [("info", source), ("text-runs", source),
                     ("replace-text", source, output, "--text", new_text)]:
            result = subprocess.run([cli, *map(str, args)], capture_output=True, timeout=30)
            assert result.returncode == 0, result.stderr.decode()
            if args[0] == "info":
                assert json.loads(result.stdout)["pageCount"] == 1
        result = subprocess.run([cli, "text-runs", str(output)], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr.decode()
        assert json.loads(result.stdout)["runs"][0]["text"] == new_text
        assert hashlib.sha256(source.read_bytes()).digest() == digest
print("Native replay driver: 3 original-font replacement cases passed")
