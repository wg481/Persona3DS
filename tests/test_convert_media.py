"""tools/convert_media.py without ffmpeg: a fake converter writes IMA-ADPCM WAV files."""
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("convert_media", ROOT / "tools/convert_media.py")
cm = importlib.util.module_from_spec(spec)
sys.modules["convert_media"] = cm  # dataclasses look the module up
spec.loader.exec_module(cm)


def at3(frames: bytes, block: int = 0x180, fmt: int = 0x270) -> bytes:
    """Minimal .at3: fmt (32 bytes), fact, data of whole frames."""
    fmt_ck = struct.pack("<HHIIHHH", fmt, 2, 44100, 44100 * block // 1024, block, 0, 14) + bytes(14)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt_ck)) + fmt_ck
    body += b"fact" + struct.pack("<III", 8, 0, 0) + b"data" + struct.pack("<I", len(frames)) + frames
    return b"RIFF" + struct.pack("<I", len(body)) + body


def ima_wav(samples: int) -> bytes:
    """Stereo IMA-ADPCM WAV header with a fact chunk (data content irrelevant here)."""
    align, per_block = 24, 17
    blocks = -(-samples // per_block)
    fmt_ck = struct.pack("<HHIIHHHH", 0x11, 2, 44100, 0, align, 4, 2, per_block)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt_ck)) + fmt_ck
    body += b"fact" + struct.pack("<II", 4, samples) + b"data" + struct.pack("<I", blocks * align) + bytes(blocks * align)
    return b"RIFF" + struct.pack("<I", len(body)) + body


class ConvertMediaTests(unittest.TestCase):
    def test_fnv_matches_runtime(self):
        # Same test vector as tests/test_fork_media.cpp.
        self.assertEqual(cm.fnv1a64(b"a"), 0xAF63DC4C8601EC8C)
        self.assertEqual(cm.fnv1a64(b""), 0xCBF29CE484222325)

    def test_parse_at3(self):
        data = at3(bytes(0x180 * 3))
        info = cm.parse_at3(data)
        self.assertEqual(info.block_align, 0x180)
        self.assertEqual(info.frame_samples, 1024)
        self.assertEqual(info.raw_samples, 3 * 1024)
        self.assertEqual(data[info.data_off - 8:info.data_off - 4], b"data")
        self.assertEqual(cm.parse_at3(at3(bytes(0x180), fmt=0xFFFE)).frame_samples, 2048)
        with self.assertRaises(ValueError):
            cm.parse_at3(at3(bytes(0x180), fmt=0x1))

    def test_convert_index(self):
        frame = 0x180
        a = at3(bytes(frame) + b"\x01" * frame)       # same header and first frame as b
        b = at3(bytes(frame) + b"\x02" * frame)
        c = at3(b"\x03" * frame * 2)
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp" if (ROOT / ".tmp").is_dir() else None) as tmp:
            src, out = Path(tmp) / "USRDIR", Path(tmp) / "media"
            (src / "sdata").mkdir(parents=True)
            for name, data in (("bgm01.at3", a), ("bgm02.at3", b), ("bgm03.at3", c), ("bgm04.at3", c)):
                (src / "sdata" / name).write_bytes(data)
            (src / "sdata" / "bad.at3").write_bytes(b"not riff")
            calls = []

            def fake(s, d):
                calls.append(s.name)
                d.write_bytes(ima_wav(43050))  # a wrong fact count, as ffmpeg's WAV muxer wrote

            logs = []
            tracks = cm.convert(src, out, fake, log=logs.append)
            self.assertEqual(len(tracks), 4)
            self.assertEqual(calls, ["bgm01.at3", "bgm02.at3", "bgm03.at3"])  # bgm04 is identical to bgm03
            self.assertTrue(any("bad.at3" in line for line in logs))
            entries = [l.split("#")[0].split() for l in (out / "index.txt").read_text().splitlines() if l.startswith("atrac")]
            by_file = {e[4]: e for e in entries}
            first = cm.parse_at3(a)
            # a and b share header + first frame: the prefix doubles past it.
            self.assertGreater(int(by_file["sdata_bgm01.wav"][1]), first.data_off + frame)
            self.assertEqual(int(by_file["sdata_bgm03.wav"][1]), first.data_off + frame)
            for e in entries:
                self.assertEqual(e[0], "atrac")
                self.assertEqual(int(e[3]), 0)
            for e, data in zip(entries, (a, b, c, c)):
                self.assertEqual(int(e[2], 16), cm.fnv1a64(data[:int(e[1])]))
            self.assertEqual(entries[3][4], "sdata_bgm03.wav")


def psmf_with_audio(frames: list[bytes], b2: int = 0x28, b3: int = 0x2E) -> bytes:
    """PSMF header (stream at 0x800) + packs whose private stream 1 carries the ATRAC3plus units."""
    es = b"".join(bytes([0x0F, 0xD0, b2, b3]) + bytes(4) + f for f in frames)
    packs = []
    for i in range(0, len(es), 1900):
        payload = b"\x00\x00\x00\x00" + es[i:i + 1900]
        pes = b"\x00\x00\x01\xbd" + struct.pack(">H", 3 + len(payload)) + b"\x81\x00\x00" + payload
        pack = b"\x00\x00\x01\xba" + bytes(9) + b"\xf8" + pes
        packs.append(pack + bytes(2048 - len(pack)))
    header = bytearray(0x800)
    header[0:4] = b"PSMF"
    struct.pack_into(">I", header, 8, 0x800)
    return bytes(header) + b"".join(packs)


class MovieTests(unittest.TestCase):
    def test_access_units(self):
        aud = b"\x00\x00\x00\x01\x09\xf0"
        stream = aud + b"\x00\x00\x00\x01\x67AB" + b"\x00\x00\x01\x65CD" + aud + b"\x00\x00\x01\x41EF"
        aus = cm.h264_access_units(stream)
        self.assertEqual(aus, [(0, 19), (19, len(stream) - 19)])  # 6 + 7 + 6 bytes, then 6 + 6
        self.assertEqual(cm.h264_access_units(b"\x00\x00\x01\x65CD"), [])

    def test_psmf_audio_oma(self):
        frames = [bytes([k]) * 376 for k in range(5)]  # (0x2E + 1) * 8 = 376 bytes per frame
        oma = cm.psmf_audio_oma(psmf_with_audio(frames))
        self.assertEqual(oma[:3], b"EA3")
        self.assertEqual((oma[5], oma[32], oma[33:36]), (96, 1, b"\x00\x28\x2e"))
        self.assertEqual(oma[96:], b"".join(frames))
        self.assertIsNone(cm.psmf_audio_oma(psmf_with_audio([])))
        self.assertIsNone(cm.psmf_audio_oma(b"RIFF" + bytes(100)))

    def test_convert_movies(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "USRDIR", Path(tmp) / "media"
            (src / "MOVIE").mkdir(parents=True)
            (src / "MOVIE" / "MV00.pmf").write_bytes(psmf_with_audio([bytes(376)] * 3))
            (src / "MOVIE" / "MV01.pmf").write_bytes(psmf_with_audio([]))
            (src / "MOVIE" / "bad.pmf").write_bytes(b"nope")
            aud = b"\x00\x00\x00\x01\x09\xf0"

            def video(s, d):
                d.write_bytes((aud + b"\x00\x00\x01\x65XX") * 3)

            def audio(s, d):
                if cm.psmf_audio_oma(s.read_bytes()) is None:
                    raise ValueError("no ATRAC3plus stream")
                d.write_bytes(ima_wav(3 * 2048))

            logs = []
            lines = cm.convert_movies(src, out, video, audio, lambda p: (480, 272), log=logs.append)
            self.assertEqual(len(lines), 2)
            first = lines[0].split("#")[0].split()
            head = (src / "MOVIE" / "MV00.pmf").read_bytes()[:0x800]
            self.assertEqual(first, ["movie", "2048", f"{cm.fnv1a64(head):016x}", "MOVIE_MV00.h264", "MOVIE_MV00.idx", "MOVIE_MV00.wav"])
            self.assertEqual(lines[1].split()[5], "-")  # no audio stream
            self.assertTrue(any("bad.pmf" in l for l in logs))
            idx = (out / "MOVIE_MV00.idx").read_bytes()
            self.assertEqual(idx[:4], b"P3MV")
            self.assertEqual(struct.unpack_from("<IIII", idx, 4), (1, 480, 272, 3))
            self.assertEqual(struct.unpack_from("<II", idx, 20 + 8), (len(aud) + 6, len(aud) + 6))  # AUD + 3-byte start code + 3 bytes


class FinishWavTests(unittest.TestCase):
    def run_finish(self, wav: bytes, raw: int):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "t.wav"
            p.write_bytes(wav)
            result = cm.finish_wav(p, raw)
            blocks, fact_at, _ = cm.ima_layout(p.read_bytes())
            return result, struct.unpack_from("<I", p.read_bytes(), fact_at)[0]

    def test_all_raw_samples_kept(self):
        # 121 blocks of 17 = 2057 >= 2048 raw samples: offset 0, fact = raw count.
        self.assertEqual(self.run_finish(ima_wav(2048), 2048), ((0, 2048), 2048))

    def test_wrong_fact_is_rewritten(self):
        self.assertEqual(self.run_finish(ima_wav(43050), 2048)[1], 2048)

    def test_dropped_leading_samples(self):
        # 100 blocks = 1700 samples for 2048 raw: 348 missing, assumed at the start.
        (offset, samples), fact = self.run_finish(ima_wav(1700), 2048)
        self.assertEqual((offset, samples, fact), (-348, 1700, 1700))


if __name__ == "__main__":
    unittest.main()
