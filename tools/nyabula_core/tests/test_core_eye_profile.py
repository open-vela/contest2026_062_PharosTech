#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep the K7 Eye profile aligned with its physical panel wiring."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


class CoreEyeProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lines = (ROOT / "configs/core_eye/defconfig").read_text(encoding="utf-8").splitlines()
        cls.config = dict(line.split("=", 1) for line in lines if line.startswith("CONFIG_"))

    def test_physical_te_gpio(self):
        self.assertEqual(self.config.get("CONFIG_NYABULA_DISPLAY_TE_GPIO"), "y")
        self.assertNotEqual(self.config.get("CONFIG_NYABULA_DISPLAY_TE_SW"), "y")
        self.assertEqual(self.config.get("CONFIG_DEV_GPIO"), "y")
        self.assertEqual(self.config.get("CONFIG_NYABULA_DISPLAY_TE0_DEVPATH"), '"/dev/gpio1"')
        self.assertEqual(self.config.get("CONFIG_NYABULA_DISPLAY_TE1_DEVPATH"), '"/dev/gpio2"')

    def test_gc9b72_initialization(self):
        self.assertEqual(self.config.get("CONFIG_LCD_ST77916"), "y")
        self.assertEqual(self.config.get("CONFIG_LCD_ST77916_CHIP_TYPE_GC9B72"), "y")
        self.assertNotEqual(self.config.get("CONFIG_LCD_ST77916_CHIP_TYPE_ST77916"), "y")


if __name__ == "__main__":
    unittest.main()
