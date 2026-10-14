"""Kill real save/recovery processes; the original and recovery must survive."""
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

DRIVER, CORPUS = sys.argv[1], Path(sys.argv[2])
with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    target = root / "original.pdf"
    target.write_bytes((CORPUS / "basic.pdf").read_bytes())
    original = hashlib.sha256(target.read_bytes()).digest()
    source = root / "large-cache.pdf"
    with source.open("wb") as f:
        f.write(target.read_bytes())
        f.truncate(256*1024*1024)
    process = subprocess.Popen([DRIVER,"save",str(source),str(target)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    interrupted = False
    deadline = time.monotonic()+15
    try:
        while process.poll() is None and time.monotonic()<deadline:
            for fd in (Path("/proc") / str(process.pid) / "fd").glob("*"):
                try:
                    name = os.readlink(fd)
                    size = fd.stat().st_size
                    if name.startswith(str(root)+"/") and name not in (str(source),str(target)) and 0 < size < source.stat().st_size:
                        os.kill(process.pid,signal.SIGSTOP)
                        # Recheck while stopped: the write must really be incomplete.
                        if 0 < fd.stat().st_size < source.stat().st_size:
                            interrupted=True
                            process.kill()
                            break
                        os.kill(process.pid,signal.SIGCONT)
                except (FileNotFoundError,ProcessLookupError):
                    pass
            if interrupted:
                break
            time.sleep(.001)
        if not interrupted:
            raise AssertionError("Could not observe the real saver mid-write")
        process.communicate(timeout=10)
        assert hashlib.sha256(target.read_bytes()).digest()==original, "Interrupted save changed original"
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=10)
    recovery = root / "recovery"
    edit = subprocess.Popen([DRIVER,"edit",str(CORPUS/"subset-latin.pdf"),str(recovery)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        # Wait for atomic metadata publication without depending on shutdown.
        deadline=time.monotonic()+15
        while not list(recovery.glob("*/session.json")):
            if edit.poll() is not None or time.monotonic()>deadline:
                raise AssertionError("Recovery generation was not published")
            time.sleep(.01)
        edit.kill()
        edit.communicate(timeout=10)
        result=subprocess.run([DRIVER,"restore","unused",str(recovery)],capture_output=True,timeout=15)
        assert result.returncode==0,result.stderr.decode()
    finally:
        if edit.poll() is None:
            edit.kill()
        edit.communicate(timeout=10)
print("Killed a save mid-write: original unchanged; killed editor: recovery and undo restored")
