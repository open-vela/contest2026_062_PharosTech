#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate and reproducibly pack a local Nyabula resource directory.

Two families of bundle share one manifest format:

  model / runtime / fixture   a compute-domain resource.  Needs a target
                              profile, because a model only means anything
                              against a specific runtime.

  firmware / boot             a payload for a partition.  Needs no profile:
                              the compatibility that matters is the board,
                              not a model interface.  What it does need is
                              an `install` field per file, because a
                              firmware blob is only useful together with
                              where it goes.

Licensing is gated the same way for all of them.  A bundle whose
`redistribution` is not `approved` is refused unless the caller passes
--allow-review-required, which exists so internal test images can be
built without pretending their paperwork is finished.  The flag is
deliberately explicit: it should be obvious in a command line that
something is being packed ahead of its licence review.
"""

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import tarfile


BUNDLE_KINDS = ("model", "runtime", "fixture", "firmware", "boot")

# Kinds that describe a compute resource and therefore must declare which
# runtime they were built against.
PROFILE_KINDS = ("model", "runtime")

# Kinds that land on a partition, and so must say where.
INSTALL_KINDS = ("firmware", "boot")

# The partition names nbootctl accepts.  Kept in step with the table in
# app/nbootctl/nbootctl_part.c by tools/amp/resources/test_nyamp_bundle.py,
# which reads both.  A name that passes here but not there would let a
# bundle be built whose write the board then refuses, after the whole file
# has been transferred -- cheap to prevent, annoying to discover.
NBOOTCTL_PARTITIONS = (
    "uboot", "trust", "bootctrl",
    "nuttx_a", "nuttx_b", "amp_a", "amp_b", "data",
)


def nyamp_relative(value):
    """Accept portable package paths, never device or parent paths."""
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("invalid package path")
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("invalid package path component")
    if any(not re.fullmatch(r"[A-Za-z0-9_.+-]+", part) for part in parts):
        raise ValueError("package paths must use portable ASCII names")
    return PurePosixPath(value)


def nyamp_file(root, name):
    relative = nyamp_relative(name)
    candidate = root
    for part in relative.parts:
        candidate = candidate / part
        if candidate.is_symlink():
            raise ValueError(f"symlink is not a package file: {name}")
    if not candidate.is_file() or not candidate.resolve().is_relative_to(root):
        raise ValueError(f"missing regular package file: {name}")
    return candidate


def nyamp_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def nyamp_compatible(manifest, profile):
    compatibility = manifest["compatibility"]
    for field in ("soc", "architecture", "os"):
        if compatibility[field] != profile["target"][field]:
            raise ValueError(f"incompatible target {field}")
    if compatibility["interface_version"] != profile["interface_version"]:
        raise ValueError("incompatible model interface")
    versions = {entry["id"]: entry["version"] for entry in profile["dependencies"]}
    for name, version in compatibility["dependencies"].items():
        if versions.get(name) != version:
            raise ValueError(f"incompatible dependency: {name}")


def nyamp_install(entry, name, kind):
    """Validate the `install` record that tells the updater where a file goes.

    Only required for the kinds that are written to a partition, and only
    for the payload files themselves -- a licence text travels with the
    bundle but is never written to the medium, so it carries no install.

    When present the record names either a GPT partition or an absolute LBA
    range -- the loader lives at sector 64, outside every partition, so both
    forms are needed.  Exactly one of them, never both: ambiguity here would
    mean the board has to guess, and guessing wrong writes over something
    else.
    """
    if kind not in INSTALL_KINDS or name.startswith("licenses/"):
        if "install" in entry:
            raise ValueError(f"install is only for payload files: {name}")
        return None

    install = entry.get("install")
    if not isinstance(install, dict):
        raise ValueError(f"missing install record: {name}")

    partition = install.get("partition")
    lba = install.get("lba")
    if (partition is None) == (lba is None):
        raise ValueError(f"install needs exactly one of partition or lba: {name}")

    if partition is not None:
        if not isinstance(partition, str) or not re.fullmatch(r"[a-z0-9_]+", partition):
            raise ValueError(f"invalid install.partition: {name}")
        # The name has to be one nbootctl accepts, or the write will be
        # refused on the board after the whole file has been transferred.
        if partition not in NBOOTCTL_PARTITIONS:
            raise ValueError(f"unknown install.partition {partition!r}: {name}")
    else:
        if type(lba) is not int or lba < 0:
            raise ValueError(f"invalid install.lba: {name}")
        sectors = install.get("sectors")
        if type(sectors) is not int or sectors < 1:
            raise ValueError(f"install needs a positive sector count: {name}")
        # A raw window still has to fit the payload; catching it here means
        # the board never has to decide what to do with the overhang.
        needed = (entry["bytes"] + 511) // 512
        if needed > sectors:
            raise ValueError(
                f"install window is {sectors} sectors but {name} needs {needed}")

    return install


def nyamp_manifest(root, allow_review_required=False):
    root = Path(root).resolve(strict=True)
    path = nyamp_file(root, "manifest.json")
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or type(manifest.get("schema_version")) is not int or manifest["schema_version"] != 1:
        raise ValueError("unsupported bundle schema")
    for field in ("bundle_id", "version"):
        value = manifest.get(field)
        if not isinstance(value, str) or not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]*", value):
            raise ValueError(f"invalid {field}")
    if manifest["version"].lower() == "latest":
        raise ValueError("bundle version must be pinned")
    if manifest.get("kind") not in BUNDLE_KINDS:
        raise ValueError("unsupported bundle kind")
    kind = manifest["kind"]
    compatibility = manifest.get("compatibility", {})
    if not isinstance(compatibility, dict):
        raise ValueError("invalid compatibility")
    for field in ("soc", "architecture", "os", "interface_version"):
        if field not in compatibility:
            raise ValueError(f"missing compatibility.{field}")
    for field in ("soc", "architecture", "os"):
        if not isinstance(compatibility[field], str) or not compatibility[field]:
            raise ValueError(f"invalid compatibility.{field}")
    if type(compatibility["interface_version"]) is not int or compatibility["interface_version"] != 1:
        raise ValueError("unsupported model interface version")
    if not isinstance(compatibility.get("dependencies"), dict):
        raise ValueError("exact dependency versions are required")
    for key, value in compatibility["dependencies"].items():
        if not isinstance(key, str) or not key or not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.+-]+", value) or value.lower() == "latest":
            raise ValueError("dependency versions must be pinned")
    redistribution = manifest.get("redistribution")
    if redistribution != "approved" and not allow_review_required:
        raise ValueError(
            f"bundle redistribution is {redistribution!r}, not approved "
            "(pass --allow-review-required to pack anyway)")
    provenance = manifest.get("provenance", {})
    if not isinstance(provenance, dict):
        raise ValueError("invalid provenance")
    for field in ("source", "revision", "conversion"):
        if not isinstance(provenance.get(field), str) or not provenance[field].strip():
            raise ValueError(f"missing provenance.{field}")
    files = manifest.get("files")
    licenses = manifest.get("license_files")
    if not isinstance(files, list) or not files or not isinstance(licenses, list) or not licenses:
        raise ValueError("files and license_files must not be empty")
    names = {"manifest.json"}
    entries = []
    for entry in files:
        if not isinstance(entry, dict):
            raise ValueError("invalid file entry")
        name = entry.get("path")
        nyamp_relative(name)
        if name.casefold() in names:
            raise ValueError(f"duplicate package path: {name}")
        names.add(name.casefold())
        size, digest = entry.get("bytes"), entry.get("sha256")
        if type(size) is not int or size < 1 or not isinstance(digest, str) or not re.fullmatch(r"[a-f0-9]{64}", digest):
            raise ValueError(f"invalid size or digest: {name}")
        source = nyamp_file(root, name)
        if source.stat().st_size != size or nyamp_sha256(source) != digest:
            raise ValueError(f"file verification failed: {name}")
        nyamp_install(entry, name, kind)
        entries.append((name, source))
    for name in licenses:
        nyamp_relative(name)
        if not name.startswith("licenses/") or name not in {entry[0] for entry in entries}:
            raise ValueError("license files must be hashed entries below licenses/")
    return manifest, entries


def nyamp_pack(root, output, profile=None, allow_review_required=False):
    manifest, entries = nyamp_manifest(root, allow_review_required)
    # A compute resource only means anything against a specific runtime, so
    # it must say which.  Firmware and boot payloads have no such
    # dependency -- their compatibility is the board, which the manifest
    # already states.
    if manifest["kind"] in PROFILE_KINDS and profile is None:
        raise ValueError("model/runtime packing requires a target profile")
    if profile is not None:
        nyamp_compatible(manifest, profile)
    output = Path(output).absolute()
    if output.exists():
        raise ValueError("refusing to overwrite an existing bundle")
    if output.resolve().is_relative_to(Path(root).resolve()):
        raise ValueError("output must be outside the resource directory")
    # The directory is a trusted local staging area, not a concurrent upload API.
    canonical = (json.dumps(manifest, ensure_ascii=False, sort_keys=True,
                            separators=(",", ":")) + "\n").encode("utf-8")
    created = False
    try:
        with output.open("xb") as destination:
            created = True
            with gzip.GzipFile(filename="", mode="wb", fileobj=destination, mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.USTAR_FORMAT) as archive:
                    for name, source in [("manifest.json", None)] + sorted(entries):
                        header = tarfile.TarInfo(name)
                        header.size = len(canonical) if source is None else source.stat().st_size
                        header.mode = 0o755 if name.startswith("bin/") else 0o644
                        if source is None:
                            archive.addfile(header, io.BytesIO(canonical))
                        else:
                            with source.open("rb") as content:
                                archive.addfile(header, content)
    except Exception:
        # Only this call's newly created output can be removed.
        if created:
            output.unlink(missing_ok=True)
        raise
    return nyamp_sha256(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("verify", "pack"))
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--profile", type=Path, help="Target dependencies.json")
    parser.add_argument("--allow-review-required", action="store_true",
                        help="pack a bundle whose redistribution is not yet "
                             "approved; for internal test images only")
    args = parser.parse_args()
    try:
        profile = json.loads(args.profile.read_text(encoding="utf-8")) if args.profile else None
        if args.command == "verify":
            manifest, files = nyamp_manifest(args.root, args.allow_review_required)
            if profile is not None:
                nyamp_compatible(manifest, profile)
            note = "" if manifest["redistribution"] == "approved" else \
                " redistribution=" + str(manifest["redistribution"])
            print(f"NYAMP_BUNDLE_VERIFIED {manifest['bundle_id']} "
                  f"kind={manifest['kind']} files={len(files)}{note}")
        else:
            if args.output is None:
                parser.error("pack requires --output")
            print(f"{nyamp_pack(args.root, args.output, profile, args.allow_review_required)}  {args.output.name}")
    except (ValueError, OSError, json.JSONDecodeError) as error:
        parser.exit(1, f"bundle error: {error}\n")


if __name__ == "__main__":
    main()
