#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep the product entry point out of the reusable Display module."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


class DisplayOwnershipTest(unittest.TestCase):
    def test_display_has_no_eye_or_core_dependency(self):
        forbidden = re.compile(r"nyabula_(?:eye|core)|NYABULA_(?:EYE|CORE|DISPLAY_EYES)")
        for path in (ROOT / "app/nyabula_display").rglob("*"):
            if path.is_file() and (path.suffix in (".c", ".h", ".cpp", ".cxx") or
                                   path.name in ("Kconfig", "Makefile", "Make.defs",
                                                 "CMakeLists.txt")):
                with self.subTest(path=path.relative_to(ROOT)):
                    self.assertIsNone(forbidden.search(path.read_text(encoding="utf-8")))

    def test_eye_uses_public_display_api_not_demo(self):
        entry = (ROOT / "app/nyabula/src/nyabula_eye_main.c").read_text(encoding="utf-8")
        self.assertIn('#include "nyabula_display.h"', entry)
        self.assertIn("nyabula_display_init(", entry)
        self.assertIn("nyabula_display_task(", entry)
        self.assertIn("nyabula_eye_service_attach(", entry)
        self.assertNotIn("nyabula_display_main(", entry)
        self.assertNotIn("nyabula_dual_demo", entry)


if __name__ == "__main__":
    unittest.main()
