#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements. See the NOTICE file distributed with this work for
# additional information regarding copyright ownership. The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations.
"""Build, validate, package, and install Nyabula plugins."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey,
        Ed25519PublicKey,
    )
except ImportError:
    InvalidSignature = None  # type: ignore[assignment,misc]
    Ed25519PrivateKey = None  # type: ignore[assignment,misc]
    Ed25519PublicKey = None  # type: ignore[assignment,misc]

API_VERSION = 1
MAX_ARCHIVE_FILES = 128
MAX_ARCHIVE_SIZE = 8 * 1024 * 1024
MAX_MANIFEST_SIZE = 16 * 1024
KNOWN_PERMISSIONS = {
    "core.log",
    "storage.read",
    "storage.write",
    "network.request",
    "ui.notify",
    "ai.invoke",
}
FIXED_ZIP_TIME = (1980, 1, 1, 0, 0, 0)
SIGNATURE_SIZE = 64


class PluginError(Exception):
    """An actionable plugin validation error."""


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise PluginError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load_json(path: Path, limit: int) -> dict[str, object]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise PluginError(f"cannot read {path}: {exc}") from exc
    if not data or len(data) > limit:
        raise PluginError(f"invalid size for {path}: {len(data)}")
    try:
        value = json.loads(data, object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PluginError(f"invalid JSON in {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise PluginError(f"JSON root must be an object: {path}")
    return value


def _crypto_required() -> None:
    if Ed25519PrivateKey is None or Ed25519PublicKey is None:
        raise PluginError("cryptography is required; install requirements.txt")


def keygen(key_id: str, private_key_path: Path, trust_store_path: Path) -> None:
    _crypto_required()
    if not key_id or len(key_id) >= 64 or any(
        not ("a" <= char <= "z" or "0" <= char <= "9" or char in "._-")
        for char in key_id
    ):
        raise PluginError("invalid signer key ID")
    if private_key_path.exists():
        raise PluginError(f"private key already exists: {private_key_path}")
    private_key = Ed25519PrivateKey.generate()
    private_data = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )
    public_data = private_key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )
    private_key_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = private_key_path.with_suffix(private_key_path.suffix + ".tmp")
    # Create with 0600 from the start so the unencrypted key is never
    # world-readable, not even between write and chmod.
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.write(fd, private_data)
    finally:
        os.close(fd)
    os.replace(temporary, private_key_path)
    trust_store: dict[str, str] = {}
    if trust_store_path.exists():
        existing = _load_json(trust_store_path, MAX_MANIFEST_SIZE)
        if any(not isinstance(value, str) for value in existing.values()):
            raise PluginError("existing trust store contains invalid keys")
        trust_store = {name: str(value) for name, value in existing.items()}
    if key_id in trust_store:
        raise PluginError(f"signer already exists: {key_id}")
    trust_store[key_id] = public_data.hex()
    trust_store_path.parent.mkdir(parents=True, exist_ok=True)
    trust_store_path.write_text(
        json.dumps(trust_store, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _load_private_key(path: Path) -> object:
    _crypto_required()
    try:
        key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    except (OSError, ValueError) as exc:
        raise PluginError(f"cannot load private key: {exc}") from exc
    if not isinstance(key, Ed25519PrivateKey):
        raise PluginError("private key is not Ed25519")
    return key


def _load_trust_store(path: Path) -> dict[str, bytes]:
    value = _load_json(path, MAX_MANIFEST_SIZE)
    result: dict[str, bytes] = {}
    for key_id, encoded in value.items():
        if not isinstance(encoded, str) or len(encoded) != 64:
            raise PluginError(f"invalid public key for {key_id}")
        try:
            public_key = bytes.fromhex(encoded)
        except ValueError as exc:
            raise PluginError(f"invalid public key for {key_id}") from exc
        if len(public_key) != 32:
            raise PluginError(f"invalid public key length for {key_id}")
        result[key_id] = public_key
    return result


def _safe_relative(value: object, suffix: str | None = None) -> str:
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        raise PluginError("path must be a non-empty POSIX relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise PluginError(f"unsafe relative path: {value}")
    if suffix is not None and path.suffix != suffix:
        raise PluginError(f"path must end in {suffix}: {value}")
    return path.as_posix()


def validate_manifest(path: Path) -> dict[str, object]:
    manifest = _load_json(path, MAX_MANIFEST_SIZE)
    required = {"id", "version", "apiVersion", "runtime", "entry"}
    missing = sorted(required - manifest.keys())
    if missing:
        raise PluginError(f"missing manifest members: {', '.join(missing)}")

    plugin_id = manifest["id"]
    if (
        not isinstance(plugin_id, str)
        or not plugin_id
        or len(plugin_id) >= 64
        or plugin_id.startswith(".")
        or plugin_id.endswith(".")
        or any(not ("a" <= char <= "z" or "0" <= char <= "9" or char in "._-") for char in plugin_id)
    ):
        raise PluginError(f"invalid plugin id: {plugin_id!r}")

    version = manifest["version"]
    if not isinstance(version, str) or not version or len(version) >= 32:
        raise PluginError("version must be a non-empty string shorter than 32 bytes")
    if isinstance(manifest["apiVersion"], bool) or manifest["apiVersion"] != API_VERSION:
        raise PluginError(f"unsupported apiVersion: {manifest['apiVersion']!r}")
    if not isinstance(manifest.get("background", False), bool):
        raise PluginError("background must be a boolean")
    runtime = manifest["runtime"]
    if runtime not in ("quickjs", "wamr"):
        raise PluginError(f"unsupported runtime: {runtime!r}")
    manifest["entry"] = _safe_relative(
        manifest["entry"], ".js" if runtime == "quickjs" else ".wasm"
    )

    permissions = manifest.get("permissions", [])
    if not isinstance(permissions, list) or any(not isinstance(item, str) for item in permissions):
        raise PluginError("permissions must be an array of strings")
    unknown = sorted(set(permissions) - KNOWN_PERMISSIONS)
    if unknown:
        raise PluginError(f"unknown permissions: {', '.join(unknown)}")
    if len(permissions) != len(set(permissions)):
        raise PluginError("permissions must not contain duplicates")

    limits = manifest.get("limits", {})
    if not isinstance(limits, dict):
        raise PluginError("limits must be an object")
    ranges = {
        "memoryKiB": (64, 8192),
        "stackKiB": (16, 256),
        "cpuMsPerEvent": (1, 100),
        "eventsPerMinute": (1, 600),
    }
    for name, value in limits.items():
        if name not in ranges:
            raise PluginError(f"unknown limit: {name}")
        minimum, maximum = ranges[name]
        if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
            raise PluginError(f"{name} must be an integer in [{minimum}, {maximum}]")
    return manifest


def _find_esbuild(project: Path, requested: str | None) -> str:
    candidates = []
    if requested:
        candidates.append(Path(requested))
    local_name = "esbuild.cmd" if os.name == "nt" else "esbuild"
    candidates.append(project / "node_modules" / ".bin" / local_name)
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    executable = shutil.which("esbuild")
    if executable:
        return executable
    raise PluginError("esbuild not found; run npm install or pass --esbuild")


def build_plugin(project: Path, esbuild: str | None) -> Path:
    manifest = validate_manifest(project / "manifest.json")
    if manifest["runtime"] != "quickjs":
        raise PluginError("WAMR projects must provide a prebuilt .wasm entry")
    source = project / "src" / "main.ts"
    if not source.is_file():
        raise PluginError(f"missing TypeScript entry: {source}")

    build_root = project / "build"
    helper_root = build_root / ".nyabula"
    output = build_root / str(manifest["entry"])
    helper_root.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    source_import = os.path.relpath(source, helper_root).replace(os.sep, "/")
    if not source_import.startswith("."):
        source_import = f"./{source_import}"
    wrapper = helper_root / "entry.ts"
    wrapper.write_text(
        "import plugin from " + json.dumps(source_import) + ";\n"
        "export async function ny_on_start() { return plugin.onStart?.(); }\n"
        "export async function ny_on_event(event: string) { return plugin.onEvent?.(event); }\n"
        "export async function ny_on_stop() { return plugin.onStop?.(); }\n",
        encoding="utf-8",
        newline="\n",
    )

    command = [
        _find_esbuild(project, esbuild),
        str(wrapper),
        "--bundle",
        "--format=esm",
        "--platform=neutral",
        "--target=es2020",
        "--external:@nyabula/core",
        "--external:@nyabula/storage",
        "--external:@nyabula/network",
        "--external:@nyabula/ui",
        "--external:@nyabula/ai",
        f"--outfile={output}",
    ]
    try:
        subprocess.run(command, cwd=project, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise PluginError(f"esbuild failed: {exc}") from exc
    return output


def test_project(project: Path) -> None:
    manifest = validate_manifest(project / "manifest.json")
    entry = project / "build" / str(manifest["entry"])
    if not entry.is_file() or entry.stat().st_size == 0:
        raise PluginError(f"built entry missing: {entry}")
    if entry.stat().st_size > 1024 * 1024:
        raise PluginError("built entry exceeds the device source limit")
    if manifest["runtime"] == "wamr":
        if entry.read_bytes()[:8] != b"\0asm\x01\0\0\0":
            raise PluginError("WAMR entry is not a WebAssembly 1.0 module")
    else:
        content = entry.read_text(encoding="utf-8")
        for lifecycle in ("ny_on_start", "ny_on_event", "ny_on_stop"):
            if lifecycle not in content:
                raise PluginError(f"built entry does not export {lifecycle}")


def _collect_package_files(project: Path, manifest: dict[str, object]) -> dict[str, bytes]:
    files: dict[str, bytes] = {
        "manifest.json": (project / "manifest.json").read_bytes(),
    }
    entry_name = str(manifest["entry"])
    entry_path = project / "build" / entry_name
    if not entry_path.is_file():
        raise PluginError(f"built entry missing: {entry_path}")
    files[entry_name] = entry_path.read_bytes()

    assets = project / "assets"
    if assets.exists():
        for path in sorted(assets.rglob("*")):
            if path.is_symlink():
                raise PluginError(f"asset symlinks are forbidden: {path}")
            if path.is_file():
                relative = _safe_relative(Path("assets", path.relative_to(assets)).as_posix())
                files[relative] = path.read_bytes()
    if len(files) + 1 > MAX_ARCHIVE_FILES:
        raise PluginError("package contains too many files")
    if sum(len(value) for value in files.values()) > MAX_ARCHIVE_SIZE:
        raise PluginError("package exceeds the uncompressed size limit")
    return files


def pack_plugin(
    project: Path,
    output: Path | None,
    private_key_path: Path | None,
    key_id: str | None,
) -> Path:
    manifest = validate_manifest(project / "manifest.json")
    test_project(project)
    files = _collect_package_files(project, manifest)
    if (private_key_path is not None or key_id is not None):
        if private_key_path is None or key_id is None:
            raise PluginError("--private-key and --key-id must be used together")
        if not key_id or len(key_id) >= 64:
            raise PluginError("invalid signer key ID")
        files["signer.json"] = (
            json.dumps({"keyId": key_id}, separators=(",", ":"), sort_keys=True) + "\n"
        ).encode("utf-8")
    hashes = {
        name: hashlib.sha256(content).hexdigest()
        for name, content in sorted(files.items())
    }
    hashes_data = (
        json.dumps(hashes, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode("utf-8")
    files["hashes.json"] = hashes_data
    if private_key_path is not None:
        private_key = _load_private_key(private_key_path)
        files["signature.ed25519"] = private_key.sign(hashes_data)
    if output is None:
        output = project / "dist" / f"{manifest['id']}-{manifest['version']}.nya"
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, content in sorted(files.items()):
            info = zipfile.ZipInfo(name, FIXED_ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(info, content)
    os.replace(temporary, output)
    return output


def _safe_archive_name(name: str) -> str:
    return _safe_relative(name)


def verify_archive(
    path: Path,
    trust_store_path: Path | None = None,
    require_signature: bool = False,
) -> tuple[dict[str, object], dict[str, bytes]]:
    files: dict[str, bytes] = {}
    try:
        with zipfile.ZipFile(path, "r") as archive:
            entries = archive.infolist()
            if not entries or len(entries) > MAX_ARCHIVE_FILES:
                raise PluginError("invalid archive file count")
            total = 0
            for entry in entries:
                name = _safe_archive_name(entry.filename)
                if name in files or entry.is_dir():
                    raise PluginError(f"duplicate or directory entry: {name}")
                mode = entry.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise PluginError(f"archive symlink forbidden: {name}")
                total += entry.file_size
                if total > MAX_ARCHIVE_SIZE:
                    raise PluginError("archive exceeds the size limit")
                content = archive.read(entry)
                if len(content) != entry.file_size:
                    raise PluginError(f"archive size mismatch: {name}")
                files[name] = content
    except (OSError, zipfile.BadZipFile) as exc:
        raise PluginError(f"invalid archive: {exc}") from exc

    if "manifest.json" not in files or "hashes.json" not in files:
        raise PluginError("archive is missing manifest.json or hashes.json")
    with tempfile.TemporaryDirectory() as directory:
        manifest_path = Path(directory) / "manifest.json"
        manifest_path.write_bytes(files["manifest.json"])
        manifest = validate_manifest(manifest_path)
    try:
        hashes = json.loads(files["hashes.json"], object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PluginError(f"invalid hashes.json: {exc}") from exc
    expected_names = set(files) - {"hashes.json", "signature.ed25519"}
    if not isinstance(hashes, dict) or set(hashes) != expected_names:
        raise PluginError("hash manifest does not cover exactly the package files")
    for name in sorted(expected_names):
        actual = hashlib.sha256(files[name]).hexdigest()
        if hashes[name] != actual:
            raise PluginError(f"hash mismatch: {name}")
    if str(manifest["entry"]) not in files:
        raise PluginError("manifest entry is absent from archive")
    has_signature = "signature.ed25519" in files or "signer.json" in files
    if require_signature and not has_signature:
        raise PluginError("package signature is required")
    if has_signature:
        if "signature.ed25519" not in files or "signer.json" not in files:
            raise PluginError("incomplete package signature")
        if len(files["signature.ed25519"]) != SIGNATURE_SIZE:
            raise PluginError("invalid Ed25519 signature length")
        if "signer.json" not in hashes:
            raise PluginError("signer metadata is not hash-covered")
        if trust_store_path is None:
            if require_signature:
                raise PluginError("a trust store is required")
            print(
                "WARNING: package is signed but no trust store was given; "
                "the signature was NOT verified",
                file=sys.stderr,
            )
        else:
            _crypto_required()
            with tempfile.TemporaryDirectory() as directory:
                signer_path = Path(directory) / "signer.json"
                signer_path.write_bytes(files["signer.json"])
                signer = _load_json(signer_path, 4096)
            key_id = signer.get("keyId")
            if not isinstance(key_id, str):
                raise PluginError("signer keyId is missing")
            trust_store = _load_trust_store(trust_store_path)
            if key_id not in trust_store:
                raise PluginError(f"untrusted signer: {key_id}")
            try:
                Ed25519PublicKey.from_public_bytes(trust_store[key_id]).verify(
                    files["signature.ed25519"], files["hashes.json"]
                )
            except ValueError as exc:
                raise PluginError(f"invalid trusted public key: {key_id}") from exc
            except InvalidSignature as exc:
                raise PluginError("Ed25519 signature verification failed") from exc
    return manifest, files


def install_archive(
    archive: Path,
    root: Path,
    trust_store: Path | None,
    require_signature: bool,
) -> Path:
    manifest, files = verify_archive(archive, trust_store, require_signature)
    plugin_id = str(manifest["id"])
    destination = root / plugin_id
    if destination.exists():
        raise PluginError(f"plugin already installed: {plugin_id}")
    root.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{plugin_id}.", dir=root))
    try:
        for name, content in sorted(files.items()):
            target = temporary.joinpath(*PurePosixPath(name).parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        os.replace(temporary, destination)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return destination


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="nyabula-plugin")
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build")
    build.add_argument("project", type=Path)
    build.add_argument("--esbuild")
    test = subparsers.add_parser("test")
    test.add_argument("project", type=Path)
    pack = subparsers.add_parser("pack")
    pack.add_argument("project", type=Path)
    pack.add_argument("--output", type=Path)
    pack.add_argument("--private-key", type=Path)
    pack.add_argument("--key-id")
    keygen_parser = subparsers.add_parser("keygen")
    keygen_parser.add_argument("--id", required=True)
    keygen_parser.add_argument("--private-key", required=True, type=Path)
    keygen_parser.add_argument("--trust-store", required=True, type=Path)
    verify = subparsers.add_parser("verify")
    verify.add_argument("archive", type=Path)
    verify.add_argument("--trust-store", type=Path)
    verify.add_argument("--require-signature", action="store_true")
    install = subparsers.add_parser("install")
    install.add_argument("archive", type=Path)
    install.add_argument("--root", required=True, type=Path)
    install.add_argument("--trust-store", type=Path)
    install.add_argument("--require-signature", action="store_true")
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        if args.command == "build":
            print(build_plugin(args.project.resolve(), args.esbuild))
        elif args.command == "test":
            test_project(args.project.resolve())
            print("PASS")
        elif args.command == "pack":
            print(
                pack_plugin(
                    args.project.resolve(),
                    args.output,
                    args.private_key.resolve() if args.private_key else None,
                    args.key_id,
                )
            )
        elif args.command == "keygen":
            keygen(args.id, args.private_key.resolve(), args.trust_store.resolve())
            print(args.trust_store.resolve())
        elif args.command == "verify":
            manifest, _ = verify_archive(
                args.archive.resolve(),
                args.trust_store.resolve() if args.trust_store else None,
                args.require_signature,
            )
            print(f"PASS {manifest['id']} {manifest['version']}")
        elif args.command == "install":
            print(
                install_archive(
                    args.archive.resolve(),
                    args.root.resolve(),
                    args.trust_store.resolve() if args.trust_store else None,
                    args.require_signature,
                )
            )
        return 0
    except PluginError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
