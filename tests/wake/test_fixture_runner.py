"""Check that fixture acceptance cannot mistake probe errors for a negative."""
import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools/wake/check_fixtures.py"


class FixtureRunnerTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.exists(), "acoustic fixture runner is not implemented")
        spec = importlib.util.spec_from_file_location("wake_fixtures", SCRIPT)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def test_negative_requires_explicit_no_hit_status(self):
        line = "wake replay frames=100 hits=0 status=2\n"
        self.assertTrue(self.module.accept_result(2, line, 0)["passed"])
        self.assertFalse(self.module.accept_result(1, line, 0)["passed"])
        self.assertFalse(self.module.accept_result(0, line, 0)["passed"])

    def test_repeated_wake_requires_expected_count(self):
        line = "wake replay frames=300 hits=2 status=0\n"
        self.assertTrue(self.module.accept_result(0, line, 2)["passed"])
        self.assertFalse(self.module.accept_result(0, line, 1)["passed"])

    def test_missing_summary_is_not_accepted(self):
        self.assertFalse(self.module.accept_result(0, "initialized\n", 1)["passed"])
        self.assertFalse(self.module.accept_result(2, "", 0)["passed"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
