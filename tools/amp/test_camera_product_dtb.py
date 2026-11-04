#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the optional native-camera AMP ownership extension."""
import importlib.util
from pathlib import Path
import sys
import unittest
root=Path(__file__).resolve().parent
sys.path.insert(0,str(root/'linux'))
import amp_product_dtb as product
class CameraOwnership(unittest.TestCase):
 def test_preserves_existing_and_adds_camera(self):
  before='rockchip-amp { amp-irqs = <0x0 0x6c 0x0 0x80 0x0 0x0>; };'
  after=product.camera_irqs(before)
  self.assertIn('0x0 0x6c 0x0 0x80 0x0 0x0',after)
  self.assertIn('0x0 0x156 0x0 0x80 0x0 0x0',after)
  self.assertIn('0x0 0x15e 0x0 0x80 0x0 0x0',after)
  self.assertEqual(after,product.camera_irqs(after))
 def test_requires_typed_base_irq_table(self):
  with self.assertRaises(ValueError):product.camera_irqs('rockchip-amp {};')
  with self.assertRaises(ValueError):product.camera_irqs('amp-irqs = <0 108 128>;')
 def test_legacy_resources_remain_separate(self):
  self.assertNotIn(11,product.ALWAYS_ON_DOMAINS)
  self.assertNotIn(15,product.ALWAYS_ON_DOMAINS)
  self.assertEqual(product.CAMERA_DOMAINS,(11,15))
  self.assertNotIn('CLK_VEPU0_CORE',product.AMP_CLOCKS)
if __name__=='__main__':unittest.main()
