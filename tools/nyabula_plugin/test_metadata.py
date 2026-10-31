#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import json
import tempfile
import unittest
from pathlib import Path
import nyabula_plugin as sdk

class MetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
        self.manifest={'id':'hardware-demo','name':'硬件小助手','version':'1.0.0','runtime':'quickjs','entry':'main.js','apiVersion':1,'permissions':['audio.read','audio.capture','bluetooth.read','bluetooth.connect','bluetooth.call','usb.access','camera.capture']}
    def tearDown(self): self.temp.cleanup()
    def validate(self,icon):
        self.manifest['icon']=icon
        path=self.root/'manifest.json';path.write_text(json.dumps(self.manifest),encoding='utf-8')
        return sdk.validate_manifest(path)
    def test_valid(self):
        for icon in ['pets',{'type':'image','path':'assets/icon.png'},{'type':'iconfont','path':'assets/icons.woff2','codepoint':'e601'}]:
            self.assertEqual(self.validate(icon)['icon'],icon)
    def test_invalid(self):
        for icon in [None,'',{},'https://icon',{'type':'image','path':'assets/../x'},{'type':'image','path':'assets//x'},{'type':'image','path':'assets/x/'},{'type':'iconfont','path':'assets/icon.woff2','codepoint':'d800'}]:
            with self.assertRaises(sdk.PluginError): self.validate(icon)
    def test_asset_validation(self):
        manifest=self.validate({'type':'image','path':'assets/icon.png'})
        with self.assertRaises(sdk.PluginError): sdk._validate_icon_asset(manifest,{})
        with self.assertRaises(sdk.PluginError): sdk._validate_icon_asset(manifest,{'assets/icon.png':b'<svg>unsafe document</svg>'})
        with self.assertRaises(sdk.PluginError): sdk._validate_icon_asset(manifest,{'assets/icon.png':b'x'*16385})
        sdk._validate_icon_asset(manifest,{'assets/icon.png':b'\x89PNG\r\n\x1a\n'+bytes(8)})
    def test_native_permissions_match(self):
        import re
        native=Path(__file__).resolve().parents[2]/'app/nyabula_core/ny_manifest.c'
        names=set(re.findall(r'\{ "([a-z.]+)", NY_PERMISSION_',native.read_text(encoding='utf-8')))
        self.assertEqual(sdk.KNOWN_PERMISSIONS,names)
if __name__=='__main__': unittest.main()
