"""Test the direct BSP against real ALSA declarations and a deterministic device double.

The fake device checks control/error handling, not target ABI or acoustic timing.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PcmBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT / "bsp/boards/v821_aitoy/audio_pcm.c"
        assert source.exists(), "direct ALSA backend has not been implemented"
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "test_pcm"
        alsa = Path(os.environ.get("ALSA_TEST_INCLUDE", ROOT / "build/aec-deps/alsa-lib-1.2.8/include"))
        command = [os.environ.get("CC", "cc"), "-std=c99", "-D_POSIX_C_SOURCE=200809L",
                   "-Wall", "-Wextra", "-Werror", "-I" + str(alsa),
                   "-I" + str(ROOT / "bsp/boards/v821_aitoy"),
                   "-I" + str(ROOT / "bsp/include")]
        if os.uname().sysname == "Darwin":
            compat = Path(cls.tmp.name) / "darwin-headers"
            compat.mkdir()
            (compat / "endian.h").write_text(
                "#include <machine/endian.h>\n"
                "#define __BYTE_ORDER __DARWIN_BYTE_ORDER\n"
                "#define __LITTLE_ENDIAN __DARWIN_LITTLE_ENDIAN\n"
                "#define __BIG_ENDIAN __DARWIN_BIG_ENDIAN\n")
            command += ["-I" + str(compat)]
        command += [str(source), str(Path(__file__).with_name("fake_device.c")),
                    "-o", str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True,
                                timeout=6)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_partial_io_and_retry_preserve_samples(self):
        self.run_case("partial")

    def test_flush_drops_only_playback(self):
        self.run_case("flush")

    def test_repeated_prewrite_flush_preserves_capture_margin(self):
        self.run_case("flush-cadence")

    def test_xrun_is_sticky_until_stop_and_restart(self):
        self.run_case("xrun")

    def test_mic_pause_resume_leaves_playback_running(self):
        self.run_case("mic")

    def test_negotiated_rate_and_buffer_are_checked(self):
        self.run_case("configuration")

    def test_capture_and_playback_open_failures_release_handles(self):
        self.run_case("open-failure")

    def test_waiting_io_has_bounded_timeout(self):
        self.run_case("timeout")

    def test_drain_restores_continuous_playback(self):
        self.run_case("drain")


if __name__ == "__main__":
    unittest.main(verbosity=2)
