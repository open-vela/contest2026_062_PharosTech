# 原生 Core 的眼睛 WebUI 接口

这是眼睛接入的第一片实现，不是完整的设备管理后端。浏览器独立运行 Canvas2D
眼睛引擎；WS 只传参数、状态和命令，不传 LCD 帧。网络任务不调用 LVGL。

## 运行边界

- `CONFIG_NYABULA_EYE_WS=y` 为可选构建项，默认关闭。
- 先由产品入口 `nyabula_eye` attach Eye 服务，再启动 `nyabula_eye_ws`。
- 首版只支持一个客户端；没有 TLS、设备认领、短码配对、插件 UI 或云转发。
- 必须提供 64 位十六进制随机令牌文件和精确的浏览器 Origin；令牌不得写入源码或日志。
- 默认只监听回环。开发中优先通过可信隧道访问；显式绑定其他 IPv4 地址只应用于
  隔离测试网络。不要将这个明文 WS 端点发布到公网。

NSH 示例（令牌文件由操作者安全预配置）：

```text
nyabula_eye &
nyabula_eye_ws 7788 /data/nyabula/ws.token http://127.0.0.1:5180 &
```

命令最后可加显式 `bind-ip`。网页在连接页填写 WS 地址及访问令牌；手动令牌仅
保存在会话内存，不加入已知设备列表或持久化存储。忘记/刷新后需重新输入。
静态 Web 应用仍在项目 `Web/apps/nyabula`，不由此端点提供。

## NyaLink 信封

请求沿用 `v=1/type=req/id/topic/data`，响应为 `res` 或 `err`。

| topic | data / 语义 |
|---|---|
| `sys.hello` | `token`，成功后能力含 `eyes.native-v1` |
| `sys.ping` | 返回设备墙钟 `t`，不作为原生眼睛状态的时间基准 |
| `eyes.state.get` | 取得当前完整快照，解决页面晚挂载及重连后的初始状态 |
| `eyes.expression` | `expression`、可选 `transition_ms` |
| `eyes.gaze` | `x/y` 为 -1～1，`hold_ms` 为 0～5000；0 释放目标 |
| `eyes.blink` | 可选 `eyes=left/right/both` |
| `eyes.auto_blink` | `enabled` |
| `eyes.ambient` | `level` 为 0～1 |
| `eyes.iris` | `rgb` 为 24 位十六进制字符串、可选 `eyes` |
| `eyes.scene.show/update/hide` | 原生 `SCENE_SCHEMA.md` 的参数，不接受任意 UI |

所有写请求通过 `nyabula_eye_service_submit()` 进入 Core 队列。网络层固定来源
`webui`、优先级 40、租约 5000ms，忽略客户端伪造的身份/优先级/租约。
响应 `queued:true` 仅表示入队，不表示已经显示。实际结果看快照里的
`last_request_id/last_status` 和当前 owner/revision。场景与表情仍服从 Core 原有仲裁；
注视沿用原生 gaze API，不额外声称存在独立的 gaze 优先级仲裁。

## 状态与独立渲染

事件 topic 为 `eye.state`，data 的 `schema=nyabula.eye.v1`。`seq` 是实际 Core
revision。原生快照包含表情、场景及 payload、虹膜、环境光、自动眨眼、单次眨眼
nonce、显式注视目标/有效期、owner 和最后命令结果。

`uptime_ms/expression_since_ms/scene_since_ms/gaze_until_ms` 都使用同一个单调时钟。
浏览器在接收处转成相对年龄和剩余保持时间，不要求板子设置 RTC，也不能再叠加
NyaLink 墙钟偏移。首次快照不重放历史 blink nonce。网页可自主运行微动动画，
它是参数驱动的独立渲染器，不是实际 LCD 的像素镜像。

逗猫棒开启后，鼠标悬停即可追视；触屏支持点击、拖动、取消及单指捕获。
前端本地预览降低手感延迟，Core 回传状态负责校正。网络发送限制为单请求在途、
只保留最新目标，避免堆积旧光标轨迹。释放和连接中断不会留下无限期注视目标。

## 验证

`tools/nyabula_core/tests/eye_ws_integration.py` 启动真正的 NuttX sim、Core、Eye
和本 WS 服务，使用测试 LCD 接收器代替硬件。需启用 `SIM_NETUSRSOCK` 和现有
`EXAMPLES_EYEPROBE` 测试夹具。它不使用 Go Simulator，不写板子。

浏览器像素基线测试位于 `Web/packages/eye-engine/test/visual-parity.cjs`：同尺寸、
同随机种子和时钟，对照原始 `工具/cat_eyes_demo.html`，原始文件不修改。
特定样本的像素一致不等于所有浏览器/分辨率/动画时刻或板上显示均已验证。

协议边界依据 [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html)：有界头部、
掩码校验、分片文本及控制帧、UTF-8、读写超时。SHA-1 和 Base64 复用系统库。
首版的限流、单连接和预共享令牌只覆盖隔离开发场景，不能替代产品级安全设计。

功能页中的音乐播放、计时任务、天气采集等业务不由眼睛渲染接口实现；本次不迁移
这些服务。部分页面的数据字段仍需逐项适配原生场景，不以视觉基线通过冒充业务完成。

### 2026-09-12 本地验收

- NuttX sim完整CMake构建通过；原生WS集成测试通过。
- 桌面1440×1000、手机390×844的生产Web产物实际连接上述Core：鼠标悬停与触摸拖动
  均改变Core注视状态，关闭后释放，零页面异常，手动令牌未持久化。
- 800×480/DPR=1下对照原demo的201个确定性帧，像素差异为0；涵盖13表情、25场景
  的两种样式、环境光、异瞳、注视和眨眼。
- Eye/NyaUI共33项单元测试、Web类型检查、生产构建通过。
- K7 ARM64完整Make构建和链接通过，实际包含WS入口与状态序列化符号；没有烧板。

上述结果不等于已完成物理双屏、真实手机硬件触控、TLS或完整设备WebUI验收。
