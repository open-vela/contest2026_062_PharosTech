# Nyabula Plugin SDK

## 安装与中文显示名

通过局域网直连设备后，在产品 WebUI 的“插件”列表点击“安装插件”，上传 `.nya` 或 `.zip`。
包根目录必须包含 `manifest.json`；原生设备仍验证签名、签名者信任和文件完整性。
上传上限与解包上限均为 8 MiB，最多 128 个文件。解包在浏览器进行，
通过认证的 `plugin.upload.begin/chunk/commit/cancel` 分块发送到设备暂存目录，
不要求设备引入 ZIP 解压库。原有设备目录安装入口保留在弹窗“高级”中。
新装插件保持停止且不自动授权；在列表开关启动，详情页管理能力权限。
模拟器允许可信开发源码包，不提供原生签名信任链验收。

插件 ID 继续用于路径和接口，显示名可使用中文：

```json
{"id":"companion-counter","name":"陪伴小记","version":"1.1.0"}
```

`name` 可选，1–128 个 UTF-8 字节；未填写时回退到 ID。其余原有清单字段仍必填。
显示名属于签名清单，修改后重新打包和签名，不能直接篡改已签包。


Wasm 的 C、Rust、TinyGo 构建约束见 [WASM_SDK.md](WASM_SDK.md)。

视觉接口为`import * as ui from "@nyabula/ui"`后的`ui.eye(command)`与
`ui.notify(text)`，共用`ui.notify`权限。命令字段、入队语义和Display启动步骤
见[Core说明](../../app/nyabula_core/README.md)。
通知会显示为眼睛字幕，最多47个UTF-8字节；详细反馈放在插件页面。

## Agent生成与技能

设备内生成使用[生成契约](authoring-contract.txt)，由主人开启后签名安装，
并通过真实UI事件检查行为。完整使用方法见[Agent工作台](../../docs/agent-workbench.md)。
`examples/local-ledger`保留真机MiMo生成并迭代的记账工具源码，
`skills/transfer-planning/SKILL.md`可在Nyabot技能页导入。
技能不授予插件权限；生成工具仍需遵守组件、存储及硬件能力边界。

`nyabula_plugin.py` builds and packages TypeScript/JavaScript plugins for the
Nyabula Core runtime.

## 图片与 iconfont 图标

清单里的 `icon` 可以继续使用内置图标名，例如 `"pets"`；也可以引用包内文件：

```json
"icon": { "type": "image", "path": "assets/icon.png" }
```

使用自己的图标字体时，提供 WOFF2 子集和十六进制 Unicode 码点：

```json
"icon": { "type": "iconfont", "path": "assets/icons.woff2", "codepoint": "e601" }
```

图片支持 PNG、JPEG、WebP；字体支持 WOFF2。单个资源最多 16 KiB，路径必须位于
`assets/`，不能包含 `..`、符号链接或外部 URL。`pack` 自动收录 assets 并签名。
列表、详情、首页小组件和命令面板共用同一图标。宿主通过 `plugin.icon` 按需获取，
资源缺失或无法解码时回退到默认图标，不影响插件使用。

完整示例见 `examples/hardware-panel`，其中包含原创 PNG 和 WOFF2 两种资源。

## 硬件能力与授权

QuickJS 插件通过同一个受控入口调用硬件，不直接打开设备节点：

```ts
import { request } from "@nyabula/hardware";

// Declare audio.read in manifest.permissions, then grant it on the panel.
const status = await request("audio.status");
```

声明权限不等于授权。Core 每次调用检查当前权限；撤销后下一次调用立即拒绝。
该入口只接受下表对应的固定 topic，不允许借用主人的身份调用任意产品接口。
Wasm 当前没有此新增 JS 模块。

| 权限 | 可调用能力 |
|---|---|
| `audio.read` | `audio.status`、`music.library/status`、`dsp.status` |
| `audio.control` | `audio.volume/channel/output.route`；麦克风设置另需 `audio.capture` |
| `audio.play` | `music.play/pause/resume/stop` |
| `audio.capture` | `audio.capture` 读取已开启麦克风的短片段；配合 control 可调用 `audio.input.route/mic.gain/mic.mute` |
| `usb.access` | `usb.devices` 枚举真实 USB 音频节点，结果明确标注 `scope: usb-audio` |
| `bluetooth.read` | `bt.status/devices`，不返回 HFP 通话详情或配对数字 |
| `bluetooth.connect` | `bt.discoverable/pair.confirm/connect/disconnect/forget` |
| `bluetooth.call` | 通话状态与号码、`bt.call.reject/hangup`；`bt.call.answer` 还需 `audio.capture` |
| `camera.capture` | 受控摄像头入口；当前采集服务未提供，调用明确返回 `ENOTSUP` |

- `dsp.config` 同时需要 `audio.control` 与 `usb.access`；`dsp.play/stop` 同时需要
  `audio.play` 与 `usb.access`，避免输出路由变化后越权访问 USB 声卡。
- `bt.media.play/pause/stop/next/prev` 同时需要 `bluetooth.connect` 与 `audio.play`。
- `audio.capture { samples: 1600 }` 最多返回 100 ms、16 kHz、单声道 S16LE 的 base64
  数据。它不打开麦克风，不消耗唤醒词读取游标；麦克风关闭、静音或没有新数据时失败。
- USB 权限不是任意 USB 控制传输或文件访问能力。摄像头仍归 openvela，未转交 Linux。
- Windows 工作台执行同样的权限检查。采集与硬件缺失会返回不支持；开发测试可显式
  配置 `hardware:<topic>` mock，模拟数据不会被当作真实设备结果。

## 自定义页面

在 `onStart()` 中调用 `ui.render(document)` 发布页面。组件树需要 `ui.page`，HTML 需要 `ui.html`；声明权限后仍须主人授权。

```ts
import * as ui from "@nyabula/ui";

export default {
  onStart() {
    ui.render({ nyaui: 1, root: {
      type: 'button', id: 'hello', props: { text: '打个招呼' },
      on: { tap: { command: 'hello' } },
    }});
  },
  onCommand(command, args, values) {
    // The runtime resolves the published binding, not a client-supplied command.
  },
};
```

需要两种显示方式时，发布 `views: { component: {nyaui:1, root:...}, html: {nyaui:1, html:..., events:...} }`，切换交给宿主顶栏，不在插件内部放模式切换按钮。两种视图共用插件状态，并分别需要 `ui.page` / `ui.html` 授权。该扩展需要支持 views 的 Core；旧单视图文档保持兼容。

HTML 文档用 `html` 替代 `root`，并声明 `events: { hello: { command: 'hello' } }`；帧内通过 `nyabula.send('hello')` 发送事件。容器默认透明并按内容自动伸缩。完整文档不超过 24 KiB。

运行中的权限撤销立即作用于能力调用，UI 权限撤销同时清除已发布页面。停止、重新启动后由插件重新发布。`ui.event` 的 `queued:true` 仅表示入队，不表示硬件动作已经完成。

## Project layout

```text
plugin/
├── manifest.json
├── src/main.ts
├── assets/
└── node_modules/
```

The source module default-exports a lifecycle object with `onStart`, `onCommand`,
and optional `onEvent` / `onStop` methods.
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
