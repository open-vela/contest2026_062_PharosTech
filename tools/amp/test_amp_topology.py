#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Offline regression tests for the four-LITTLE/four-BIG artifact gate."""

import gzip
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from validate_amp_layout import parse_defconfig, validate_config, validate_dtb


class TopologyTest(unittest.TestCase):
    def test_board_reserves_optee(self):
        source = (Path(__file__).parent /
                  "linux/rk3576-kickpi-k7-amp.dtsi").read_text(encoding="utf-8")
        node = re.search(r"optee_reserved:\s*optee@48400000\s*\{([^}]+)\}",
                         source)
        self.assertIsNotNone(node, "BL32 memory must not enter the Linux allocator")
        body = node.group(1)
        self.assertRegex(body, r"reg\s*=\s*<0x0\s+0x48400000\s+0x0\s+0x1000000>;")
        self.assertRegex(body, r"\bno-map\s*;")
        self.assertNotRegex(body, r"\breusable\s*;")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def config(self, **changes):
        values = dict(SMP="y", SMP_NCPUS="4", NCPUS="4",
                      ARCH_HAVE_MULTICPU="y", ARCH_HAVE_IRQTRIGGER="y",
                      ARM64_GICV2_PREINITIALIZED="y", ARM64_GICV2_STATIC_SPI="y",
                      UART0_SERIAL_CONSOLE="y",
                      MM_REGIONS="1", RAM_START="0x4a400000",
                      RAM_SIZE="16777216", SMP_DEFAULT_CPUSET="0xf")
        values.update(changes)
        path = self.root / ".config"
        path.write_text("".join(f"CONFIG_{k}={v}\n" for k, v in values.items()))
        return path

    def test_config_accepts_four_little(self):
        validate_config(self.config())

    def test_savedefconfig_may_omit_default_cpu_count(self):
        path = self.root / "defconfig"
        base = ("CONFIG_RAM_START=0x4a400000\nCONFIG_RAM_SIZE=16777216\n"
                "CONFIG_SMP=y\nCONFIG_OPENAMP_CACHE=y\n")
        path.write_text(base)
        self.assertEqual(parse_defconfig(path), (0x4A400000, 16777216))
        path.write_text(base + "CONFIG_SMP_NCPUS=8\n")
        with self.assertRaises(ValueError):
            parse_defconfig(path)

    def test_config_rejects_unsafe_variants(self):
        for changes in (dict(SMP="n"), dict(NCPUS="1"), dict(SMP_NCPUS="8"),
                        dict(UP="y"), dict(ARCH_HAVE_MULTICPU="n"),
                        dict(ARCH_HAVE_IRQTRIGGER="n"), dict(MM_REGIONS="2"),
                        dict(RK3576_DMA_ALLOC="y"), dict(RAM_START="0x40200000"),
                        dict(RAM_SIZE="33554432"), dict(SMP_DEFAULT_CPUSET="0xff")):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate_config(self.config(**changes))

    def dtb(self, changes=None, missing=None, address_cells=2):
        if not shutil.which("dtc") or not shutil.which("fdtget"):
            self.skipTest("dtc and fdtget required")
        cpus = {0x100 + cpu: "okay" for cpu in range(4)}
        cpus.update(changes or {})
        if missing is not None:
            del cpus[missing]
        nodes = []
        for cpu, status in cpus.items():
            status_prop = f'status = "{status}";' if status else ""
            reg = f"0 {cpu}" if address_cells == 2 else str(cpu)
            nodes.append(f'cpu@{cpu:x} {{ device_type = "cpu"; '
                         'compatible = "arm,cortex-a72"; enable-method = "psci"; '
                         f'reg = <{reg}>; {status_prop} }};')
        source = self.root / "test.dts"
        result = self.root / "test.dtb"
        source.write_text('/dts-v1/; / { cpus { '
                          f'#address-cells = <{address_cells}>; #size-cells = <0>; '
                          + "".join(nodes) + '}; };')
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(result),
                        str(source)], check=True, capture_output=True)
        return result

    def test_dtb_accepts_four_big(self):
        for cells in (1, 2):
            validate_dtb(self.dtb(address_cells=cells))

    def test_dtb_accepts_default_enabled_big(self):
        validate_dtb(self.dtb({0x100: None, 0x101: "ok"}))

    def test_dtb_rejects_little_enabled(self):
        for status in ("okay", "disabled", None):
            with self.subTest(status=status), self.assertRaises(ValueError):
                validate_dtb(self.dtb({0: status}))

    def test_dtb_rejects_disabled_big(self):
        with self.assertRaises(ValueError):
            validate_dtb(self.dtb({0x100: "disabled"}))

    def test_dtb_rejects_missing_cpu(self):
        with self.assertRaises(ValueError):
            validate_dtb(self.dtb(missing=0x103))

    def test_dtb_rejects_extra_cpu(self):
        with self.assertRaises(ValueError):
            validate_dtb(self.dtb({0x104: "okay"}))

    def fit(self, config, dtb, firmware=None):
        if not shutil.which("mkimage"):
            self.skipTest("mkimage required")
        kernel = self.root / "Image"
        header = bytearray(bytes(56) + b"ARM\x64" + bytes(4))
        struct.pack_into("<Q", header, 16, 0x2000)
        kernel.write_bytes(header)
        ramdisk = self.root / "initramfs.gz"
        ramdisk.write_bytes(gzip.compress(b"fixture"))
        nuttx = self.root / "nuttx.bin"
        if firmware is None:
            firmware = header
        nuttx.write_bytes(firmware)
        output = self.root / "amp.itb"
        script = Path(__file__).parent / "nboot/build_amp_fit.sh"
        result = subprocess.run(["bash", str(script), str(kernel), str(dtb),
                                 str(ramdisk), str(nuttx), str(config), str(output)],
                                capture_output=True, text=True)
        return result, output

    def test_fit_declares_new_primary_cpus(self):
        result, output = self.fit(self.config(), self.dtb())
        self.assertEqual(result.returncode, 0, result.stderr)
        for node, cpu in (("linux", "100"), ("openvela", "0")):
            actual = subprocess.check_output(["fdtget", "-t", "x", str(output),
                                               f"/images/{node}", "cpu"], text=True)
            self.assertEqual(actual.strip(), cpu)

    def test_fit_rejects_old_config_before_output(self):
        result, output = self.fit(self.config(NCPUS="1"), self.dtb())
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_fit_rejects_old_dtb_before_output(self):
        result, output = self.fit(self.config(), self.dtb({0: "okay"}))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_fit_rejects_empty_firmware(self):
        result, output = self.fit(self.config(), self.dtb(), firmware=b"")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
