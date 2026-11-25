#!/usr/bin/env python3
"""Stage the experimental x64 Xenia engine in the existing Jadyx APPX only."""
import argparse
import hashlib
import json
import re
import runpy
import shutil
import subprocess
from pathlib import Path

validate = runpy.run_path(str(Path(__file__).with_name("stage-embedded-ps2.py")))["_validate_x64_dll"]
CORE = "uaibrain_xenia_embedded.dll"
MARKER = "Xenia-Native-Dependencies.json"

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()

def stage(engine, layout, source):
    if not engine.is_dir() or engine.is_symlink():
        raise ValueError("Invalid Xenia build directory")
    license_file = source / "LICENSE"
    if not license_file.is_file() or license_file.is_symlink():
        raise ValueError("Missing Xenia license")
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("Invalid Xenia source revision")
    if subprocess.check_output(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"], text=True).strip():
        raise ValueError("Xenia source must have no tracked modifications")
    files = sorted(p for p in engine.iterdir() if p.suffix.lower() == ".dll")
    if not any(p.name == CORE for p in files) or len(files) > 128:
        raise ValueError("Missing embedded Xenia DLL or unexpected dependency count")
    if layout.is_symlink() or (layout / "Licenses").is_symlink() or (layout / MARKER).exists():
        raise ValueError("Unsafe or already staged layout")
    seen = set()
    checksums = {}
    for p in files:
        if not re.fullmatch(r"[a-zA-Z0-9_.+-]+[.]dll", p.name, re.I) or p.name.casefold() in seen:
            raise ValueError("Unsafe or colliding dependency name")
        seen.add(p.name.casefold())
        validate(p)
        dest = layout / p.name
        digest = sha(p)
        if dest.is_symlink() or (dest.exists() and (not dest.is_file() or sha(dest) != digest)):
            raise ValueError("Conflicting dependency: " + p.name)
        checksums[p.name] = digest
    notices = layout / "Licenses"
    license_dest = notices / "Xenia-BSD.txt"
    if license_dest.is_symlink() or license_dest.exists():
        raise ValueError("Xenia license already present")
    layout.mkdir(parents=True, exist_ok=True)
    notices.mkdir(exist_ok=True)
    for p in files:
        if not (layout / p.name).exists():
            shutil.copy2(p, layout / p.name)
    shutil.copy2(license_file, license_dest)
    (layout / MARKER).write_text(json.dumps({"source_commit": revision, "source": "https://github.com/uaibrain-lab/xenia-canarytest", "dlls": checksums}, indent=2) + "\n", encoding="utf-8")
    return checksums

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path)
    parser.add_argument("layout", type=Path)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    print("JADYX_XENIA_ONE_APPX_STAGED=" + ",".join(stage(args.engine, args.layout, args.source)))
