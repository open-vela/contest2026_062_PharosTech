#!/usr/bin/env python3
# tools/nyabula_core/tests/audio_stream_config_test.py
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

"""Check audio stream feature guards without requiring a target audio driver."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
args, remaining = parser.parse_known_args()
CORE = args.core.resolve()


class AudioStreamConfigTest(unittest.TestCase):
    def check_config(self, feature):
        with tempfile.TemporaryDirectory(prefix='ny-audio-config-') as directory:
            root = Path(directory)
            (root / 'nuttx/audio').mkdir(parents=True)
            (root / 'netutils').mkdir()
            (root / 'nuttx/config.h').write_text(
                f'#define {feature} 1\n' if feature else '', encoding='utf-8')
            # Enabled cases inspect preprocessing only; the full target build
            # supplies the real NuttX audio ABI. No fake driver is linked here.
            (root / 'nuttx/audio/audio.h').write_text('', encoding='utf-8')
            (root / 'netutils/cJSON.h').write_text(
                'typedef struct cJSON cJSON;\n', encoding='utf-8')
            command = ['cc', '-std=c11', '-I' + str(root),
                       '-I' + str(CORE.parent / 'nyabula_dsp'),
                       str(CORE / 'ny_audio_stream.c')]
            result = subprocess.run(command + ['-E', '-P'], capture_output=True,
                                    text=True, check=True)
            for name in ('open', 'read', 'write', 'drain', 'pause', 'close'):
                definition = re.search(
                    r'\bny_audio_stream_' + name + r'\([^;]*?\)\s*\{',
                    result.stdout)
                self.assertEqual(bool(definition), bool(feature), name)
            if feature is None:
                # Disabled builds must remain valid C, not expose drain/pause
                # after the private stream type and audio includes disappear.
                subprocess.run(command + ['-fsyntax-only', '-Wall', '-Werror'],
                               check=True, capture_output=True, text=True)

    def test_disabled(self):
        self.check_config(None)

    def test_bluetooth_only(self):
        self.check_config('CONFIG_NYABULA_CORE_BT')

    def test_dsp_only(self):
        self.check_config('CONFIG_NYABULA_CORE_DSP')

    def test_intercom_only(self):
        self.check_config('CONFIG_NYABULA_CORE_INTERCOM')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
