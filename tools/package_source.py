#!/usr/bin/env python3
"""Make a source bundle with exact recursive dependencies; never include caches."""
import argparse
import json
from pathlib import Path
import subprocess
import tarfile
import io

ROOT = Path(__file__).resolve().parents[1]


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args])


def repositories(path, relative=Path()):
    yield path, relative
    if not (path / ".gitmodules").exists():
        return
    entries=git(path,"config","--file",".gitmodules","--get-regexp",r"^submodule\..*\.path$").decode().splitlines()
    for entry in entries:
        child=entry.split(None,1)[1]
        dep=path/child
        status=git(path,"submodule","status","--",child).decode()
        if not status or status[0] in "-+U":
            raise RuntimeError(f"Submodule missing or not pinned: {relative/child}")
        yield from repositories(dep,relative/child)


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--output",type=Path,required=True);parser.add_argument("--notices",type=Path,required=True);parser.add_argument("--allow-dirty-root",action="store_true",help="Developer preview only; bundle includes current tracked root contents")
    args=parser.parse_args();repos=list(repositories(ROOT));manifest={};names=[]
    for path,relative in repos:
        if git(path,"status","--porcelain","--untracked-files=no").strip() and (relative!=Path() or not args.allow_dirty_root):
            raise RuntimeError(f"Commit source changes before packaging: {relative}")
        manifest[str(relative) or "."]=git(path,"rev-parse","HEAD").decode().strip()
        for name in git(path,"ls-files","-z").split(b"\0"):
            if not name:continue
            rel=Path(name.decode());file=path/rel
            if file.is_file():names.append((file,relative/rel))
    args.output.parent.mkdir(parents=True,exist_ok=True);args.notices.mkdir(parents=True,exist_ok=True)
    with tarfile.open(args.output,"w:gz") as archive:
        for file,relative in names:
            archive.add(file,arcname=str(Path("yolopdf")/relative),recursive=False)
            basename=relative.name.upper()
            if any(word in basename for word in ("LICENSE","COPYING","NOTICE")) and file.stat().st_size<2*1024*1024:
                notice=args.notices/relative;notice.parent.mkdir(parents=True,exist_ok=True);notice.write_bytes(file.read_bytes())
        data=json.dumps({"commits":manifest,"root_dirty_preview":args.allow_dirty_root},indent=2).encode()
        info=tarfile.TarInfo("yolopdf/SOURCE_COMMITS.json");info.size=len(data);archive.addfile(info,io.BytesIO(data))
    print(f"Bundled {len(names)} tracked files from {len(repos)} repositories: {args.output}")


if __name__=="__main__":main()
