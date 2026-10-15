#!/usr/bin/env python3
"""Convert the game's ATRAC music and PSMF movies for the runtime (Persona3DS fork tool).

The PSP game decodes its .at3 music with the ATRAC codec, which the runtime
does not have. This tool converts every .at3 file under the extracted game
data (tools/prepare_game.py writes <profile>/game/USRDIR) with the user's own
ffmpeg into IMA-ADPCM WAV files, and writes index.txt, which lets the runtime
recognize each track by a hash of the first bytes of the .at3 file
(core/include/p3p3ds/hle/fork_media.hpp). Movies (.pmf) become H.264
Baseline streams with a frame index plus IMA-ADPCM audio
(core/include/p3p3ds/hle/fork_movie.hpp). Nothing is downloaded and nothing
leaves the workspace.

usage: python tools/convert_media.py [--profile DIR] [--input DIR] [--output DIR] [--ffmpeg PATH] [--force] [--skip-movies]
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
    "# movie <prefix bytes> <FNV-1a 64 of the PSMF header> <video .h264> <frames .idx> <audio .wav or ->\n"
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


def finish_wav(path: Path, raw_samples: int | None) -> tuple[int, int]:
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
    if raw_samples is None:  # no reference count (movie audio): every whole block
        raw_samples = blocks_samples
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


PSMF_PREFIX = 0x800  # the PSMF header the game reads and passes to sceMpegQueryStreamOffset


def h264_access_units(data: bytes) -> list[tuple[int, int]]:
    """(offset, size) of each access unit of an Annex B stream, split at access unit delimiters (NAL type 9)."""
    starts, i = [], data.find(b"\x00\x00\x01")
    while i >= 0 and i + 3 < len(data):
        if data[i + 3] & 0x1F == 9:
            starts.append(i - 1 if i > 0 and data[i - 1] == 0 else i)
        i = data.find(b"\x00\x00\x01", i + 3)
    return [(a, (starts[k + 1] if k + 1 < len(starts) else len(data)) - a) for k, a in enumerate(starts)]


def write_frame_index(path: Path, width: int, height: int, aus: list[tuple[int, int]]) -> None:
    out = bytearray(b"P3MV" + struct.pack("<IIII", 1, width, height, len(aus)))
    for offset, size in aus:
        out += struct.pack("<II", offset, size)
    path.write_bytes(bytes(out))


def run_ffmpeg_video(ffmpeg: str, src: Path, dst: Path) -> None:
    # Baseline (no B-frames: one access unit per picture in display order), level 3.0 for the
    # New 3DS decoder, an access unit delimiter before every picture, one slice per picture.
    # P3P3DS: one reference picture (ref=1; preset medium uses 3). The New 3DS showed the start of the
    # opening washed out until a scene change while the same .h264 plays correctly in ffplay
    # (docs/CURRENT_STATE.md 2026-10-14): [INFERRED] the decoder mispredicts from older references.
    subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src), "-an",
                    "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.0", "-bf", "0", "-pix_fmt", "yuv420p",
                    "-preset", "medium", "-crf", "21", "-x264-params", "aud=1:slices=1:sliced-threads=0:ref=1",
                    "-fps_mode", "passthrough", "-f", "h264", str(dst)], check=True)


def psmf_audio_oma(pmf: bytes, channel: int = 0) -> bytes | None:
    """The ATRAC3plus stream of a PSMF movie as an OMA file, or None without audio.

    Same demultiplexing as core/src/hle/mpeg.cpp PsDemuxer: 2048-byte packs from the
    stream offset (header word 8, big endian); private stream 1 (0xBD) payloads start
    with the substream id and 3 more bytes, then ATRAC3plus access units of an 8-byte
    header (0F D0 b2 b3 ...) and ((b2 & 3) << 8 | b3) * 8 + 8 frame bytes. b2 b3 are the
    codec parameters OMA stores (ffmpeg omadec: channels in bits 10-12, sample rate in
    13-15, frame size in 0-9), so the frames go unchanged after a 96-byte EA3 header.
    """
    if pmf[:4] != b"PSMF" or len(pmf) < 16:
        return None
    es, p = bytearray(), struct.unpack_from(">I", pmf, 8)[0]
    while p + 2048 <= len(pmf):
        pk, q = pmf[p:p + 2048], 0
        p += 2048
        while q + 6 <= len(pk) and pk[q:q + 3] == b"\x00\x00\x01":
            sid = pk[q + 3]
            if sid == 0xBA:
                q += 14 + (pk[q + 13] & 7)
                continue
            if sid == 0xB9:
                break
            length = struct.unpack_from(">H", pk, q + 4)[0]
            body, nxt = q + 6, q + 6 + length
            if sid == 0xBD and length >= 3:
                payload = body + 3 + pk[body + 2]
                if payload + 4 <= nxt and pk[payload] == channel:
                    es += pk[payload + 4:nxt]
            q = nxt
    frames, i = [], 0
    while i + 8 <= len(es) and es[i] == 0x0F and es[i + 1] == 0xD0:
        size = (((es[i + 2] & 3) << 8) | es[i + 3]) * 8 + 8
        if i + 8 + size > len(es):
            break
        frames.append(bytes(es[i + 8:i + 8 + size]))
        i += 8 + size
    if not frames:
        return None
    header = bytearray(96)
    header[0:3], header[5], header[6:8], header[32] = b"EA3", 96, b"\xff\xff", 1  # not encrypted, ATRAC3plus
    header[34], header[35] = es[2], es[3]
    return bytes(header) + b"".join(frames)


def run_ffmpeg_movie_audio(ffmpeg: str, src: Path, dst: Path) -> None:
    oma = psmf_audio_oma(src.read_bytes())
    if oma is None:
        raise ValueError("no ATRAC3plus stream")
    tmp = dst.with_suffix(".oma")
    tmp.write_bytes(oma)
    try:
        subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "oma", "-i", str(tmp),
                        "-c:a", "adpcm_ima_wav", str(dst)], check=True)
    finally:
        tmp.unlink(missing_ok=True)


def probe_size(ffprobe: str, path: Path) -> tuple[int, int]:
    out = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                          "-of", "csv=p=0", str(path)], check=True, capture_output=True, text=True).stdout
    w, h = out.strip().split(",")[:2]
    return int(w), int(h)


def convert_movies(input_dir: Path, output_dir: Path, video_one, audio_one, probe, force: bool = False, log=print) -> list[str]:
    """Index lines of the converted .pmf movies under input_dir."""
    movies = [p for p in sorted(input_dir.rglob("*")) if p.is_file() and p.suffix.lower() == ".pmf"]
    heads = {}
    for p in movies:
        with p.open("rb") as f:
            heads[p] = f.read(PSMF_PREFIX)
    output_dir.mkdir(parents=True, exist_ok=True)
    lines = []
    for p in movies:
        rel = p.relative_to(input_dir).as_posix()
        head = heads[p]
        if head[:4] != b"PSMF":
            log(f"  skip {rel}: not a PSMF movie")
            continue
        if any(o != p and heads[o] == head for o in movies):
            log(f"  warning {rel}: its {len(head)}-byte header equals another movie's; the first one listed wins")
        base = rel.replace("/", "_").rsplit(".", 1)[0]
        video, frames, audio = base + ".h264", base + ".idx", base + ".wav"
        try:
            if force or not (output_dir / video).exists() or not (output_dir / frames).exists():
                video_one(p, output_dir / video)
                width, height = probe(output_dir / video)
                aus = h264_access_units((output_dir / video).read_bytes())
                if not aus:
                    raise ValueError("no access unit delimiters in the converted stream")
                write_frame_index(output_dir / frames, width, height, aus)
            if force or not (output_dir / audio).exists():
                try:
                    audio_one(p, output_dir / audio)
                    finish_wav(output_dir / audio, None)
                except (subprocess.CalledProcessError, OSError, ValueError) as e:
                    log(f"  {rel}: no audio ({e})")
                    (output_dir / audio).unlink(missing_ok=True)
        except (subprocess.CalledProcessError, OSError, ValueError) as e:
            log(f"  skip {rel}: conversion failed ({e})")
            for name in (video, frames):
                (output_dir / name).unlink(missing_ok=True)
            continue
        count = struct.unpack_from("<I", (output_dir / frames).read_bytes(), 16)[0]
        audio_field = audio if (output_dir / audio).exists() else "-"
        lines.append(f"movie {len(head)} {fnv1a64(head):016x} {video} {frames} {audio_field}  # {rel}\n")
        log(f"  {rel} -> {video}  {count} pictures, audio {audio_field}")
    return lines


def convert(input_dir: Path, output_dir: Path, convert_one, force: bool = False, log=print, extra_lines=()) -> list[Track]:
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
    lines.extend(extra_lines)
    (output_dir / "index.txt").write_text("".join(lines), newline="\n")
    return converted_tracks


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", default=DEFAULT_PROFILE_DIR)
    ap.add_argument("--input", default=None)
    ap.add_argument("--output", default=None)
    ap.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable (default: ffmpeg on PATH)")
    ap.add_argument("--force", action="store_true", help="convert again even if the output exists")
    ap.add_argument("--skip-movies", action="store_true", help="music only (movies take much longer)")
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
    movie_lines = []
    if not args.skip_movies:
        ffprobe = shutil.which("ffprobe", path=str(Path(ffmpeg).parent)) or shutil.which("ffprobe")
        if ffprobe is None:
            print("ffprobe (part of ffmpeg) not found: movies skipped", file=sys.stderr)
        else:
            print(f"Converting movies from {input_dir} (H.264 Baseline + IMA-ADPCM audio; this takes a while)")
            movie_lines = convert_movies(input_dir, output_dir, lambda s, d: run_ffmpeg_video(ffmpeg, s, d),
                                         lambda s, d: run_ffmpeg_movie_audio(ffmpeg, s, d),
                                         lambda p: probe_size(ffprobe, p), args.force)
    print(f"Converting ATRAC music from {input_dir} to {output_dir} with {ffmpeg}")
    tracks = convert(input_dir, output_dir, lambda src, dst: run_ffmpeg(ffmpeg, src, dst), args.force, extra_lines=movie_lines)
    print(f"{len(tracks)} tracks, {len(movie_lines)} movies; index: {output_dir / 'index.txt'}")
    print("3DS: copy this folder to sdmc:/persona3ds/media")
    return 0


if __name__ == "__main__":
    sys.exit(main())
