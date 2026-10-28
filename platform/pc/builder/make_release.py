#!/usr/bin/env python3
"""Assemble the P3P3DS Builder release package for the host platform.

Windows: out/P3P3DS-Builder-<version>/ + .zip. Linux: out/P3P3DS-Builder-<version>-linux/
+ .tar.gz (keeps the executable bits).

Prerequisites (maintainer machine): the host build in build/ (P3P3DS-Builder,
psp_recomp) and the New 3DS build in build/3ds/ (runtime objects). Nothing
derived from the game goes into the package: sdk/ holds only the prebuilt
P3P3DS runtime for the 3DS, the code generator, its headers and the CWCheat
patch list, which are all project code or community data.

P3P3DS (Persona3DS): the package also carries the tools for the music/movie conversion and the
optional .cia, so users install nothing but devkitPro:
  sdk/bin/ffmpeg, ffprobe  a self-contained (static) ffmpeg build with libx264, from --ffmpeg
  sdk/bin/makerom, bannertool  from --cia-tools (default: the repository's banner/ folder)
  sdk/cia/  the .cia settings template and banner (banner/ art when present, else the default
            platform/pc/builder/cia/banner.png and a silent audio.wav)
THIRD_PARTY_NOTICES.txt lists their licenses and where their source code is.

Usage: python platform/pc/builder/make_release.py --ffmpeg <folder with ffmpeg and ffprobe> [--cia-tools <folder>] [--version 0.1.0]
"""
import argparse
import shutil
import subprocess
import sys
import tarfile
import wave
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "build"
BUILD3DS = BUILD / "3ds"
BANNER = ROOT / "banner"  # the maintainer's own banner art and tools (not in git)
CIA_DEFAULTS = Path(__file__).resolve().parent / "cia"
WINDOWS = sys.platform == "win32"
EXE = ".exe" if WINDOWS else ""

RUNTIME_OBJECTS = {
    "main.o": BUILD3DS / "CMakeFiles/p3p3ds_objects.dir/main.o",
    "gpu_renderer.o": BUILD3DS / "CMakeFiles/p3p3ds_objects.dir/gpu_renderer.o",
    "ge_shbin.o": BUILD3DS / "CMakeFiles/p3p_ge_shader_bin.dir/.dkp-generated/p3p_ge_shader_bin/ge_shbin.o",
    "libp3p3ds_core.a": BUILD3DS / "libp3p3ds_core.a",
    "libpsprecomp_runtime.a": BUILD3DS / "libpsprecomp_runtime.a",
}

README_WINDOWS = """P3P3DS Builder {version}
==========================

Builds Persona3DS (Shin Megami Tensei: Persona for New Nintendo 3DS) from YOUR OWN copy of
the game. Nothing is downloaded and no game data is included here.

You need
  * Windows 10 or 11 (64-bit)
  * your Persona (PSP) ISO, North American release ULUS-10432
  * its decrypted EBOOT, dumped with PPSSPP: turn on "Dump decrypted EBOOT.BIN on game
    boot" in its developer tools, start the game once, and take ULUS10432.BIN from
    PPSSPP's PSP/SYSTEM/DUMP folder
  * devkitPro with the "3DS Development" component
    (https://devkitpro.org/wiki/Getting_Started - Windows installer)
  ffmpeg (for the music and movies) and the .cia tools come with the builder.

How to use
  1. Run P3P3DS-Builder.exe.
  2. Choose your ISO, the decrypted EBOOT and the devkitPro folder (found automatically
     when possible). Tick "Also make an installable .cia" if you want one.
  3. Press Build. The first build takes about 15 to 45 minutes, depending on your CPU;
     most of it is converting the movies, which happens only once.
     An interrupted build continues where it stopped.
  4. Put on the SD card of your New 3DS (the builder can copy them for you):
       \\3ds\\persona3ds.3dsx
       \\persona3ds\\<your game>.iso
       \\persona3ds\\media\\          (the output\\media folder)
       \\3ds\\dspfirm.cdc             (DSP firmware dump, needed for sound)
     and start Persona3DS from the Homebrew Launcher, or install persona3ds.cia with FBI.

Your own .cia banner: put banner.png (256x128), audio.wav and optionally icon.png (48x48)
in a folder named banner next to P3P3DS-Builder.exe.

The results contain code made from your copy of the game: keep them to yourself,
do not share them.

State of the port: see README.md on GitHub.

Persona3DS by wg481, based on P3P3DS by enderlit aka endercodezz - MIT License (see LICENSE.txt).
Persona is (c) ATLUS. This project is not affiliated with ATLUS or SEGA.
"""

README_LINUX = """P3P3DS Builder {version} (Linux x86-64)
========================================

Builds Persona3DS (Shin Megami Tensei: Persona for New Nintendo 3DS) from YOUR OWN copy of
the game. Nothing is downloaded and no game data is included here.

You need
  * Linux x86-64 (the builder and its code generator are static binaries)
  * your Persona (PSP) ISO, North American release ULUS-10432
  * its decrypted EBOOT, dumped with PPSSPP ("Dump decrypted EBOOT.BIN on game boot" in
    its developer tools; the file appears in PPSSPP's PSP/SYSTEM/DUMP folder)
  * devkitPro with the 3DS packages (https://devkitpro.org/wiki/devkitPro_pacman:
    `sudo dkp-pacman -S 3ds-dev`); found in $DEVKITPRO or /opt/devkitpro.
    NixOS: devkitPro's binaries need an FHS environment (see shell.nix, p3p-3ds).
  ffmpeg (for the music and movies) and the .cia tools come with the builder.

How to use
  1. ./P3P3DS-Builder --iso "/path/to/Persona.iso" --eboot "/path/to/ULUS10432.BIN" [--cia]
     Options: --devkitpro <dir>, --out <file.3dsx>, --banner <dir>, --jobs <n>, --work <dir>.
  2. The first build takes about 15 to 45 minutes, depending on your CPU; most of it is
     converting the movies, which happens only once.
     An interrupted build continues where it stopped.
  3. Put on the SD card of your New 3DS:
       /3ds/persona3ds.3dsx            (from output/)
       /persona3ds/<your game>.iso
       /persona3ds/media/              (output/media)
       /3ds/dspfirm.cdc                (DSP firmware dump, needed for sound)
     and start Persona3DS from the Homebrew Launcher, or install persona3ds.cia with FBI.

The results contain code made from your copy of the game: keep them to yourself,
do not share them.

State of the port: see README.md on GitHub.

Persona3DS by wg481, based on P3P3DS by enderlit aka endercodezz - MIT License (see LICENSE.txt).
Persona is (c) ATLUS. This project is not affiliated with ATLUS or SEGA.
"""


def git_version() -> str:
    try:
        return subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default="0.1.0")
    parser.add_argument("--devkitpro", default="", help="devkitPro folder: strips debug info from the runtime objects")
    parser.add_argument("--ffmpeg", required=True,
                        help="folder of a self-contained ffmpeg build (ffmpeg and ffprobe, with libx264; its bin/ is searched too)")
    parser.add_argument("--cia-tools", default=str(BANNER), help="folder with makerom and bannertool (default: banner/)")
    args = parser.parse_args()

    # P3P3DS: bundled tools.
    def find(folder: Path, name: str) -> Path | None:
        for candidate in (folder / f"{name}{EXE}", folder / "bin" / f"{name}{EXE}", folder / name):
            if candidate.is_file():
                return candidate
        return None
    ffmpeg_dir = Path(args.ffmpeg)
    tools = {"ffmpeg": find(ffmpeg_dir, "ffmpeg"), "ffprobe": find(ffmpeg_dir, "ffprobe"),
             "makerom": find(Path(args.cia_tools), "makerom"), "bannertool": find(Path(args.cia_tools), "bannertool")}
    absent = [n for n, p in tools.items() if p is None]
    if absent:
        print("missing tools: " + ", ".join(absent) + " (see --ffmpeg and --cia-tools)", file=sys.stderr)
        return 1
    encoders = subprocess.run([str(tools["ffmpeg"]), "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    if "libx264" not in encoders:
        print(f"{tools['ffmpeg']} has no libx264 encoder: use a GPL build of ffmpeg", file=sys.stderr)
        return 1

    builder, recomp = f"P3P3DS-Builder{EXE}", f"psp_recomp{EXE}"
    required = {builder: BUILD / builder,
                recomp: BUILD / "recomp/PSPRecomp" / recomp,
                "patches.txt": BUILD3DS / "generated/patches.txt", **RUNTIME_OBJECTS}
    missing = [name for name, path in required.items() if not path.exists()]
    if missing:
        print("missing build outputs: " + ", ".join(missing), file=sys.stderr)
        return 1

    name = f"P3P3DS-Builder-{args.version}" + ("" if WINDOWS else "-linux")
    out = ROOT / "out" / name
    if out.exists():
        shutil.rmtree(out)
    sdk = out / "sdk"
    (sdk / "bin").mkdir(parents=True)
    (sdk / "lib").mkdir()
    shutil.copy2(required[builder], out / builder)
    shutil.copy2(required[recomp], sdk / "bin" / recomp)
    shutil.copy2(required["patches.txt"], sdk / "patches.txt")
    strip = Path(args.devkitpro) / f"devkitARM/bin/arm-none-eabi-strip{EXE}" if args.devkitpro else None
    for obj, path in RUNTIME_OBJECTS.items():
        shutil.copy2(path, sdk / "lib" / obj)
        if strip and strip.exists():  # debug info is 95 % of the archives
            subprocess.check_call([str(strip), "--strip-debug", str(sdk / "lib" / obj)])
    shutil.copytree(ROOT / "recomp/PSPRecomp/include/psprecomp", sdk / "include" / "psprecomp")
    # P3P3DS: ffmpeg (with the DLLs next to it, for builds that have them), the .cia tools and settings.
    for tool_name in ("ffmpeg", "ffprobe", "makerom", "bannertool"):
        shutil.copy2(tools[tool_name], sdk / "bin" / tools[tool_name].name)
    for dll in tools["ffmpeg"].parent.glob("*.dll"):
        shutil.copy2(dll, sdk / "bin" / dll.name)
    (sdk / "cia").mkdir()
    shutil.copy2(ROOT / "platform/3ds/cia/p3p3ds.rsf.in", sdk / "cia" / "p3p3ds.rsf.in")
    art = BANNER if (BANNER / "banner.png").is_file() and (BANNER / "audio.wav").is_file() else CIA_DEFAULTS
    for art_name in ("banner.png", "audio.wav", "icon.png"):
        if (art / art_name).is_file():
            shutil.copy2(art / art_name, sdk / "cia" / art_name)
    if not (sdk / "cia" / "audio.wav").is_file():  # default banner sound: one second of silence
        with wave.open(str(sdk / "cia" / "audio.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(22050)
            w.writeframes(bytes(2 * 22050))
    shutil.copy2(Path(__file__).resolve().parent / "THIRD_PARTY_NOTICES.txt", out / "THIRD_PARTY_NOTICES.txt")
    for license_name in ("LICENSE", "LICENSE.txt", "COPYING.GPLv3", "COPYING.txt"):
        for folder in (ffmpeg_dir, ffmpeg_dir.parent):
            if (folder / license_name).is_file():
                shutil.copy2(folder / license_name, out / f"FFMPEG_{license_name}")
    (sdk / "VERSION").write_text(f"{args.version} {git_version()}\n", encoding="utf-8", newline="\n")
    shutil.copy2(ROOT / "LICENSE", out / "LICENSE.txt")
    readme = README_WINDOWS if WINDOWS else README_LINUX
    (out / "README.txt").write_text(readme.format(version=args.version), encoding="utf-8", newline="\r\n" if WINDOWS else "\n")

    if WINDOWS:
        archive = ROOT / "out" / f"{name}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(out.rglob("*")):
                if f.is_file():
                    z.write(f, Path(name) / f.relative_to(out))
    else:
        archive = ROOT / "out" / f"{name}.tar.gz"
        with tarfile.open(archive, "w:gz", compresslevel=9) as t:
            t.add(out, arcname=name)
    size = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    print(f"{out} ({size / 1e6:.1f} MB), {archive} ({archive.stat().st_size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
