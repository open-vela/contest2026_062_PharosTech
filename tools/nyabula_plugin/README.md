# Nyabula Plugin SDK

Wasm 的 C、Rust、TinyGo 构建约束见 [WASM_SDK.md](WASM_SDK.md)。

视觉接口为`import * as ui from "@nyabula/ui"`后的`ui.eye(command)`与
`ui.notify(text)`，共用`ui.notify`权限。命令字段、入队语义和Display启动步骤
见[Core说明](../../app/nyabula_core/README.md)。

`nyabula_plugin.py` builds and packages TypeScript/JavaScript plugins for the
Nyabula Core runtime.

## Project layout

```text
plugin/
├── manifest.json
├── src/main.ts
├── assets/
└── node_modules/
```

The source module must default-export an object created with `definePlugin()`.
The build command generates the runtime lifecycle exports and leaves
`@nyabula/*` capability imports external for the device loader.

## Commands

```sh
python nyabula_plugin.py build PLUGIN --esbuild /path/to/esbuild
python nyabula_plugin.py test PLUGIN
python nyabula_plugin.py keygen --id developer --private-key developer.pem \
  --trust-store trusted-keys.json
python nyabula_plugin.py pack PLUGIN --output plugin.nya \
  --private-key developer.pem --key-id developer
python nyabula_plugin.py verify plugin.nya --trust-store trusted-keys.json \
  --require-signature
python nyabula_plugin.py install plugin.nya --root SIM_PLUGIN_ROOT \
  --trust-store trusted-keys.json --require-signature
```

`pack` creates a deterministic ZIP-compatible `.nya` archive with a complete
SHA-256 `hashes.json`. Signed packages add a `signer.json` covered by that hash
manifest and a raw 64-byte Ed25519 signature over the exact `hashes.json`
bytes. `install` verifies the archive and atomically extracts a new plugin ID
under the selected root. Version upgrades and last-known-good activation belong
to the device package manager.
