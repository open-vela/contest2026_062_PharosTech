# 用 `nyabula` 管理设备

在 NSH、ADB Shell 或以后接入的 SSH Shell 中执行 `nyabula`，进入编号菜单；输入 `0` 退出。脚本直接使用下面的子命令，失败时返回非零退出码。

## 忘记面板密码

```sh
nyabula panel reset-password
```

输入新密码两次（8–64 字节），不需要旧密码，终端不回显。保存成功后旧面板会话失效，用新密码重新登录。两次不一致、长度不符或写入失败不会采用新密码。

```sh
nyabula panel status
nyabula panel qr
nyabula panel hide
```

`status` 查看密码是否已设置、登录锁定状态与面板连接数；`qr` 在设备眼屏显示五分钟登录二维码，手机扫描后可以登录或设置新密码。语音可以说“帮我登录面板”或“忘了面板密码”，由 `nyabula_panel_login` 工具显示同一二维码。`hide` 立即关闭二维码；面板的“退出场景”执行同一操作。原密码只保存加盐摘要，不能还原；二维码和新密码不返回给云端模型。

## 查状态、配网络

```sh
nyabula status
nyabula device status
nyabula network status
nyabula network name Nyabula
nyabula network scan
nyabula network results
nyabula network connect "My WiFi"
```

`connect` 会提示输入 Wi-Fi 密码，不把密码放进命令历史。扫描或切换网络会影响当前无线连接，远程操作时保留串口或 ADB。

```sh
nyabula network provision
nyabula network stop-provision
```

开启或停止配网热点，使用现有产品配网流程。

## 模型、思考与语音

```sh
nyabula model status
nyabula mimo status
nyabula thinking status
nyabula thinking set none
nyabula thinking set enabled
nyabula voice status
nyabula voice on
nyabula voice say "你好，我是 Nyabula"
nyabula voice listen
nyabula voice cancel
nyabula voice off
```

MiMo 当前区分开启/关闭思考；其他等级保留为偏好，实际都按开启处理。详细配置与面板共用字段、校验和持久化：

```sh
nyabula model configure /tmp/model.json
nyabula mimo configure /tmp/mimo.json
nyabula voice configure /tmp/voice.json
```

`model.json` 使用 `host`、`path`、`port`（字符串）、`model`、`key` 字段。普通 MiMo API 和 Token Plan 的主机、密钥不能混用。`mimo.json` 示例：

```json
{
  "host": "api.xiaomimimo.com",
  "visionModel": "mimo-v2.6-flash",
  "ttsBackend": "mimo",
  "ttsModel": "mimo-v2.5-tts",
  "voice": "mimo_default",
  "style": "",
  "key": ""
}
```

空 `key` 保留已配置的专用密钥；没有专用密钥时，只在主机相同的情况下继承主模型密钥。显式 `clearKey: true` 清除专用密钥。`ttsBackend: "local"` 切回本地语音。`voice.json` 可以只包含需要修改的字段，例如 `{"replySpeed":100,"maxListenMs":15000}`。

### MiMo 2.6 与云端 ASR（2026-10-20 核对）

日常对话与图片理解使用 `mimo-v2.6-flash`；复杂对话可选择 `mimo-v2.6-pro`。
Token Plan 中国端点拆分为 `host: token-plan-cn.xiaomimimo.com`、`port: "443"`、
`path: /v1/chat/completions`。按量付费端点保持独立，不自动搬移密钥。

官方 ASR 仍为 `mimo-v2.5-asr`，TTS 仍为 `mimo-v2.5-tts` 系列，不统一替换版本号。
ASR 使用同一 Chat Completions 路径，通过 `input_audio` 提交 WAV/MP3 的 Base64，
而不是 `/audio/transcriptions`。现有设备语音识别走本地路径；云端 ASR 接入与实机联测
待设备连接后完成，不能仅配置 LLM 模型就视为已切换识别后端。

官方参考：[模型退役](https://mimo.mi.com/docs/en-US/updates/deprecate)、
[ASR 接口](https://mimo.mi.com/docs/zh-CN/api/audio/Speech-Recognition)。
旧文本 `mimo-v2.5` / `mimo-v2.5-pro` 于2026-10-21 10:00（UTC+8）退役。
代码读取旧图片模型时兼容映射到2.6 Flash；主对话已保存的模型配置由主人在配置时更新。

## 服务与排障

```sh
nyabula service status
nyabula service stop
nyabula service start
nyabula eyes status
nyabula storage status
nyabula update status
nyabula time status
nyabula time sync
nyabula logs
nyabula help
```

`service` 控制产品后台，不停止串口或 ADB；Web 监听服务仍由 `nyabula_web` 管理。所有已实现的产品接口还可直接调用，无需增加 CLI 别名：

```sh
nyabula request system.time.get
nyabula request voice.config /tmp/voice.json
nyabula request update.reboot
```

最后一条会重启设备。固件写入与槽位操作继续用 `nbootctl`，避免管理命令隐藏目标分区。现有 `nycore` 插件调试命令保持可用。

## SSH 调试路线（待独立接入）

上游 NuttX 已有 [Dropbear SSH 服务](https://nuttx.apache.org/docs/latest/applications/netutils/dropbear/index.html)，可以远程进入 NSH，并使用 `scp -O` 传文件。当前官方移植只支持密码认证、ECDSA P-256 主机密钥，不支持 SFTP、转发或公钥登录。

2026-10-18 检查当前 openvela 构建基线，`apps/netutils/dropbear` 尚不存在，不能仅加一个配置项就开启。推荐单独准备上游实现的兼容回移及板级配置，SSH 账户与面板密码分开，主机密钥和账户库放持久分区；不在本轮 CLI 中另造 SSH 协议实现。
