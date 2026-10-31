#!/usr/bin/env python3
# tools/tests/test_dependency_patches.py
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

"""An applied patch and a conflicting patch are different outcomes."""
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'apply_dependency_patches.sh'


class PatchTest(unittest.TestCase):
    def test_overlapping_stack_is_repeatable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'apps').mkdir()
            agent = root / 'packages/ai_agent'
            agent.mkdir(parents=True)
            patches = root / 'patches'
            patches.mkdir()
            target = agent / 'test.c'
            target.write_text('base\n')
            for name, old, new in [('a', 'base', 'adapter'),
                                   ('b', 'adapter', 'transport')]:
                (patches / f'packages-ai_agent-{name}.patch').write_text(
                    f'--- a/test.c\n+++ b/test.c\n@@ -1 +1 @@\n-{old}\n+{new}\n')
            command = ['bash', str(SCRIPT), str(root), str(patches)]
            for _ in range(2):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(target.read_text(), 'transport\n')
            # Adding another dependency fix must not mistake the installed
            # overlapping prefix for a conflict.
            (patches / 'packages-ai_agent-c.patch').write_text(
                '--- a/extra.c\n+++ b/extra.c\n@@ -1 +1 @@\n-old\n+new\n')
            (agent / 'extra.c').write_text('old\n')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((agent / 'extra.c').read_text(), 'new\n')
            target.write_text('conflicting transport\n')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(target.read_text(), 'conflicting transport\n')

    def test_overlapping_apps_stack_is_repeatable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            apps = root / 'apps'
            apps.mkdir()
            patches = root / 'patches'
            patches.mkdir()
            target = apps / 'CMakeLists.txt'
            target.write_text('base\n')
            for name, old, new in [('a', 'base', 'default'),
                                   ('b', 'default', 'owned')]:
                (patches / f'apps-{name}.patch').write_text(
                    '--- a/CMakeLists.txt\n+++ b/CMakeLists.txt\n'
                    f'@@ -1 +1 @@\n-{old}\n+{new}\n')
            command = ['bash', str(SCRIPT), str(root), str(patches)]
            for _ in range(2):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(target.read_text(), 'owned\n')
            target.write_text('unrelated local edit\n')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(target.read_text(), 'unrelated local edit\n')

    def test_apply_repeat_and_conflict(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'apps').mkdir()
            patches = root / 'patches'
            patches.mkdir()
            target = root / 'apps/example.c'
            target.write_text('before\n')
            (patches / 'apps-test.patch').write_text(
                '--- a/example.c\n+++ b/example.c\n@@ -1 +1 @@\n-before\n+after\n')
            command = ['bash', str(SCRIPT), str(root), str(patches)]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(target.read_text(), 'after\n')
            again = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(again.returncode, 0, again.stderr)
            self.assertIn('already applied', again.stdout)
            target.write_text('unrelated local edit\n')
            conflict = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(conflict.returncode, 0)
            self.assertIn('conflicts', conflict.stderr)
            self.assertEqual(target.read_text(), 'unrelated local edit\n')


if __name__ == '__main__':
    unittest.main()
