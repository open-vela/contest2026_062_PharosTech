#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the checked-in dependency inventory without downloading assets."""

import json
from pathlib import Path
import re
import unittest

from nyamp_bundle import nyamp_relative


class NyampCatalogTest(unittest.TestCase):
    def test_inventory(self):
        profile = json.loads((Path(__file__).parent / "dependencies.json").read_text(encoding="utf-8"))
        self.assertEqual(profile["schema_version"], 1)
        self.assertEqual(profile["interface_version"], 1)
        entries = {entry["id"]: entry for entry in profile["dependencies"]}
        self.assertEqual(len(entries), len(profile["dependencies"]))
        paths = set()
        for entry in entries.values():
            self.assertNotEqual(entry["version"], "latest")
            self.assertTrue(entry["source"].startswith("https://"))
            self.assertIn(entry["redistribution"], ("approved", "review-required"))
            if entry["redistribution"] == "review-required":
                self.assertTrue(entry["release_blockers"])
            else:
                self.assertFalse(entry["release_blockers"])
            for dependency in entry["requires"]:
                self.assertIn(dependency, entries)
            for asset in entry["files"]:
                nyamp_relative(asset["path"])
                self.assertNotIn(asset["path"], paths)
                paths.add(asset["path"])
                self.assertGreater(asset["bytes"], 0)
                self.assertIsNotNone(re.fullmatch(r"[a-f0-9]{64}", asset["sha256"]))
        visited, active = set(), set()

        def visit(name):
            self.assertNotIn(name, active, "dependency cycle")
            if name in visited:
                return
            active.add(name)
            for dependency in entries[name]["requires"]:
                visit(dependency)
            active.remove(name)
            visited.add(name)

        for name in entries:
            visit(name)


if __name__ == "__main__":
    unittest.main()
