"""Format and dictionary checks for the small Mandarin KWS model preparer."""
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/wake/prepare_model.py"


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.exists(), "Mandarin model preparer is not implemented")
        spec = importlib.util.spec_from_file_location("prepare_model", SCRIPT)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def test_keyword_uses_model_dictionary_phones(self):
        lexicon = {"你": "n i3", "好": "h ao3", "你好": "n i3 h ao3",
                   "涂": "t u2", "鸦": "ii ia1", "涂鸦": "t u2 ii ia1"}
        self.assertEqual(self.module.phonemes("你好涂鸦", lexicon), "n i3 h ao3 t u2 ii ia1")

    def test_custom_phrase_and_unknown_character(self):
        self.assertEqual(self.module.phonemes("小涂", {"小": "x iao3", "涂": "t u2"}),
                         "x iao3 t u2")
        with self.assertRaises(ValueError):
            self.module.phonemes("你好未知", {"你好": "n i3 h ao3"})

    def test_invalid_phrase_is_not_silently_changed(self):
        for text in ("", "你好 涂鸦", "你好\n涂鸦", "x" * 128):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.module.phonemes(text, {text: "n i3"})

    def test_binary_model_rejects_invalid_header_and_truncation(self):
        for data in (b"", b"not-an-s3-model", b"s3\nendhdr\n\x44\x33\x22\x11"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.module.reduce_gaussian(data, "means", 576)

    def test_compact_inventory_keeps_keyword_tones_and_all_base_phones(self):
        rows = [[name, "-", "-", "-", "n/a", "0", "0", "1", "2", "N"]
                for name in ("SIL", "a1", "a3", "a5", "n")]
        selected = self.module.select_inventory(rows, {"SIL", "a3", "n"}, compact=True)
        self.assertEqual([row[0] for row in selected], ["SIL", "a3", "n"])
        self.assertEqual(self.module.select_inventory(rows, {"SIL", "a3"}, compact=False), rows)

    def test_keyword_triphones_retain_only_exact_chain_contexts(self):
        rows = [[center, left, right, position, "n/a", "0", "600", "601", "602", "N"]
                for center, left, right, position in
                (("n", "SIL", "i3", "b"), ("n", "SIL", "i3", "i"),
                 ("i3", "n", "h", "i"), ("h", "i3", "SIL", "e"),
                 ("h", "a1", "SIL", "e"))]
        selected = self.module.select_keyword_triphones(rows, ["n", "i3", "h"])
        self.assertEqual(selected, rows[:4])


if __name__ == "__main__":
    unittest.main(verbosity=2)
