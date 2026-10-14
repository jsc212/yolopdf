#!/usr/bin/env python3
"""Fetch source for redistributed Qt runtime modules, verifying vendor SHA-256."""
import argparse
import hashlib
from pathlib import Path
import urllib.request


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--version",default="6.8.3");parser.add_argument("--output",type=Path,required=True);parser.add_argument("--modules",nargs="+",default=["qtbase","qtdeclarative","qtsvg","qtshadertools"]);args=parser.parse_args()
    if not all(p.isdigit() for p in args.version.split('.')) or len(args.version.split('.'))!=3:raise RuntimeError("Invalid Qt version")
    base=f"https://download.qt.io/archive/qt/{'.'.join(args.version.split('.')[:2])}/{args.version}/submodules/"
    args.output.mkdir(parents=True,exist_ok=True)
    for module in args.modules:
        if not module.isalnum() or not module.startswith("qt"):raise RuntimeError("Invalid Qt module")
        name=f"{module}-everywhere-src-{args.version}.tar.xz";path=args.output/name
        with urllib.request.urlopen(base+name+".sha256",timeout=60) as response:checksum=response.read().decode().split()[0]
        if len(checksum)!=64 or any(c not in "0123456789abcdefABCDEF" for c in checksum):raise RuntimeError("Invalid vendor checksum")
        if not path.exists():
            temporary=path.with_suffix(path.suffix+".download")
            with urllib.request.urlopen(base+name,timeout=120) as response,temporary.open("wb") as output:
                while block:=response.read(1024*1024):output.write(block)
            temporary.replace(path)
        digest=hashlib.sha256()
        with path.open("rb") as source:
            while block:=source.read(1024*1024):digest.update(block)
        if digest.hexdigest().lower()!=checksum.lower():raise RuntimeError(f"Vendor checksum mismatch: {name}")
        (args.output/(name+".sha256")).write_text(checksum+"  "+name+"\n")
        print(f"Verified Qt corresponding source: {name}")


if __name__=="__main__":main()
