#!/usr/bin/env python3
"""Evidence for imports the game has not reached yet (Persona3DS fork tool).

Writes one text file with:
  1. every call site of the selected imports (default: the imports that still
     have no HLE handler), with the instructions before and after each call,
     from build/recomp/PSPRecomp/dump_function;
  2. the UMD image's file list (path, first LBN, size), so `disc0:/sce_lbn...`
     opens in run logs can be matched to file names.

usage: python tools/p1_callsites.py <eboot.elf> <imports.csv> <iso> <out.txt> [NID ...]
  imports.csv: psp_analyze's *_imports.csv (library,nid,name,stub_address).
"""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOAD_BASE = 0x08804000  # profiles/p1p/config/aot_layout.cmake
DEFAULT_NIDS = [
    "F2930C9C", "4A9E5E29", "6AF9B50A", "4D4E10EC", "779103A0", "05572A5F", "2E0911AA", "D1FF982A",
    "F0A26395", "8F2DF740", "2AD8E239", "67AF3428", "95FC253B", "9A1C91D7", "2DD3E298", "5D268707",
    "61EB33F5", "644E5607", "6A8C3CD5", "7A20E7AF", "7DB31251", "83BF7AFD", "83E85EA0", "868120B5",
    "9AE849A7", "A2BBA8BE", "E23E3A35", "E88F759B",
]
BEFORE, AFTER = 24, 6


def dump_function_path() -> Path:
    for name in ("dump_function.exe", "dump_function"):
        p = ROOT / "build" / "recomp" / "PSPRecomp" / name
        if p.exists():
            return p
    sys.exit("dump_function not found: build the host tree first (cmake --build build)")


def text_words(elf: Path) -> int:
    data = elf.read_bytes()
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    for i in range(phnum):
        p_type, _, vaddr, _, filesz, _, flags, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1 and vaddr == 0 and flags & 1:  # first executable LOAD segment
            return filesz // 4
    sys.exit("no executable LOAD segment at vaddr 0")


def call_sites(elf: Path, csv: Path, nids: list[str]) -> list[str]:
    stubs: dict[str, str] = {}
    for line in csv.read_text().splitlines()[1:]:
        lib, nid, name, stub = line.split(",")[:4]
        nid = nid.upper().removeprefix("0X").zfill(8)
        if nid in nids:
            stubs[stub.lower()] = f"{lib}::{name or nid} (NID 0x{nid}, stub {stub})"
    words = text_words(elf)
    dump = subprocess.run([str(dump_function_path()), str(elf), hex(LOAD_BASE), hex(words)],
                          check=True, capture_output=True, text=True).stdout.splitlines()
    out = [f"== call sites ({len(stubs)} of {len(nids)} requested NIDs imported) =="]
    found: dict[str, int] = {s: 0 for s in stubs}
    for i, line in enumerate(dump):
        target = line.rsplit("target=", 1)[1].lower() if "target=" in line else None
        if target not in stubs or " jal " not in line:
            continue
        found[target] += 1
        out.append(f"\n-- {stubs[target]} called at {line.split()[0]}")
        out.extend(dump[max(0, i - BEFORE):i + AFTER + 1])
    out.append("\n== summary ==")
    out.extend(f"{n:4d} call sites  {stubs[s]}" for s, n in sorted(found.items(), key=lambda kv: stubs[kv[0]]))
    return out


def iso_listing(iso: Path) -> list[str]:
    out = ["", "== UMD image files (path, first LBN, size) =="]
    with iso.open("rb") as f:
        def sector(lbn: int, count: int = 1) -> bytes:
            f.seek(lbn * 2048)
            return f.read(count * 2048)

        pvd = sector(16)
        if pvd[1:6] != b"CD001":
            return out + ["not an ISO 9660 image"]

        def walk(lbn: int, size: int, path: str) -> None:
            data = sector(lbn, (size + 2047) // 2048)
            pos = 0
            while pos < size:
                length = data[pos]
                if length == 0:  # records do not cross sectors
                    pos = (pos // 2048 + 1) * 2048
                    continue
                rec = data[pos:pos + length]
                extent, = struct.unpack_from("<I", rec, 2)
                dsize, = struct.unpack_from("<I", rec, 10)
                flags, nlen = rec[25], rec[32]
                name = rec[33:33 + nlen]
                pos += length
                if name in (b"\x00", b"\x01"):
                    continue
                text = name.decode("ascii", "replace").split(";")[0]
                if flags & 2:
                    walk(extent, dsize, f"{path}{text}/")
                else:
                    out.append(f"{path}{text}  lbn 0x{extent:x}  size 0x{dsize:x} ({dsize})")

        root = pvd[156:156 + 34]
        walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "/")
    return out


def main() -> int:
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    elf, csv, iso, out = (Path(a) for a in sys.argv[1:5])
    nids = [n.upper().removeprefix("0X").zfill(8) for n in sys.argv[5:]] or DEFAULT_NIDS
    lines = call_sites(elf, csv, nids) + iso_listing(iso)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    print(f"wrote {out} ({len(lines)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
