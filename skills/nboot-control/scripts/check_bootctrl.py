#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check a bootctrl image against the on-disk format.

This is a read-only shape audit.  It can tell you that a record is
well formed and self-consistent; it cannot tell you that the slot it
describes will actually boot.

Usage:
  check_bootctrl.py <bootctrl.img>
  check_bootctrl.py --json <bootctrl.img>
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"K7ABCTRL"
FORMAT_VERSION = 1
HEADER_SIZE = 20
RECORD_SIZE = 4096
COPY_COUNT = 2
CRC_OFFSET = 4092

HEADER = struct.Struct("<8sHHQ")
DOMAIN_HEADER = struct.Struct("<BBBB")
SLOT = struct.Struct("<BBBBQQ32s")
DOMAIN_NAMES = ("nuttx", "amp")

# Layout the bootloader insists on, from include/nboot_storage.h. A medium
# whose GPT does not match these starts and sizes is not selected at all,
# so bootctrl on such a medium is never consulted.
LAYOUT = (
    ("uboot", 0x4000, 8192),
    ("trust", 0x6000, 8192),
    ("bootctrl", 0x8000, 2048),
    ("nuttx_a", 0x9000, 131072),
    ("nuttx_b", 0x29000, 131072),
)


class RecordError(Exception):
    pass


def _slot(raw: bytes) -> dict:
    priority, tries, successful, _res, size, version, sha = SLOT.unpack(raw)
    return {
        "priority": priority,
        "tries_remaining": tries,
        "successful": successful,
        "image_size": size,
        "image_version": version,
        "sha256": sha.hex(),
        "bootable": priority != 0,
    }


def _domain(raw: bytes, name: str) -> dict:
    active, r1, r2, r3 = DOMAIN_HEADER.unpack(raw[:4])
    if active > 1:
        raise RecordError("%s active slot is invalid: %d" % (name, active))
    return {
        "name": name,
        "active_slot": "ab"[active],
        "slots": {
            "a": _slot(raw[4:56]),
            "b": _slot(raw[56:108]),
        },
    }


def parse(record: bytes) -> dict:
    if len(record) != RECORD_SIZE:
        raise RecordError("record is %d bytes, expected %d"
                          % (len(record), RECORD_SIZE))
    magic, version, header_size, generation = HEADER.unpack(
        record[: HEADER.size])
    if magic != MAGIC:
        raise RecordError("magic mismatch: %r" % magic)
    if version != FORMAT_VERSION:
        raise RecordError("unsupported format version %d" % version)
    if header_size != HEADER_SIZE:
        raise RecordError("header size is %d, expected %d"
                          % (header_size, HEADER_SIZE))
    body = record[:CRC_OFFSET]
    crc, = struct.unpack("<I", record[CRC_OFFSET:RECORD_SIZE])
    actual = zlib.crc32(body) & 0xFFFFFFFF
    if crc != actual:
        raise RecordError("CRC32 mismatch: stored 0x%08x computed 0x%08x"
                          % (crc, actual))
    request, = struct.unpack("<I", record[236:240])
    return {
        "generation": generation,
        "crc32": "0x%08x" % crc,
        "domains": [
            _domain(record[20:128], DOMAIN_NAMES[0]),
            _domain(record[128:236], DOMAIN_NAMES[1]),
        ],
        "request": "0x%08x" % request,
    }


def audit(image: Path) -> dict:
    blob = image.read_bytes()
    if len(blob) < RECORD_SIZE * COPY_COUNT:
        raise RecordError("image is shorter than two records (%d bytes)"
                          % len(blob))
    copies = []
    for index in range(COPY_COUNT):
        raw = blob[index * RECORD_SIZE:(index + 1) * RECORD_SIZE]
        try:
            copies.append({"copy": index, **parse(raw)})
        except RecordError as exc:
            copies.append({"copy": index, "error": str(exc)})

    valid = [c for c in copies if "error" not in c]
    selected = max(valid, key=lambda c: c["generation"]) if valid else None

    warnings = []

    # Both copies are written to be byte-identical; a mismatch is not
    # fatal (the newer generation wins) but it means an interrupted write
    # and is worth surfacing.
    if len(valid) == 2:
        first = blob[:RECORD_SIZE]
        second = blob[RECORD_SIZE:RECORD_SIZE * 2]
        if first != second:
            warnings.append(
                "the two copies differ (generation %d vs %d); a write was "
                "interrupted" % (valid[0]["generation"], valid[1]["generation"]))

    if selected is None:
        warnings.append("no valid copy; the device will refuse to boot")
    else:
        for domain in selected["domains"]:
            live = [s for s in domain["slots"].values() if s["bootable"]]
            if not live:
                warnings.append(
                    "domain %s has no bootable slot" % domain["name"])

    return {
        "image": str(image),
        "copies": copies,
        "selected_copy": selected["copy"] if selected else None,
        "warnings": warnings,
    }


def check_layout(device: Path, offset: int = 0) -> dict:
    """Audit the GPT against the layout the bootloader requires.

    A medium whose partitions do not match these exact starts and sizes is
    never selected, so any bootctrl it carries is irrelevant.  Reading the
    GPT out of a raw disk image (or the `dd`-ed partition region) is enough
    to tell whether the writer got the layout right.

    A whole disk starts with a protective MBR, so the GPT header lives in
    LBA 1; a region extracted with `dd` may start right at the header.  Both
    are accepted.
    """
    blob = device.read_bytes()
    if offset:
        blob = blob[offset:]
    if len(blob) < 0x4000:
        raise RecordError("too small to hold a GPT header")

    header_at = None
    for candidate in (0, 512):
        if blob[candidate:candidate + 8] == b"EFI PART":
            header_at = candidate
            break
    if header_at is None:
        raise RecordError("no GPT header at LBA 0 or LBA 1")

    header = blob[header_at:header_at + 92]
    entries_lba, = struct.unpack("<Q", header[72:80])
    count, entry_size = struct.unpack("<II", header[80:88])
    if entry_size < 128 or entry_size % 8 or count > 128:
        raise RecordError("implausible GPT entry geometry: %d x %d"
                          % (count, entry_size))

    found = {}
    for index in range(count):
        base = entries_lba * 512 + index * entry_size
        entry = blob[base:base + entry_size]
        if len(entry) < 128 or entry[:16] == bytes(16):
            continue
        start, end = struct.unpack("<QQ", entry[32:48])
        name = entry[56:128].decode("utf-16-le").rstrip("\x00")
        found[name] = (start, end - start + 1)

    problems = []
    for name, start, blocks in LAYOUT:
        actual = found.get(name)
        if actual is None:
            problems.append("partition %s is missing" % name)
        elif actual != (start, blocks):
            problems.append(
                "partition %s is at 0x%x/%d, bootloader requires 0x%x/%d"
                % (name, actual[0], actual[1], start, blocks))

    return {
        "device": str(device),
        "partitions": {n: {"start": s, "blocks": b}
                       for n, (s, b) in found.items()},
        "problems": problems,
    }


def selftest() -> int:
    """Build a record, corrupt it, and confirm the checks catch it."""

    def build(priority_a=15, priority_b=14):
        body = bytearray()
        body += HEADER.pack(MAGIC, FORMAT_VERSION, HEADER_SIZE, 7)
        for active, pa, pb in ((0, priority_a, priority_b), (0, 0, 0)):
            body += DOMAIN_HEADER.pack(active, 0, 0, 0)
            for priority in (pa, pb):
                body += SLOT.pack(priority, 0, 1 if priority else 0, 0,
                                  4752792, 1, bytes(range(32)))
        # Pad up to where the CRC lives, then append it. The record is
        # CRC_OFFSET bytes of content followed by a 4-byte checksum.
        body += bytes(CRC_OFFSET - len(body))
        body += struct.pack("<I", zlib.crc32(bytes(body)) & 0xFFFFFFFF)
        assert len(body) == RECORD_SIZE, len(body)
        return bytes(body)

    record = build()
    parsed = parse(record)
    assert parsed["generation"] == 7, parsed
    assert parsed["domains"][0]["slots"]["a"]["bootable"] is True
    assert parsed["domains"][1]["slots"]["a"]["bootable"] is False

    # A flipped byte in the payload region must fail the CRC.
    bad = bytearray(record)
    bad[100] ^= 0x01
    try:
        parse(bytes(bad))
    except RecordError as exc:
        assert "CRC" in str(exc), exc
    else:
        raise AssertionError("corrupt record passed the CRC check")

    # A bad magic must be caught before the CRC.
    bad = bytearray(record)
    bad[0:8] = b"XXXXXXXX"
    try:
        parse(bytes(bad))
    except RecordError as exc:
        assert "magic" in str(exc), exc
    else:
        raise AssertionError("bad magic passed")

    # An out-of-range active slot must be refused.
    bad = bytearray(record)
    bad[20] = 2
    bad[CRC_OFFSET:CRC_OFFSET + 4] = struct.pack(
        "<I", zlib.crc32(bytes(bad[:CRC_OFFSET])) & 0xFFFFFFFF)
    try:
        parse(bytes(bad))
    except RecordError as exc:
        assert "active slot" in str(exc), exc
    else:
        raise AssertionError("invalid active slot passed")

    print("SELFTEST_PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path, nargs="?")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--layout", action="store_true",
                    help="audit a disk image's GPT instead of a bootctrl file")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.image:
        ap.error("an image path is required (or --selftest)")

    if args.layout:
        try:
            result = check_layout(args.image)
        except (OSError, RecordError) as exc:
            print("bootctrl: %s" % exc, file=sys.stderr)
            return 1
        if args.json:
            print(json.dumps(result, indent=2))
        else:
            for name, part in sorted(result["partitions"].items(),
                                     key=lambda kv: kv[1]["start"]):
                print("  %-12s start=0x%-8x blocks=%d"
                      % (name, part["start"], part["blocks"]))
            for problem in result["problems"]:
                print("problem: %s" % problem)
            if not result["problems"]:
                print("layout matches the bootloader's expectation")
        return 1 if result["problems"] else 0

    try:
        result = audit(args.image)
    except (OSError, RecordError) as exc:
        print("bootctrl: %s" % exc, file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("%s: selected copy %s" % (result["image"], result["selected_copy"]))
        for domain in result["copies"][result["selected_copy"]]["domains"] \
                if result["selected_copy"] is not None else []:
            print("  domain %-5s active=%s" % (domain["name"], domain["active_slot"]))
            for side, slot in domain["slots"].items():
                print("    %s priority=%-3d tries=%-3d successful=%d size=%d v%d"
                      % (side, slot["priority"], slot["tries_remaining"],
                         slot["successful"], slot["image_size"],
                         slot["image_version"]))
        for warning in result["warnings"]:
            print("warning: %s" % warning)

    return 0 if result["selected_copy"] is not None else 1


if __name__ == "__main__":
    sys.exit(main())
