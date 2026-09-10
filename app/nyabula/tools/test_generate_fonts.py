#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Offline tests of font selection and glyph-source discovery."""

import tempfile
import unittest
from pathlib import Path

import generate_fonts


class FontSelectionTest(unittest.TestCase):
    def test_missing_original_uses_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fallback = root / "NotoSans.otf"
            fallback.touch()
            self.assertEqual(generate_fonts.select_font(root, "MiSans.ttf", fallback),
                             fallback)

    def test_original_takes_precedence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / "MiSans.ttf"
            original.touch()
            self.assertEqual(generate_fonts.select_font(root, original.name,
                                                        root / "missing.otf"),
                             original)

    def test_no_font_has_actionable_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(FileNotFoundError, "download-fallback"):
                generate_fonts.select_font(root, "MiSans.ttf", root / "NotoSans.otf")

    def test_symbols_are_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "scene.c"
            source.write_text("猫，猫° hello 世界", encoding="utf-8")
            self.assertEqual(generate_fonts.collect_symbols([source]),
                             "".join(sorted(set("猫，°世界"))))


if __name__ == "__main__":
    unittest.main()
