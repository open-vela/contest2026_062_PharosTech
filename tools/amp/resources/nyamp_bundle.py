#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate and reproducibly pack a local Nyabula resource directory."""

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import tarfile


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


def nyamp_manifest(root):
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
    if manifest.get("kind") not in ("model", "runtime", "fixture"):
        raise ValueError("unsupported bundle kind")
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
    if manifest.get("redistribution") != "approved":
        raise ValueError("bundle redistribution has not been approved")
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
        entries.append((name, source))
    for name in licenses:
        nyamp_relative(name)
        if not name.startswith("licenses/") or name not in {entry[0] for entry in entries}:
            raise ValueError("license files must be hashed entries below licenses/")
    return manifest, entries


def nyamp_pack(root, output, profile=None):
    manifest, entries = nyamp_manifest(root)
    if manifest["kind"] != "fixture" and profile is None:
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
    args = parser.parse_args()
    try:
        profile = json.loads(args.profile.read_text(encoding="utf-8")) if args.profile else None
        if args.command == "verify":
            manifest, files = nyamp_manifest(args.root)
            if profile is not None:
                nyamp_compatible(manifest, profile)
            print(f"NYAMP_BUNDLE_VERIFIED {manifest['bundle_id']} files={len(files)}")
        else:
            if args.output is None:
                parser.error("pack requires --output")
            print(f"{nyamp_pack(args.root, args.output, profile)}  {args.output.name}")
    except (ValueError, OSError, json.JSONDecodeError) as error:
        parser.exit(1, f"bundle error: {error}\n")


if __name__ == "__main__":
    main()
