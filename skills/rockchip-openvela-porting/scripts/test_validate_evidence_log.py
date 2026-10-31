#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Focused tests for validate_evidence_log.py."""

from pathlib import Path
import contextlib
import io
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

import validate_evidence_log as validator


class EvidenceValidatorTest(unittest.TestCase):
    def inspect_text(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'log.md'
            path.write_text(text, encoding='utf-8')
            entries = validator.parse(path)
        return entries, validator.audit(entries)

    def complete(self, confidence='编译通过'):
        return ('## [2026-09-16] Build\n- 场景: host\n- 操作: make\n'
                '- 结果: exit code 0\n- 结论: host build finished\n'
                f'- 置信: {confidence}\n')

    def test_no_keyword_requirement_for_results(self):
        _, (errors, warnings) = self.inspect_text(self.complete('实测确认'))
        self.assertEqual((errors, warnings), ([], []))

    def test_negated_label_cannot_be_promoted(self):
        _, (errors, _) = self.inspect_text(self.complete('未实测确认'))
        self.assertEqual(len(errors), 1)

    def test_code_block_does_not_create_records_or_fields(self):
        text = self.complete().replace('- 结果: exit code 0',
            '- 结果:\n```text\n## [2026-09-17] quoted\n- 置信: 仅推断\n```')
        entries, (errors, warnings) = self.inspect_text(text)
        self.assertEqual(len(entries), 1)
        self.assertEqual((errors, warnings), ([], []))
        self.assertIn('quoted', entries[0].fields['结果'])

    def test_duplicate_field_is_rejected(self):
        entries, (errors, _) = self.inspect_text(self.complete() + '- 置信: 实测确认\n')
        self.assertEqual(entries[0].fields['置信'], '编译通过')
        self.assertTrue(any('duplicate field' in item for item in errors))

    def test_fence_with_info_does_not_close_code(self):
        text = self.complete().replace('- 结果: exit code 0',
            '- 结果:\n```text\n```still quoted\n- 置信: invalid\n```')
        entries, (errors, warnings) = self.inspect_text(text)
        self.assertEqual((errors, warnings), ([], []))
        self.assertIn('- 置信: invalid', entries[0].fields['结果'])

    def test_non_padded_date_is_not_silently_dropped(self):
        text = self.complete() + self.complete().replace('2026-09-16', '2026-9-17')
        entries, (errors, _) = self.inspect_text(text)
        self.assertEqual(len(entries), 2)
        self.assertTrue(any('invalid date' in item for item in errors))

    def test_legacy_date_suffix_preserved_as_valid_date(self):
        entries, (errors, warnings) = self.inspect_text(
            self.complete().replace('2026-09-16', '2026-09-16 续2'))
        self.assertEqual(entries[0].date, '2026-09-16')
        self.assertEqual((errors, warnings), ([], []))

    def test_long_fence_does_not_close_on_short_fence(self):
        text = self.complete().replace('- 结果: exit code 0',
            '- 结果:\n````text\n```\n- 置信: invalid\n````')
        entries, (errors, warnings) = self.inspect_text(text)
        self.assertEqual((errors, warnings), ([], []))
        self.assertIn('- 置信: invalid', entries[0].fields['结果'])

    def test_invalid_calendar_date(self):
        _, (errors, _) = self.inspect_text(self.complete().replace('2026-09-16', '2026-02-30'))
        self.assertTrue(any('invalid date' in item for item in errors))

    def test_other_section_ends_record(self):
        entries, (errors, warnings) = self.inspect_text(
            self.complete() + '## Other section\n- 置信: invalid\n')
        self.assertEqual((errors, warnings), ([], []))
        self.assertEqual(entries[0].fields['置信'], '编译通过')

    def test_incomplete_record_still_checks_invalid_confidence(self):
        _, (errors, warnings) = self.inspect_text('## [2026-09-16] Partial\n- 置信: nope\n')
        self.assertTrue(errors)
        self.assertTrue(warnings)

    def test_bom_and_fullwidth_colon(self):
        entries, result = self.inspect_text('\ufeff' + self.complete().replace(':', '：'))
        self.assertEqual(len(entries), 1)
        self.assertEqual(result, ([], []))

    def test_cli_strict_json_and_readonly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'log.md'
            path.write_text('## [2026-09-16] Partial\n- 结果: timeout\n', encoding='utf-8')
            original = path.read_bytes()
            output = io.StringIO()
            with patch.object(sys, 'argv', ['audit', str(path), '--strict', '--json']), contextlib.redirect_stdout(output):
                status = validator.main()
            self.assertEqual(status, 1)
            self.assertEqual(json.loads(output.getvalue())['complete'], 0)
            self.assertEqual(path.read_bytes(), original)

    def test_complete_entry(self):
        text = """## [2026-09-16] UART output
- 场景: M2 board revision A
- 操作: flash image and capture UART
- 结果: 原文输出 NuttShell (NSH)
- 结论: proves UART and NSH for this image; 未证明 SMP
- 置信: 实测确认
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.md"
            path.write_text(text, encoding="utf-8")
            errors, warnings = validator.audit(validator.parse(path))
        self.assertEqual(errors, [])
        self.assertEqual(warnings, [])

    def test_invalid_confidence(self):
        text = """## [2026-09-16] Build
- 场景: build
- 操作: make
- 结果: build passed
- 结论: 不代表板测
- 置信: 大概可用
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.md"
            path.write_text(text, encoding="utf-8")
            errors, _ = validator.audit(validator.parse(path))
        self.assertEqual(len(errors), 1)

    def test_compile_and_link_alias(self):
        text = """## [2026-09-16] Link result
- 场景: target link
- 操作: build target
- 结果: link passed
- 结论: 不代表板测
- 置信: 编译+链接通过（未上板）
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.md"
            path.write_text(text, encoding="utf-8")
            errors, _ = validator.audit(validator.parse(path))
        self.assertEqual(errors, [])

    def test_legacy_entry_is_warning(self):
        text = """## [2026-09-16] Old note
- 结果: passed
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.md"
            path.write_text(text, encoding="utf-8")
            errors, warnings = validator.audit(validator.parse(path))
        self.assertEqual(errors, [])
        self.assertTrue(any("legacy/incomplete" in item for item in warnings))

    def test_qualified_and_mixed_confidence(self):
        text = """## [2026-09-16] Mixed result
- 场景: target build and board smoke
- 操作: build, flash, boot
- 结果: build passed; 原文输出 NSH
- 结论: board boot verified; 未证明 long-term stability
- 置信: 编译通过；启动路径实测确认
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log.md"
            path.write_text(text, encoding="utf-8")
            errors, warnings = validator.audit(validator.parse(path))
        self.assertEqual(errors, [])
        self.assertTrue(any("mixed confidence" in item for item in warnings))


if __name__ == "__main__":
    unittest.main()
