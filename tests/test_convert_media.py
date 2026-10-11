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
