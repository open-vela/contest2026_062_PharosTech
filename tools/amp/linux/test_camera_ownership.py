#!/usr/bin/env python3
# tools/amp/linux/test_camera_ownership.py
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to you under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Camera handoff gates, independent of a board and vendor SDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from camera_ownership import (config_check, config_disable, dtb_check,
                              dtb_disable, property_value)


class OwnershipTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_seed_cannot_reenable_camera(self):
        config = self.root / '.config'
        config.write_text('CONFIG_VIDEO_OV5647=y\nCONFIG_VIDEO_ROCKCHIP_CIF=m\n'
                          'CONFIG_ROCKCHIP_RKNPU=y\nCONFIG_VIDEO_ROCKCHIP_RGA=y\n')
        with self.assertRaises(ValueError):
            config_check(config)
        config_disable(config)
        first = config.read_text()
        config_disable(config)
        self.assertEqual(first, config.read_text())
        config_check(config)
        self.assertIn('CONFIG_ROCKCHIP_RKNPU=y', first)
        self.assertIn('CONFIG_VIDEO_ROCKCHIP_RGA=y', first)
        config.write_text(first + 'CONFIG_PHY_ROCKCHIP_SAMSUNG_DCPHY=y\n')
        with self.assertRaises(ValueError):
            config_check(config)

    def dtb(self):
        if not all(shutil.which(x) for x in ('dtc', 'fdtget', 'fdtput')):
            self.skipTest('dtc/fdtget/fdtput required')
        source = self.root / 'input.dts'
        source.write_text('''/dts-v1/;
        / {
          csi { compatible = "rockchip,rk3576-csi2-dphy"; };
          i2c@2ac70000 { status = "okay"; };
          other-i2c {
            sensor { compatible = "vendor,other-sensor";
                     rockchip,camera-module-index = <0>; status = "okay"; };
          };
          npu { compatible = "rockchip,rk3576-rknpu"; status = "okay"; };
          rga { compatible = "rockchip,rk3576-rga"; status = "okay"; };
        };''')
        dtb = self.root / 'input.dtb'
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(dtb),
                        str(source)], check=True, capture_output=True)
        return dtb

    def test_dtb_preserves_compute_and_source(self):
        source = self.dtb()
        original = source.read_bytes()
        with self.assertRaises(ValueError):
            dtb_check(source)
        output = self.root / 'owned.dtb'
        nodes = dtb_disable(source, output)
        self.assertEqual(len(nodes), 3)
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(dtb_check(output), nodes)
        for node in ('/npu', '/rga'):
            self.assertEqual(property_value(output, node, 'status'), 'okay')
        subprocess.run(['fdtput', '-t', 's', str(output), '/csi',
                        'status', 'okay'], check=True)
        with self.assertRaises(ValueError):
            dtb_check(output)

    def test_dtb_requires_separate_output(self):
        source = self.dtb()
        with self.assertRaises(ValueError):
            dtb_disable(source, source)


if __name__ == '__main__':
    unittest.main()
