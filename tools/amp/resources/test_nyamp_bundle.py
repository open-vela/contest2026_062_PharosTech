#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Offline bundle tests; fixtures are not model weights."""

import hashlib
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

from nyamp_bundle import nyamp_compatible, nyamp_manifest, nyamp_pack


class NyampBundleTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name) / "resource"
        (self.root / "models").mkdir(parents=True)
        (self.root / "licenses").mkdir()
        files = []
        for name, data in [("models/fixture.bin", b"not-a-model"),
                           ("licenses/fixture.txt", b"Test fixture, no third-party data.\n")]:
            (self.root / name).write_bytes(data)
            files.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        self.manifest = dict(schema_version=1, bundle_id="nyamp-fixture", version="1.0.0",
                             kind="fixture", redistribution="approved",
                             compatibility=dict(soc="host", architecture="native", os="host",
                                                interface_version=1, dependencies={}),
                             provenance=dict(source="local-test", revision="1", conversion="none"),
                             license_files=["licenses/fixture.txt"], files=files)
        self.save()

    def save(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def test_verify(self):
        manifest, files = nyamp_manifest(self.root)
        self.assertEqual(manifest["bundle_id"], "nyamp-fixture")
        self.assertEqual(len(files), 2)

    def test_reproducible_pack(self):
        first = Path(self.directory.name) / "first.tar.gz"
        second = Path(self.directory.name) / "second.tar.gz"
        self.assertEqual(nyamp_pack(self.root, first), nyamp_pack(self.root, second))
        with tarfile.open(first) as archive:
            self.assertEqual(archive.getnames(), ["manifest.json", "licenses/fixture.txt", "models/fixture.bin"])
            self.assertEqual(archive.extractfile("models/fixture.bin").read(), b"not-a-model")

    def test_no_overwrite(self):
        output = Path(self.directory.name) / "existing"
        output.write_bytes(b"keep")
        with self.assertRaises(ValueError):
            nyamp_pack(self.root, output)
        self.assertEqual(output.read_bytes(), b"keep")

    def test_bad_digest(self):
        (self.root / "models/fixture.bin").write_bytes(b"wrong-bytes")
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_unapproved(self):
        self.manifest["redistribution"] = "review-required"
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_paths(self):
        for path in ("../secret", "/tmp/file", "C:/secret", "models\\file", "models//file", "models/./file"):
            self.manifest["files"][0]["path"] = path
            self.save()
            with self.subTest(path=path), self.assertRaises(ValueError):
                nyamp_manifest(self.root)

    def test_duplicate(self):
        self.manifest["files"].append(self.manifest["files"][0])
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_missing_license(self):
        self.manifest["license_files"] = []
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_unpinned_dependency(self):
        self.manifest["compatibility"]["dependencies"] = {"runtime": "latest"}
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_missing_source_revision(self):
        self.manifest["provenance"]["revision"] = ""
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_symlink(self):
        source = self.root / "models/fixture.bin"
        target = Path(self.directory.name) / "outside"
        target.write_bytes(source.read_bytes())
        source.unlink()
        try:
            source.symlink_to(target)
        except OSError:
            self.skipTest("symlinks unavailable")
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_wrong_target(self):
        profile = dict(target=dict(soc="rk3576", architecture="aarch64", os="linux"),
                       interface_version=1, dependencies=[])
        with self.assertRaises(ValueError):
            nyamp_compatible(self.manifest, profile)

    def test_wrong_runtime(self):
        profile = dict(target=dict(soc="host", architecture="native", os="host"),
                       interface_version=1, dependencies=[dict(id="ort", version="1.28.2")])
        self.manifest["compatibility"]["dependencies"] = {"ort": "1.0"}
        with self.assertRaises(ValueError):
            nyamp_compatible(self.manifest, profile)
        self.manifest["compatibility"]["dependencies"]["ort"] = "1.28.2"
        nyamp_compatible(self.manifest, profile)

    def test_model_requires_profile(self):
        self.manifest["kind"] = "model"
        self.save()
        with self.assertRaises(ValueError):
            nyamp_pack(self.root, Path(self.directory.name) / "model.tar.gz")

    # --- firmware and boot bundles ---------------------------------------

    def as_firmware(self, install):
        """Turn the fixture into a firmware bundle installing one file."""
        self.manifest["kind"] = "firmware"
        self.manifest["files"] = [self.manifest["files"][0]]
        self.manifest["files"][0]["install"] = install
        self.manifest["license_files"] = ["licenses/fixture.txt"]
        self.manifest["files"].append(dict(
            path="licenses/fixture.txt",
            bytes=len(b"Test fixture, no third-party data.\n"),
            sha256=hashlib.sha256(b"Test fixture, no third-party data.\n").hexdigest()))
        self.save()

    def test_firmware_needs_no_profile(self):
        # A partition payload has no model interface to be compatible with,
        # so packing it must not demand a dependencies.json.
        self.as_firmware(dict(partition="nuttx_a"))
        output = Path(self.directory.name) / "fw.tar.gz"
        nyamp_pack(self.root, output)
        self.assertTrue(output.exists())

    def test_firmware_requires_install(self):
        self.manifest["kind"] = "firmware"
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_install_rejects_both_forms(self):
        # partition and lba together would force the board to choose, and
        # choosing wrong writes over something else.
        self.as_firmware(dict(partition="nuttx_a", lba=36864, sectors=16384))
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_install_rejects_neither_form(self):
        self.as_firmware({})
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_install_rejects_unknown_partition(self):
        # A name nbootctl does not know would fail on the board, after the
        # whole file had already been transferred.
        self.as_firmware(dict(partition="rootfs"))
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_install_accepts_raw_window(self):
        self.as_firmware(dict(lba=64, sectors=4096))
        manifest, _ = nyamp_manifest(self.root)
        self.assertEqual(manifest["files"][0]["install"]["lba"], 64)

    def test_install_raw_window_must_fit(self):
        # 12 bytes cannot fit in one sector alongside... it can, so use a
        # window smaller than the payload needs.
        self.as_firmware(dict(lba=64, sectors=1))
        (self.root / "models/fixture.bin").write_bytes(b"x" * 4096)
        self.manifest["files"][0]["bytes"] = 4096
        self.manifest["files"][0]["sha256"] = hashlib.sha256(b"x" * 4096).hexdigest()
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_install_only_for_partition_kinds(self):
        self.manifest["files"][0]["install"] = dict(partition="nuttx_a")
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)

    def test_boot_kind(self):
        self.as_firmware(dict(partition="uboot"))
        self.manifest["kind"] = "boot"
        self.save()
        manifest, _ = nyamp_manifest(self.root)
        self.assertEqual(manifest["kind"], "boot")

    # --- licence gate -----------------------------------------------------

    def test_unapproved_requires_explicit_flag(self):
        self.manifest["redistribution"] = "review-required"
        self.save()
        with self.assertRaises(ValueError):
            nyamp_manifest(self.root)
        # The flag has to be passed deliberately; it is the only way past.
        manifest, _ = nyamp_manifest(self.root, allow_review_required=True)
        self.assertEqual(manifest["redistribution"], "review-required")

    def test_unapproved_pack_requires_explicit_flag(self):
        self.manifest["redistribution"] = "review-required"
        self.save()
        output = Path(self.directory.name) / "unapproved.tar.gz"
        with self.assertRaises(ValueError):
            nyamp_pack(self.root, output)
        self.assertFalse(output.exists())
        nyamp_pack(self.root, output, allow_review_required=True)
        self.assertTrue(output.exists())


if __name__ == "__main__":
    unittest.main()
