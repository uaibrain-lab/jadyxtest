#!/usr/bin/env python3
"""Stage an embedded GPL PCSX2 runtime INSIDE the Hub's one APPX layout.

Jadyx remains the only installed application. This function does not
compile or launch PCSX2; it checks an already-built DLL and its dependencies.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
from pathlib import Path

REQUIRED = ("uaibrain_ps2_embedded.dll",)
_PE_AMD64 = 0x8664
_ALLOWED_DLL = re.compile(r"[a-zA-Z0-9_.+-]+[.]dll", re.IGNORECASE)
# Known D3D12 redistributable side-by-side DLLs from the native engine build.
_NESTED_DLLS = ("D3D12/D3D12Core.dll", "D3D12/d3d12SDKLayers.dll")


def _validate_x64_dll(path: Path) -> None:
    """Reject wrong-architecture/non-PE dependencies before APPX packaging."""
    if path.is_symlink() or not path.is_file() or path.stat().st_size < 512:
        raise ValueError(f"Invalid engine DLL: {path.name}")
    with path.open("rb") as stream:
        header = stream.read(4096)
    if header[:2] != b"MZ" or len(header) < 64:
        raise ValueError(f"Not a Windows DLL: {path.name}")
    offset = struct.unpack_from("<I", header, 0x3C)[0]
    if offset < 64 or offset + 24 > len(header) or header[offset:offset + 4] != b"PE" + bytes((0, 0)):
        raise ValueError(f"Malformed PE header: {path.name}")
    machine = struct.unpack_from("<H", header, offset + 4)[0]
    if machine != _PE_AMD64:
        raise ValueError(f"Engine DLL must be x64: {path.name} machine=0x{machine:04x}")
    characteristics = struct.unpack_from("<H", header, offset + 22)[0]
    magic = struct.unpack_from("<H", header, offset + 24)[0]
    if not characteristics & 0x2000 or magic != 0x20B:
        raise ValueError(f"Native PS2 dependency is not a 64-bit Windows DLL: {path.name}")


def stage(engine_dir: Path, layout: Path, source_dir: Path) -> list[str]:
    if not engine_dir.is_dir():
        raise FileNotFoundError(f"Engine output directory does not exist: {engine_dir}")
    license_file = source_dir / "COPYING.GPLv3"
    if not license_file.is_file():
        raise FileNotFoundError(f"GPL license missing from PCSX2 source: {license_file}")
    layout.mkdir(parents=True, exist_ok=True)
    for filename in REQUIRED:
        if not (engine_dir / filename).is_file():
            raise FileNotFoundError(f"Embedded PS2 engine not built: {engine_dir / filename}")
    # The MSVC/PCSX2 dependency set changes between builds. Copy ALL DLLs
    # from the controlled build output, not a stale hard-coded allowlist.
    # Never stage an executable, APPX, PFX or another application's manifest.
    resources = engine_dir / "resources"
    if not resources.is_dir() or resources.is_symlink():
        raise FileNotFoundError(f"PCSX2 resources missing/unsafe: {resources}")
    dlls = sorted((p for p in engine_dir.iterdir()
                   if p.name.lower().endswith(".dll")),
                  key=lambda p: p.name.casefold())
    if len(dlls) > 128:
        raise ValueError("Unexpected count of native PS2 DLL dependencies")
    if not any(p.name == REQUIRED[0] for p in dlls):
        raise FileNotFoundError(f"Embedded core DLL missing: {engine_dir / REQUIRED[0]}")
    seen = set()
    for file in dlls:
        if not _ALLOWED_DLL.fullmatch(file.name):
            raise ValueError(f"Unsafe dependency name: {file.name}")
        folded = file.name.casefold()
        if folded in seen:
            raise ValueError(f"Case-colliding native DLLs: {file.name}")
        seen.add(folded)
        _validate_x64_dll(file)
    # Validate the entire replacement before removing any old payload.
    for file in resources.rglob("*"):
        if file.is_symlink():
            raise ValueError(f"Symlink in PCSX2 resources: {file}")
        if file.is_file() and file.suffix.casefold() in (
            ".exe", ".appx", ".appxbundle", ".msix", ".msixbundle",
            ".pfx", ".cer", ".dll", ".iso", ".chd", ".xex", ".bin", ".rom", ".bios", ".key", ".keys"):
            raise ValueError(f"Unexpected executable/certificate in resources: {file}")
    for nested in _NESTED_DLLS:
        src = engine_dir / nested
        if src.exists() or src.is_symlink():
            _validate_x64_dll(src)
    # The Jadyx layout already contains its own runtime. Never remove or
    # overwrite an unrelated DLL/resource tree, even if the names coincide.
    if (layout / "resources").exists() or (layout / "resources").is_symlink():
        raise ValueError("PCSX2 resources collide with existing package resources")
    for src in dlls + [engine_dir / name for name in _NESTED_DLLS if (engine_dir / name).is_file()]:
        dest = layout / src.relative_to(engine_dir)
        if dest.is_symlink() or (dest.exists() and
            (not dest.is_file() or hashlib.sha256(dest.read_bytes()).digest() !=
             hashlib.sha256(src.read_bytes()).digest())):
            raise ValueError(f"Conflicting native dependency: {dest.name}")
    if (layout / "D3D12").is_symlink() or (layout / "Licenses").is_symlink():
        raise ValueError("Unsafe package destination")
    copied = []
    checksums = {}
    for file in dlls:
        if not (layout / file.name).exists():
            shutil.copy2(file, layout / file.name)
        copied.append(file.name)
        checksums[file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
    shutil.copytree(resources, layout / "resources", dirs_exist_ok=True)
    notices = layout / "Licenses"
    notices.mkdir(parents=True, exist_ok=True)
    shutil.copy2(license_file, notices / "PCSX2-GPLv3.txt")
    (notices / "PCSX2-Attribution.txt").write_text(
        "PCSX2 / XBSX2 Copyright (C) PCSX2 Development Team.\\n"
        "Additional Xbox UWP file broker adapted from\\n"
        "https://github.com/skitzellez/XBSX2-GameVault\\n"
        "Copyright (c) 2026 Skitzelles, GPL-3.0-or-later.\\n"
        "The native PCSX2 engine is part of this single Jadyx Universal package.\\n",
        encoding="utf-8"
    )
    for nested in _NESTED_DLLS:
        src = engine_dir / nested
        if src.is_file():
            dest = layout / nested
            dest.parent.mkdir(parents=True, exist_ok=True)
            if not dest.exists():
                shutil.copy2(src, dest)
            checksums[nested] = hashlib.sha256(src.read_bytes()).hexdigest()
            copied.append(nested)
    (layout / "PS2-Native-Dependencies.json").write_text(
        json.dumps(checksums, indent=2, sort_keys=True) + chr(10), encoding="utf-8")
    return copied

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("engine_dir", type=Path)
    parser.add_argument("appx_layout", type=Path)
    parser.add_argument("fork_source", type=Path)
    args = parser.parse_args()
    copies = stage(args.engine_dir, args.appx_layout, args.fork_source)
    print("JADYX_PS2_ONE_APPX_STAGED=" + ",".join(copies))
