#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read-only record-shape audit; this cannot authenticate experimental claims."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import date
import json
from pathlib import Path
import re
import sys

HEADING = re.compile(r"^## \[(\d{4}-[^]]*)\]\s+(.+)$")
DATE_PREFIX = re.compile(r"^\d{4}-\d{2}-\d{2}(?!\d)")
FIELD = re.compile(r"^\s*-\s*(场景|操作|结果|结论|置信)[:：]\s*(.*)$")
FENCE = re.compile(r"^[ \t]*(`{3,}|~{3,})(.*)$")
REQUIRED = ("场景", "操作", "结果", "结论", "置信")
LEVEL = re.compile(r"^(实测确认|编译通过|仅推断)(?=$|[\s（(;；,，。])")
ALIAS = "编译+链接通过"


@dataclass
class Entry:
    line: int
    date: str
    title: str
    fields: dict[str, str]
    duplicates: list[str] = field(default_factory=list)


def parse(path: Path) -> list[Entry]:
    entries: list[Entry] = []
    current: Entry | None = None
    active_field: str | None = None
    fence_char = None
    fence_length = 0
    for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        fence = FENCE.match(line)
        if fence:
            marker = fence.group(1)
            if fence_char is None:
                fence_char, fence_length = marker[0], len(marker)
            elif (marker[0] == fence_char and len(marker) >= fence_length
                  and not fence.group(2).strip()):
                fence_char = None
            if current and active_field:
                current.fields[active_field] += "\n" + line
            continue
        if fence_char is not None:
            if current and active_field:
                current.fields[active_field] += "\n" + line
            continue
        heading = HEADING.match(line)
        if heading:
            label = heading.group(1)
            prefix = DATE_PREFIX.match(label)
            # Keep legacy suffixes such as " 续2", but do not drop malformed
            # dated records and report the remaining records as all valid.
            current = Entry(number, prefix.group(0) if prefix else label,
                            heading.group(2), {})
            entries.append(current)
            active_field = None
            continue
        if re.match(r"^#{1,2}\s", line):
            current = None
            active_field = None
            continue
        if current is None:
            continue
        match = FIELD.match(line)
        if match:
            name, value = match.groups()
            if name in current.fields:
                current.duplicates.append(f"{name} at line {number}")
                active_field = None
            else:
                active_field = name
                current.fields[name] = value.strip()
        elif active_field and line.strip():
            current.fields[active_field] += "\n" + line
    return entries


def audit(entries: list[Entry]) -> tuple[list[str], list[str]]:
    errors, warnings = [], []
    for entry in entries:
        try:
            date.fromisoformat(entry.date)
        except ValueError:
            errors.append(f"line {entry.line}: invalid date {entry.date}")
        for duplicate in entry.duplicates:
            errors.append(f"line {entry.line}: duplicate field {duplicate}")
        missing = [name for name in REQUIRED if not entry.fields.get(name, "").strip()]
        if missing:
            warnings.append(f"line {entry.line}: legacy/incomplete entry missing "
                            + ", ".join(missing))
        # Check confidence even if other fields are missing.
        raw = entry.fields.get("置信", "").strip()
        if not raw:
            continue
        confidence = raw.splitlines()[0].strip()
        if confidence.startswith(ALIAS):
            confidence = "编译通过" + confidence[len(ALIAS):]
            warnings.append(f"line {entry.line}: legacy confidence alias; "
                            "normalize only in a new derived record")
        match = LEVEL.match(confidence)
        if match is None:
            errors.append(f"line {entry.line}: invalid confidence {raw.splitlines()[0]!r}")
            continue
        # Flag separately asserted levels, never infer truth from result words.
        if re.search(r"[;；/＋+]\s*(?:启动路径)?(?:实测确认|编译通过|仅推断)",
                     confidence):
            warnings.append(f"line {entry.line}: mixed confidence; separate claims "
                            "for manual review")
    return errors, warnings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--strict", action="store_true",
                        help="Fail on incomplete records and legacy/mixed confidence.")
    parser.add_argument("--json", action="store_true", help="Emit a machine-readable report.")
    args = parser.parse_args()
    try:
        entries = parse(args.log)
    except (OSError, UnicodeError) as error:
        parser.error(str(error))
    if not entries:
        print("ERROR: no dated evidence entries found", file=sys.stderr)
        return 2
    errors, warnings = audit(entries)
    report = {
        "entries": len(entries),
        "complete": sum(all(entry.fields.get(name, "").strip() for name in REQUIRED)
                        for entry in entries),
        "errors": errors, "warnings": warnings,
        "scope": "Record syntax only; hardware execution and claim truth require manual review.",
    }
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(f"entries={report['entries']} complete={report['complete']} "
              f"errors={len(errors)} warnings={len(warnings)}")
        for severity, messages in (("ERROR", errors), ("WARN", warnings)):
            for message in messages:
                print(f"{severity}: {message}")
    return int(bool(errors or (args.strict and warnings)))


if __name__ == "__main__":
    raise SystemExit(main())
