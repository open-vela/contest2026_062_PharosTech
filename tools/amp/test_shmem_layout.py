#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep the two copies of the shared-region layout in step.

The arena layout is a contract between the openvela control domain and the
Linux compute domain, but the two sides live in trees that build separately, so
the constants are written out twice.  A disagreement would shift every slot and
corrupt audio silently rather than failing loudly, so this compares the copies
and fails on any drift.

Run from the repository root:  python3 tools/amp/test_shmem_layout.py
"""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# The two spellings of the same contract.
FIRMWARE = ROOT / "chips/rk3576/include/rk3576_shmem_layout.h"
COMPUTE = ROOT / "tools/amp/linux/nyamp_shmem_uapi.h"

# Names that must agree, as (firmware name, compute name).
SHARED_NAMES = (
    ("NYAMP_ARENA_MAGIC", "NYAMP_SHMEM_MAGIC"),
    ("NYAMP_ARENA_VERSION", "NYAMP_SHMEM_VERSION"),
    ("NYAMP_ARENA_MAGIC_OFFSET", "NYAMP_ARENA_MAGIC_OFFSET"),
    ("NYAMP_ARENA_VERSION_OFFSET", "NYAMP_ARENA_VERSION_OFFSET"),
    ("NYAMP_ARENA_SIZE_OFFSET", "NYAMP_ARENA_SIZE_OFFSET"),
    ("NYAMP_ARENA_GENERATION_OFFSET", "NYAMP_ARENA_GENERATION_OFFSET"),
    ("NYAMP_ARENA_TRACE_OFFSET", "NYAMP_ARENA_TRACE_OFFSET"),
    ("NYAMP_ARENA_TRACE_MAPPED", "NYAMP_ARENA_TRACE_MAPPED"),
    ("NYAMP_ARENA_TRACE_READY", "NYAMP_ARENA_TRACE_READY"),
)


def definitions(path):
    """Collect ``#define NAME VALUE`` as raw text, ignoring comments."""
    text = re.sub(r"/\*.*?\*/", " ", path.read_text(encoding="utf-8"),
                  flags=re.DOTALL)
    found = {}
    for line in text.splitlines():
        match = re.match(r"#define\s+(\w+)\s+(.+?)\s*$", line)
        if match:
            found[match.group(1)] = match.group(2)
    return found


class ShmemLayoutTest(unittest.TestCase):
    def test_both_headers_exist(self):
        self.assertTrue(FIRMWARE.is_file(), f"missing {FIRMWARE}")
        self.assertTrue(COMPUTE.is_file(), f"missing {COMPUTE}")

    def test_shared_values_agree(self):
        firmware = definitions(FIRMWARE)
        compute = definitions(COMPUTE)

        for firmware_name, compute_name in SHARED_NAMES:
            with self.subTest(name=firmware_name):
                self.assertIn(firmware_name, firmware,
                              f"{firmware_name} not defined in {FIRMWARE.name}")
                self.assertIn(compute_name, compute,
                              f"{compute_name} not defined in {COMPUTE.name}")

                # Strip the trailing comment before comparing.
                left = firmware[firmware_name].split("/*")[0].strip()
                right = compute[compute_name].split("/*")[0].strip()
                self.assertEqual(
                    left, right,
                    f"{firmware_name}={left} but {compute_name}={right}")

    def test_region_geometry_agrees(self):
        firmware = definitions(FIRMWARE)
        compute = definitions(COMPUTE)

        # The base and size are the contract the driver and the firmware both
        # act on; the compute side names them differently.
        for firmware_name, compute_name in (
            ("NYAMP_SHMEM_BASE", None),
            ("NYAMP_SHMEM_SIZE", None),
        ):
            self.assertIn(firmware_name, firmware)

        # The driver derives its size from the device tree rather than from a
        # constant, so only the firmware's declared size needs checking here.
        self.assertEqual(firmware["NYAMP_SHMEM_BASE"].strip(), "0x47c00000U")
        self.assertEqual(firmware["NYAMP_SHMEM_SIZE"].strip(),
                         "(4U * 1024U * 1024U)")
        self.assertNotIn("NYAMP_SHMEM_SIZE", compute,
                         "the compute side must take its size from the region, "
                         "not from a constant that could drift")


if __name__ == "__main__":
    unittest.main(verbosity=2)
