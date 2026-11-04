<!-- SPDX-License-Identifier: Apache-2.0 -->

# NyaLink 协议 v1（三端契约，唯一真源）

WebSocket 文本帧 JSON。局域网 `ws://<host>:7788/nyalink`；Cloud 中继 `wss://<cloud>/relay/<deviceId>`（语义完全相同）。

## 信封

```jsonc
{
  "v": 1,
  "id": "c1-42",            // req/res 配对用；event/err(非响应) 无 id
  "type": "req" | "res" | "event" | "err",
  "topic": "eye.state",
  "data": { }
}
```

- 响应：`type:"res"` 回带同 id；错误：`type:"err"`，`data: { code: "EPERM|EAUTH|EINVAL|ENOTFOUND|EBUSY", message }`。
- 事件携带 `data.seq`（每 topic 单调递增，接收端丢弃 seq 回退的包）。
- 心跳：客户端每 5s `req sys.ping {}` → `res {t: <server epoch ms>}`；15s 无 pong 判断线重连。

## 鉴权

1. 连接后首条必须 `req sys.hello { "client": "flutter|web|console", "version": "1.0.0", "token": "<或 null>" }`
   - token 有效 → `res { device: {id, name, coreVersion}, role: "owner|family|guest" }`
   - 无/失效 → `res { pairingRequired: true }`
2. 配对：`req sys.pair { "code": "483921", "name": "Beacon 的手机" }`（设备眼睛显示 6 位码，scene=pairing）
   → `res { token, role: "owner" }`（首个配对者 owner，后续默认 family）。
3. mock-core 阶段：万能码 `000000`，token 为随机串存内存。
4. hello 之前任何其他 topic → err EAUTH。角色下限不满足 → err EPERM。

## Topics v1

| topic | 类型 | 角色 | data |
|---|---|---|---|
| sys.hello | req | - | 见上 |
| sys.pair | req | - | 见上 |
| sys.ping | req | guest | `{}` → `{t}` |
| sys.info | req | guest | `{}` → `{device, uptime, battery, wifi}` |
| eye.state | event | guest | EyeState（见下），状态变化时推 + 2s 心跳全量 |
| eye.look | req | family | `{x, y, hold?}` x/y∈[-1,1]，hold 秒默认 2.2；`{release:true}` 立即释放 |
| eye.mode | req | family | `{mode: "<13枚举>"}`；设备端同时清除当前场景（表情即退出场景） |
| eye.scene | req | family | `{type: "<scene或null>", style?: "full|minimal", options?: {}, payload?: {}}` |
| ui.tree | req | guest | `{pluginId}` → NyaUI 组件树（C3） |
| ui.event | req | family | `{pluginId, componentId, event, values}`（C3） |
| ui.patch | event | guest | `{pluginId, patches: [{path, value}]}`（C3） |
| plugin.list | req | guest | `{}` → `{plugins: [{id,name,version,state,icon,ui,permissions:[{name,granted}]}]}` |
| plugin.grant | req | owner | `{pluginId, permission}` → `{ok}` |
| plugin.revoke | req | owner | `{pluginId, permission}` → `{ok}` |
| ui.tree / ui.event / ui.patch | - | - | 见 nyaui.md |
| agent.chat | req | family | `{text}` → 流式 event agent.delta `{id, delta, done}`（C4+） |

## EyeState（event eye.state 的 data）

同步意图不同步帧：接收端本地跑同一套状态机（参数/时序见 `app/nyabula_web/packages/eye-params/eye-params.json`）。

```jsonc
{
  "seq": 421,
  "t": 1735000000123,                  // 发送端 epoch ms，用于对齐 since
  "expression": {
    "mode": "sleepy",                  // eye-params.json modes 枚举
    "since": 1735000000000,           // 进入 mode 的 epoch ms → modeT=(t-since)/1000
    "lightLevel": 0.55,                // 0~1 环境光
    "sleepLid": [0.1, 0.05]            // 仅 mode=sleep：进入瞬间 [lidTop,lidBot] 快照
  },
  "gaze": { "mode": "auto",            // auto=各端自主扫视 | target
            "x": 0.0, "y": 0.0, "holdUntil": 0 },
  "appearance": { "irisLeft": "#38e06e", "irisRight": "#38e06e" },
  "scene": { "type": null,             // null=无场景
             "style": "full", "since": 0, "options": {}, "payload": {} },
  "oneShot": [ { "type": "blink", "at": 1735000003050 } ]  // 瞬时事件，收到即触发
}
```

约定：
- scenePhase/sceneLid/眨眼进度**不进协议**——本地过渡状态，由 scene.type/style 变化驱动各端本地状态机。
- 微动作（眨眼间隔/扫视）默认各端独立随机；需严格镜像时 expression 加 `"seed"`（v1 不做）。
- `eye.look` 多端抢占 last-writer-wins；hover 流请求发送端 ≥30ms 节流。
- 时钟：客户端用 sys.ping 的 `t` 估计偏移（RTT/2 校正一次即可，动画对 ±50ms 不敏感）。

## 局域网发现（UDP，v1.1）

- 设备监听 UDP :7789。客户端广播（255.255.255.255:7789 或子网广播）文本 `NYABULA_DISCOVER v1`。
- 设备回单播 JSON：`{"nyabula":1,"deviceId":"nya-xxxx","name":"Nyabula","port":7788,"coreVersion":"..."}`。
- 客户端连接页据此列出设备；手动输入只填 IP（客户端自行拼 `ws://<ip>:7788/nyalink`）。

## 配对场景（v1.1）

- 设备收到 `sys.hello` 且回 `pairingRequired:true` 时，自动显示 scene=pairing 60s。
- pairing 场景 payload：`{"code":"483921","host":"192.168.1.23","port":7788}`。
- 渲染约定：**左眼二维码**（内容 `nyabula://pair?host=<host>:<port>&code=<code>`），**右眼 6 位配对码**（3+3 两行大字）。
- 配对码由设备生成并持久化（取代 mock 万能码 000000）。

## 权限中文名（客户端显示用，权限 id 不变）

servo.control=舵机控制 storage.private=私有存储 storage.shared.read=共享存储读取 storage.shared.write=共享存储写入 gpio.read=GPIO 读取 gpio.write=GPIO 控制 network.request=网络请求 network.raw_socket=原始网络套接字 bluetooth.scan=蓝牙扫描 bluetooth.connect=蓝牙连接 wifi.status=WiFi 状态 wifi.control=WiFi 控制 audio.play=音频播放 audio.capture=录音 camera.capture=摄像头 location.read=位置信息 ui.card=眼睛卡片 ui.overlay=界面悬浮层 notification.post=发送通知 ai.invoke=AI 调用 secrets.use=密钥使用 background.run=后台运行 power.wake_lock=保持唤醒 system.settings=系统设置

## 版本演进

- 信封 `v` 不匹配 → err EINVAL + `data.supported:[1]`。
- topic 只增不改语义；字段只增不删；未知字段必须忽略（各端解析器要求宽容）。
