<!-- SPDX-License-Identifier: Apache-2.0 -->

# Nyabula Cloud 协议 v1（中继 + HTTP API + 设备云配置）

Cloud = 中央服务器（开源可自部署）。职责：设备中继、账号、设备归属、统计、（后续）插件商城。
**Cloud 不解设备业务语义**：客户端↔设备之间的 NyaLink 帧透明转发；设备端仍按 nyalink.md 对每条消息鉴权。Cloud 会终止两侧 TLS，能够看到转发内容，因此它属于可信服务；当前协议不提供端到端加密。

## 1. 三条连接

```
设备 ──outbound wss──▶ /api/v1/device/connect      (设备主动拨出，穿 NAT)
客户端 ──wss──▶ /api/v1/relay/{deviceId}?token=…    (透明 NyaLink，同局域网直连语义)
控制台 ──HTTP──▶ /api/v1/*                          (账号/设备/统计)
```

## 2. 设备链路（device/connect，多路复用）

设备（Core/模拟器）与 Cloud 之间一条 ws，复用多个客户端会话。文本帧 JSON：

```jsonc
// Cloud → 设备
{ "op": "open",  "sid": "s1" }                       // 新客户端接入
{ "op": "frame", "sid": "s1", "frame": { ...NyaLink 信封原样... } }
{ "op": "close", "sid": "s1" }
// 设备 → Cloud
{ "op": "frame", "sid": "s1", "frame": { ... } }      // 回给指定客户端
{ "op": "close", "sid": "s1" }                     // 设备关闭指定客户端（鉴权失效或超时）
{ "op": "cast",  "frame": { ... } }                   // 广播给该设备全部客户端（eye.state 等 event）
{ "op": "ping" } / { "op": "pong" }                   // 30s 链路保活（双向可发）
```

注册（连接后首帧，设备→Cloud）：
```jsonc
{ "op": "register", "deviceId": "nya-xxxx", "secret": "<deviceSecret>",
  "claimCode": "<6 位认领码>", "name": "Nyabula", "coreVersion": "0.1.0" }
// Cloud 回 { "op": "registered" } 或 { "op": "err", "code": "EAUTH|ECONFLICT" } 后关连接
```
- deviceId/secret：设备首次启动自生成并持久化；模拟器可 flag 指定。
- 未被任何账号认领的设备也可注册（等待认领）；认领码 claimCode = 设备端生成短码（模拟器打印到 stdout；原生真机在局域网面板的 Cloud 设置中向 owner 显示）。

## 3. 客户端中继（relay/{deviceId}）

- 前置：`?token=<账号 session token>` 且该设备已被此账号认领，否则 HTTP 401/403 拒绝升级。
- 升级成功后：**纯 NyaLink**，与 `ws://<device>:7788/nyalink` 完全同语义。原生固件先请求 sys.auth.state，再以 sys.login 获取设备会话 token，最后 sys.hello；设备密码须预先在局域网设置。旧版模拟器使用 sys.hello / sys.pair 配对码流程。Cloud 账号密码与设备访问密码是两套凭据。
- 设备离线：升级成功但立即收到 `err` 帧 `{code:"EOFFLINE"}` 后关闭（客户端据此提示）。

## 4. HTTP API（/api/v1，JSON，Bearer token）

| 方法 路径 | 说明 |
|---|---|
| POST /auth/register | `{email, password, name}` → `{token, user}`（v1 开放注册，自部署场景）|
| POST /auth/login | `{email, password}` → `{token, user}` |
| POST /auth/logout | 注销当前会话，并断开该会话的中继连接 |
| GET  /auth/me | → `{user}` |
| GET  /devices | → `{devices:[{deviceId,name,online,claimedAt,coreVersion,lastSeen}]}` |
| POST /devices/claim | `{deviceId, claimCode}` → `{device}`（认领）|
| DELETE /devices/{deviceId} | 解绑 |
| GET  /devices/{deviceId}/stats | → `{daily:[{date,onlineSeconds,framesUp,framesDown}], current:{online,clients}}` |
| GET  /stats/overview | → `{devices,online,clients,framesToday}` |

- 错误统一 `{error:{code,message}}`，HTTP 状态码语义化。
- token：随机 256bit，服务端存储（v1 不用 JWT，可随时吊销），有效期 30 天滑动。
- 存储 SQLite（自部署零依赖）：users / sessions / devices / device_stats_daily。
- 统计口径：Cloud 只计帧数与在线时长，**不记录帧内容**。

## 5. 设备侧云配置（NyaLink 新增 topic，设备端实现）

| topic | 类型 | 角色 | data |
|---|---|---|---|
| cloud.status | req | family | `{}` → `{enabled, url, connected, deviceId, claimCode?}`（claimCode 仅 owner 可见；设备端不判断 Cloud 归属）|
| cloud.config | req | owner | `{enabled, url}`（url 为公共 Cloud 或自建地址，如 wss://cloud.nyabula.tech 或 ws://192.168.1.10:8080）→ 设备保存并重连云 |

配置入口：客户端/Web 面板/蓝牙（蓝牙走后续 GATT 承载同样 topic，暂不定义）。

## 6. 两个 WebUI 定位

- **app/nyabula_web/apps/nyabula（设备面板）**：连**单台设备**（局域网直连），实时眼睛/表情/插件页/设置。设备本体可内嵌托管。
- **apps/console（Cloud 中心）**：连 **Cloud**，账号/多设备列表/认领/统计/商城（后续）/通过 relay 远程控制眼睛；完整设备设置仍由局域网设备面板提供。

## 7. 原生客户端约束

- 配置 NYABULA_CORE_CLOUD 启用；依赖 libwebsockets、mbedTLS wrapper。
- 每台设备最多 8 条中继会话，保留原生鉴权、速率与空闲超时。状态逐会话发送，未鉴权会话不会收到眼睛状态广播。
- 私有身份保存在 `/config/nyabula/cloud.identity`；cloud.status 不返回 deviceSecret。
- URL 可为服务根地址或完整 `/api/v1/device/connect` 路径；公网使用 wss。CA 文件路径由 NYABULA_CORE_CLOUD_CA 配置，默认 `/config/nyabula/cloud-ca.pem`；证书与主机名校验保持启用。
- 本地 ws 只用于受信任的联调网络。离线后自动重连；解绑、Cloud 注销、设备断线都会关闭相应中继会话。
