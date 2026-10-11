#!/usr/bin/env python3
"""Convert the game's ATRAC music for the runtime (Persona3DS fork tool).

The PSP game decodes its .at3 music with the ATRAC codec, which the runtime
does not have. This tool converts every .at3 file under the extracted game
data (tools/prepare_game.py writes <profile>/game/USRDIR) with the user's own
ffmpeg into IMA-ADPCM WAV files, and writes index.txt, which lets the runtime
recognize each track by a hash of the first bytes of the .at3 file
(core/include/p3p3ds/hle/fork_media.hpp). Nothing is downloaded and nothing
leaves the workspace.

usage: python tools/convert_media.py [--profile DIR] [--input DIR] [--output DIR] [--ffmpeg PATH] [--force]
  default input:  <profile>/game/USRDIR
  default output: <profile>/game/media (the PC runner reads it from there;
                  copy the folder to sdmc:/persona3ds/media for the 3DS)
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE_DIR = os.environ.get("P3P_PROFILE_DIR", "profiles/p1p")
INDEX_HEADER = (
    "# Persona3DS converted media (tools/convert_media.py); format: core/include/p3p3ds/hle/fork_media.hpp\n"
    "# atrac <prefix bytes> <FNV-1a 64 of the .at3 prefix> <sample offset> <file>\n"
)
MAX_PREFIX = 0x10000  # [UNVERIFIED] the game hands sceAtrac at least this much of a track; larger prefixes are reported


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


@dataclass
class At3Info:
    data_off: int
    data_size: int
    block_align: int
    frame_samples: int  # 1024 ATRAC3, 2048 ATRAC3plus

    @property
    def raw_samples(self) -> int:
        return (self.data_size // self.block_align) * self.frame_samples


def parse_at3(data: bytes) -> At3Info:
    """Data offset, block size and codec of an .at3 (RIFF WAVE) file."""
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF WAVE file")
    pos, block_align, frame = 12, 0, 0
    while pos + 8 <= len(data):
        ck, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if ck == b"fmt ":
            fmt_code, = struct.unpack_from("<H", data, pos + 8)
            block_align, = struct.unpack_from("<H", data, pos + 20)
            frame = 1024 if fmt_code == 0x270 else 2048 if fmt_code == 0xFFFE else 0
            if frame == 0:
                raise ValueError(f"not ATRAC3/ATRAC3plus (format 0x{fmt_code:X})")
        elif ck == b"data":
            if block_align == 0:
                raise ValueError("data chunk before fmt")
            return At3Info(pos + 8, size, block_align, frame)
        pos += 8 + size + (size & 1)
    raise ValueError("no data chunk")


@dataclass
class Track:
    path: Path        # source .at3
    rel: str          # path relative to the input folder, '/' separated
    data: bytes = field(repr=False)
    info: At3Info = None
    prefix: int = 0
    output: str = ""  # converted file name in the media folder


def choose_prefixes(tracks: list[Track]) -> None:
    """Smallest prefix (header + whole ATRAC frames, doubling) that tells each file apart from all different files."""
    for t in tracks:
        others = [o.data for o in tracks if o.data != t.data]
        n = min(t.info.data_off + t.info.block_align, len(t.data))
        while any(o[:n] == t.data[:n] for o in others) and n < len(t.data):
            n = min(n * 2, len(t.data))
        t.prefix = n


def output_name(rel: str) -> str:
    return rel.replace("/", "_").rsplit(".", 1)[0] + ".wav"


def ima_layout(data: bytes) -> tuple[int, int, int]:
    """(whole-block sample frames, fact chunk offset or -1, data size) of an IMA-ADPCM WAV."""
    pos, fact_at, align, channels = 12, -1, 0, 0
    while pos + 8 <= len(data):
        ck, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if ck == b"fmt ":
            channels, = struct.unpack_from("<H", data, pos + 10)
            align, = struct.unpack_from("<H", data, pos + 20)
        elif ck == b"fact":
            fact_at = pos + 8
        elif ck == b"data":
            if not channels or align <= 4 * channels:
                raise ValueError("not an IMA-ADPCM WAV")
            per_block = 1 + (align - 4 * channels) * 2 // channels
            return (size // align) * per_block, fact_at, size
        pos += 8 + size + (size & 1)
    raise ValueError("no data chunk")


def finish_wav(path: Path, raw_samples: int) -> tuple[int, int]:
    """Sample offset and sample count of a converted file; writes the count into its fact chunk.

    ffmpeg 6-7 decodes ATRAC3plus .at3 files to exactly the raw codec samples
    (frames x 2048) and IMA-ADPCM adds no delay (measured on pspautotests
    sample.at3: 251,904 samples, best lag 0), but its WAV muxer wrote a wrong
    fact count (43,050), so the count is recomputed here. If a converter drops
    a block or more, the missing samples are assumed to be the leading codec
    delay [UNVERIFIED] and the offset shifts by them.
    """
    data = bytearray(path.read_bytes())
    blocks_samples, fact_at, _ = ima_layout(bytes(data))
    deficit = raw_samples - blocks_samples
    offset = -deficit if deficit > 0 else 0
    samples = min(raw_samples + offset, blocks_samples)
    if fact_at >= 0:
        struct.pack_into("<I", data, fact_at, samples)
        path.write_bytes(bytes(data))
    return offset, samples


def run_ffmpeg(ffmpeg: str, src: Path, dst: Path) -> None:
    subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src),
                    "-c:a", "adpcm_ima_wav", str(dst)], check=True)


def convert(input_dir: Path, output_dir: Path, convert_one, force: bool = False, log=print) -> list[Track]:
    tracks = []
    for p in sorted(input_dir.rglob("*")):
        if p.is_file() and p.suffix.lower() == ".at3":
            data = p.read_bytes()
            t = Track(p, p.relative_to(input_dir).as_posix(), data)
            try:
                t.info = parse_at3(data)
            except ValueError as e:
                log(f"  skip {t.rel}: {e}")
                continue
            tracks.append(t)
    choose_prefixes(tracks)
    output_dir.mkdir(parents=True, exist_ok=True)
    lines, done, converted_tracks = [INDEX_HEADER], {}, []
    for t in tracks:
        digest = hashlib.sha256(t.data).hexdigest()
        if digest in done:  # identical file: same converted track
            t.output = done[digest]
        else:
            t.output = output_name(t.rel)
            dst = output_dir / t.output
            if force or not dst.exists():
                try:
                    convert_one(t.path, dst)
                except (subprocess.CalledProcessError, OSError) as e:
                    log(f"  skip {t.rel}: conversion failed ({e})")
                    dst.unlink(missing_ok=True)
                    continue
            done[digest] = t.output
        offset, converted = finish_wav(output_dir / t.output, t.info.raw_samples)
        if t.prefix > MAX_PREFIX:
            log(f"  warning {t.rel}: needs a {t.prefix}-byte prefix to be told apart; the game may hand over less")
        lines.append(f"atrac {t.prefix} {fnv1a64(t.data[:t.prefix]):016x} {offset} {t.output}  # {t.rel}\n")
        log(f"  {t.rel} -> {t.output}  prefix {t.prefix}  samples {converted} (raw {t.info.raw_samples}, offset {offset})")
        converted_tracks.append(t)
    (output_dir / "index.txt").write_text("".join(lines), newline="\n")
    return converted_tracks


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", default=DEFAULT_PROFILE_DIR)
    ap.add_argument("--input", default=None)
    ap.add_argument("--output", default=None)
    ap.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable (default: ffmpeg on PATH)")
    ap.add_argument("--force", action="store_true", help="convert again even if the output exists")
    args = ap.parse_args()
    profile = Path(args.profile)
    if not profile.is_absolute():
        profile = ROOT / profile
    input_dir = Path(args.input) if args.input else profile / "game" / "USRDIR"
    output_dir = Path(args.output) if args.output else profile / "game" / "media"
    if not input_dir.is_dir():
        print(f"{input_dir} not found: run tools/prepare_game.py first", file=sys.stderr)
        return 1
    ffmpeg = shutil.which(args.ffmpeg) or (args.ffmpeg if Path(args.ffmpeg).is_file() else None)
    if ffmpeg is None:
        print("ffmpeg not found: install it (or pass --ffmpeg <path>); media conversion is optional, "
              "without it the music stays silent", file=sys.stderr)
        return 1
    print(f"Converting ATRAC music from {input_dir} to {output_dir} with {ffmpeg}")
    tracks = convert(input_dir, output_dir, lambda src, dst: run_ffmpeg(ffmpeg, src, dst), args.force)
    print(f"{len(tracks)} tracks; index: {output_dir / 'index.txt'}")
    print("3DS: copy this folder to sdmc:/persona3ds/media")
    return 0


if __name__ == "__main__":
    sys.exit(main())
