"""Host checks for the offline AEC path. Synthetic audio is not board acceptance."""
import array
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest


RATE = 16000
PROBE = Path(os.environ.get("V821_AEC_PROBE", "build/aec/v821_aec_probe")).resolve()


def write_pcm(path, samples):
    pcm = array.array("h", (max(-32768, min(32767, round(x))) for x in samples))
    if sys.byteorder != "little":
        pcm.byteswap()
    path.write_bytes(pcm.tobytes())


def read_pcm(path):
    pcm = array.array("h")
    pcm.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm


def signal(seconds, seed):
    rng = random.Random(seed)
    return [rng.randrange(-7000, 7001) for _ in range(RATE * seconds)]


def echo_of(ref):
    # A fixed, causal two-path echo, with 40 ms device/acoustic delay.
    return [0.5 * ref[i - 640] + 0.15 * ref[i - 720] if i >= 720 else 0
            for i in range(len(ref))]


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(PROBE.is_file(), "AEC probe implementation has not been built")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def replay(self, mic, ref, *args, stem=""):
        write_pcm(self.root / f"{stem}mic.raw", mic)
        write_pcm(self.root / f"{stem}ref.raw", ref)
        result = subprocess.run(
            [str(PROBE), "replay", "--mic", str(self.root / f"{stem}mic.raw"),
             "--ref", str(self.root / f"{stem}ref.raw"), "--out", str(self.root / f"{stem}out.raw"),
             *args], capture_output=True, text=True)
        return result

    def test_causal_echo_is_reduced_without_denoising(self):
        ref = signal(10, 17)
        mic = echo_of(ref)
        result = self.replay(mic, ref)
        self.assertEqual(result.returncode, 0, result.stderr)
        out = read_pcm(self.root / "out.raw")
        self.assertEqual(len(out), len(mic))
        start = 5 * RATE  # Allow the adaptive filter to converge.
        reduction = 10 * math.log10(sum(x * x for x in mic[start:]) /
                                    max(1, sum(x * x for x in out[start:])))
        self.assertGreater(reduction, 20, f"synthetic echo reduction: {reduction:.1f} dB")
        print(f"synthetic echo reduction: {reduction:.1f} dB")

    def test_near_end_speech_survives_double_talk(self):
        ref = signal(10, 21)
        mic = echo_of(ref)
        near = [0] * len(ref)
        rng = random.Random(27)
        # Independent, band-limited near-end signal, after far-end adaptation.
        value = 0.0
        for i in range(5 * RATE, len(ref)):
            value = 0.85 * value + 0.15 * rng.randrange(-13000, 13001)
            near[i] = round(value)
            mic[i] += near[i]
        result = self.replay(mic, ref)
        self.assertEqual(result.returncode, 0, result.stderr)
        out = read_pcm(self.root / "out.raw")
        # Speex always applies a DC-notch filter, even with denoising disabled.
        # Compare against the same near-end signal through that filter, so that
        # its frequency response is not mistaken for double-talk suppression.
        baseline = self.replay(near, [0] * len(near), stem="near-only-")
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        filtered_near = read_pcm(self.root / "near-only-out.raw")
        start = 6 * RATE
        target = near[start:]
        actual = out[start:]
        gain = sum(a * b for a, b in zip(actual, target)) / sum(x * x for x in target)
        expected = filtered_near[start:]
        error = sum((a - b) ** 2 for a, b in zip(actual, expected))
        snr = 10 * math.log10(sum(x * x for x in expected) / max(1, error))
        self.assertGreater(gain, 0.7, f"near-end gain: {gain:.3f}")
        self.assertGreater(snr, 8, f"synthetic double-talk SNR: {snr:.1f} dB")
        print(f"synthetic near-end gain: {gain:.3f}; double-talk SNR: {snr:.1f} dB")

    def test_partial_final_frame_preserves_sample_count(self):
        samples = signal(1, 3) + [200, -200, 50]
        result = self.replay(samples, [0] * len(samples))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(read_pcm(self.root / "out.raw")), len(samples))

    def test_mismatched_inputs_fail_without_creating_output(self):
        result = self.replay([1] * 160, [1] * 159)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same length", result.stderr)
        self.assertFalse((self.root / "out.raw").exists())

    def test_odd_byte_input_is_rejected(self):
        mic = self.root / "mic.raw"
        ref = self.root / "ref.raw"
        mic.write_bytes(b"\x01")
        ref.write_bytes(b"\x01")
        result = subprocess.run(
            [str(PROBE), "replay", "--mic", str(mic), "--ref", str(ref),
             "--out", str(self.root / "out.raw")], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("16-bit", result.stderr)
        self.assertFalse((self.root / "out.raw").exists())

    def test_existing_recording_is_never_overwritten(self):
        out = self.root / "out.raw"
        out.write_bytes(b"original recording")
        result = self.replay([0] * 160, [0] * 160)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(out.read_bytes(), b"original recording")

    def test_unexpected_and_out_of_range_options_fail(self):
        for args in (("--tail-ms", "0"), ("--tail-ms", "-1"),
                     ("--tail-ms", "999999999999999999999"),
                     ("--tail-ms", "200junk"), ("--unknown", "1"),
                     ("--tail-ms",)):
            with self.subTest(args=args):
                result = self.replay([0] * 160, [0] * 160, *args)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "out.raw").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
