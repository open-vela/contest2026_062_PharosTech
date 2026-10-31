#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Preserve WAMR's effective context define across nested CMake scopes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def additions(name):
    text = (ROOT / 'patches' / name).read_text(encoding='utf-8')
    return '\n'.join(line[1:] for line in text.splitlines()
                     if line.startswith('+') and not line.startswith('+++'))


class ContextTest(unittest.TestCase):
    def build(self, context, patch):
        expected = '0' if context == 'absent' else context
        with tempfile.TemporaryDirectory(prefix='ny-wamr-context-') as directory:
            root = Path(directory)
            (root / 'check.c').write_text(
                '#if WASM_ENABLE_MODULE_INST_CONTEXT != ' + expected + '\n'
                '#error context changed\n#endif\nint main(void) { return 0; }\n')
            dependency = '' if context == 'absent' else (
                'set(WAMR_BUILD_MODULE_INST_CONTEXT ' + context + ')\n'
                'add_definitions(-DWASM_ENABLE_MODULE_INST_CONTEXT=' + context + ')\n')
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.16)\nproject(context C)\n'
                'function(wamr_dependency)\n' + dependency + 'endfunction()\n'
                'wamr_dependency()\n' + additions(patch) + '\n'
                'add_executable(check check.c)\n'
                'target_compile_options(check PRIVATE -Werror -Wundef)\n')
            configure = subprocess.run(['cmake', '-S', str(root), '-B', str(root/'out')],
                                       capture_output=True, text=True)
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
            return subprocess.run(['cmake', '--build', str(root/'out')],
                                  capture_output=True, text=True)

    def test_legacy_guard_redefines_nested_enabled_context(self):
        result = self.build('1', 'apps-wamr-context-default.patch')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('WASM_ENABLE_MODULE_INST_CONTEXT', result.stderr)

    def test_effective_definition_is_preserved(self):
        for context in ('absent', '0', '1'):
            with self.subTest(context=context):
                result = self.build(context, 'apps-wamr-context-owner.patch')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
