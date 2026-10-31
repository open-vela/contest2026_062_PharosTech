# 从运行中的系统升级（OTA）

fastboot 是"主机推"的路径：要进 N-Boot、要 USB 线。OTA 是"系统自己写"的路径：
NuttX 在跑、固件经网络到达、由 `nbootctl` 的库函数写进非运行槽。两条路径最终
落到同一份 bootctrl，格式与写入顺序一致（见 [bootctrl.md](bootctrl.md)）。

本页每个结论后标置信度：**实测确认**（板上跑出）/ **读码确认**（读源码得出，
未单独上板对照）/ **仅推断**。来源：`实测日志.md` 2026-09-20 "面板上传升级（OTA）
全链路上板通过"、`app/nbootctl/`、`app/nyabula_core/ny_web_ota.c`、
`ny_product_maintenance.c`、`tools/amp/DISTRIBUTION.md`（草稿分支）。

## 设计：先落文件、两端对摘要、再整块写

```
上传方                          板子
  ├─ 算整个文件的 SHA-256
  ├─ POST 整个文件 ───────────→ 边收边写临时文件，边算 SHA-256
  │                  ←───────── {received, sha256}
  │   两端摘要不符 → 临时文件删除，介质一个扇区都没碰
  └─ 确认后下发"应用" ────────→ 再算一次文件摘要 → nbootctl stage
```

**为什么不流式直写槽**：流的摘要只有收完才知道，那时数据已经落地。中途断线留下
的是半写的槽。先落文件，则任何传输失败只损失一个临时文件。（设计来源
`DISTRIBUTION.md`；读码确认）

两道比对抓的东西不同，**不可互相替代**：

| 比对 | 抓什么 |
|---|---|
| 收到的文件 vs 上传方声明的摘要 | 字节有没有完整到达（链路） |
| 介质回读 vs 写入的内容 | 字节有没有完整落盘（存储） |

## `nbootctl stage` 一步做完四件事

`nbootctl_bootctrl_stage(medium, domain, running_slot, path)`（读码确认，
product 树 `app/nbootctl/nbootctl_bootctrl.c`）：

1. 选目标槽。NuttX 域 = **非运行槽**（`1 - running_slot`，运行槽取自启动交接
   记录，不是 `active_slot`）；AMP 域 = 非 `active_slot` 的那个。
2. 容量检查：文件大于分区 → `-EFBIG`，未动介质。
3. **先失效**：目标槽 `priority=0`、`successful=0` 写回 bootctrl。此后任何一步
   失败，能启动的仍是正在运行的那个槽。
4. 写载荷（尾块补零），**从介质回读**重算 SHA-256，与写入时的摘要比对；
   不符 → `-EBADMSG`。
5. 记元数据并**激活**：`priority=15`、`tries_remaining=0`、`successful=0`、
   `image_size`、`image_version = max(A,B)+1`、`sha256`，`active_slot=目标槽`，
   另一槽 `priority` 若 ≥15 则降为 14。

成功输出：`stage nuttx a: <N> bytes, version <V>, activated`。

⚠️ **与 fastboot 路径的差异**：`fastboot flash nuttx_a` 写完 `priority` 保持 0，
激活要另发 `oem board:activate:`（见 [images.md](images.md)）。`nbootctl stage`
**写完即激活**，没有单独的激活步骤——调用它就等于"下次启动换槽"。

## 槽位选择的三个事实

- **只看 `priority`。** 最高者胜，平手偏向 `active_slot`。（读码确认，见
  [bootctrl.md](bootctrl.md)）
- **`tries_remaining` 是死字段。** NuttX 域的启动判决不读它。结构体版 API 与产品
  面板都故意不暴露它——显示一个重试次数等于描述一个不存在的回退。（读码确认）
- **能过校验但起来后挂死的镜像，N-Boot 不会回滚。** 只有**镜像校验失败**时才在
  同一轮启动内落到另一槽。没有启动计数回滚。（实测日志 2026-09-20 "注意"条；
  读码确认）

因此 OTA 之后的安全网是操作层的：

1. 新固件起来后人工确认（面板"确认此版本可用" → `mark-successful`）。
2. 起来后卡死 → 复位后 N-Boot 仍会选同一个新槽（它的 `priority` 最高）。
   退回旧槽要人动手：
   - nsh 还能用：`nbootctl set-active nuttx <旧槽>` 后重启。
   - nsh 不能用：串口强制 panic 触发复位（见 [known-issues.md](known-issues.md)
     #13），复位时持续发 `!` 进 N-Boot 控制台 → `fastboot usb 0` →
     `fastboot oem board:activate:nuttx_<旧槽>`。

   各环节分别实测过；"新固件挂死后按此退回"这条完整流程**未演练，仅推断**。
3. `successful` 只是记录，不影响下次选槽；不要以为"没确认就会自动退回去"。

## 产品实现的 HTTP / WebSocket 流程

位置：`app/nyabula_core/`，由 `CONFIG_NYABULA_CORE_OTA` 控制。未开时 `update.*`
除 `status` 外都按未知 topic 处理——面板据此区分"这份固件不支持这样升级"和
"升级被拒绝"。

### 上传：`POST /ota/upload`

| 项 | 值 |
|---|---|
| 认证 | `Authorization: Bearer <配对/会话 token>` |
| 必带头 | `Content-Length`、`X-Nya-Sha256`（64 位十六进制，大小写均可） |
| 落盘 | `/data/tmp/ota.bin` |
| 大小上限 | 64 MiB（NuttX 槽分区大小），且 `/data` 写完后须留 4 MiB 余量 |
| 形状检查 | 文件第 56..59 字节须为 `ARMd`（arm64 Image 头） |
| 停滞超时 | 30 s 无数据即放弃 |
| 撤销 | `DELETE /ota/upload` → `{"removed":true|false}` |

成功：`200 {"received":<字节数>,"sha256":"<板上算出的摘要>"}`。

拒绝（读码确认；带 * 的两条板上实测确认）：

| 状态 | `error` | 含义 |
|---|---|---|
| 401 | `EAUTH` * | 无 token 或 token 不对 |
| 415 | `ENOTIMAGE` * | 不是 arm64 Image |
| 411 | `ELENGTH` | 缺 `Content-Length` |
| 413 | `ETOOLARGE` / `ENOSPACE` | 超 64 MiB / `/data` 放不下 |
| 400 | `EDIGEST` / `EINVAL` | `X-Nya-Sha256` 缺失或格式不对 / 请求头坏 |
| 409 | `EBUSY` | 已有上传、撤销或写槽在进行 |
| 422 | `EDIGEST`（附 `received`、`sha256`） | 收到的字节与声明的摘要不符，文件已删 |
| 408 / 507 / 500 | `ETIMEDOUT` / `ENOSPACE` / `ESTORAGE` | 停滞 / 写满 / 存储错误 |

拒绝通常发生在浏览器还在发 body 的时候。服务端会先**读掉剩余请求体**
（最多 4 MiB / 2 s）再关连接，否则浏览器看到的是连接复位而不是状态码。

### 应用与确认：WebSocket topic

| topic | 角色 | 作用 |
|---|---|---|
| `update.status` | 任意 | 运行中的固件、A/B 表、apply 进度 |
| `update.apply` `{"sha256": hex}` | owner | **异步**：起线程，重算文件摘要，相符则 `stage` |
| `update.reboot` | owner | 先应答，500 ms 后 `BOARDIOC_RESET` |
| `update.confirm` | owner | 对**运行槽** `mark-successful` |

`update.apply` 立即返回 `started:true`，进度靠轮询 `update.status`。
apply 前会**再算一次**文件摘要：要写的是"此刻的文件"，而 owner 确认的是某一个
特定镜像。

`update.status` 字段：

```
current.version / imageBuiltAt   /data/nyabula/build.json 里的发布号与时间
current.builtAt / os / arch      uname
current.slot                     "a" | "b" | ""（无有效交接记录）
slots[]                          name, active, running, bootable, successful,
                                 priority, version, size
target                           上传会替换的槽（永远不是运行槽）
channel="upload"  online=false   没有在线更新服务
upload                           true = 可以上传（交接记录与 bootctrl 都可读）
maxBytes                         67108864
detail                           人读的说明
apply.state                      idle | writing | done | failed
apply.error                      最近一次失败的正 errno，否则 0
apply.reason                     "" | digest | staged-file | too-large | verify | io | memory
```

**不是 N-Boot 启动的镜像拒绝升级**：没有有效交接记录就不知道运行槽，
也就不知道哪个槽可以覆盖，`update.apply`/`confirm` 返回 `-ENODEV`，
`upload=false`。

**互斥**：上传、撤销、写槽、`confirm`、`reboot` 共用一把 claim。`stage` 在写载荷
期间把 bootctrl 记录持在内存里、最后整份写回，期间任何别的 bootctrl 改动都会被
它覆盖——所以 `confirm` 也必须拿同一把锁。自己写调用方时照此办理。

## 实测数字（2026-09-20，product 固件，经 WiFi）

| 项 | 结果 |
|---|---|
| 无 token | `401 {"error":"EAUTH"}` |
| 非镜像文件 | `415 ENOTIMAGE` |
| 上传 4739608 B | **2.25 s**，返回摘要与本地一致 |
| `update.apply` | 约 2 s 到 `done` |
| apply 后槽位 | `a: active priority=15 version=31`；`b: running priority=14` |
| `update.reboot` 后 | `current.slot=a` |
| `update.confirm` 后 | `successful=true` |

置信：实测确认（设备端全链路）；面板 UI 未目测。

## ⚠️ 经 WiFi 做 OTA 之前：入站 TCP 必须先能用

OTA 是这块板子第一个**入站**大流量场景。此前只测过出站（板子发大文件），
入站从没测过，第一次上板就挂了：

- 现象：4.7 MB 上传到约 1 MB 停住，30 s 后连接被关；300 KB 也要 17.9 s 且无响应。
  `nycore wifi-stats` 显示这 300 KB 期间数据面收了约 586 KB——大量重传。
- 根因：驱动到协议栈的接收队列 `SV6621_NETWORK_RX_DEPTH` 只有 4 帧；TCP 未开
  乱序重组；接收缓冲 32 KiB。
- 处置：RX 队列 4→32；`NET_TCP_OUT_OF_ORDER=y`、`NET_TCP_SELECTIVE_ACK=y`、
  `NET_RECV_BUFSIZE=131072`、`IOB_NBUFFERS=2048`。
- 结果：同一个 4739608 B 文件 2.25 s 传完。

置信：实测确认。

同一天更早修的出站侧问题同样影响面板的 WebSocket（修前并发请求会超时、连接被关；
它对 OTA 轮询的影响是**仅推断**，OTA 是在它修好之后才测的）：`NET_TCP_WRITE_BUFFERS=y`、WebSocket 帧头与负载合成一次 `send`、
accept 后设 `SO_SNDTIMEO`/`TCP_NODELAY`/keepalive。详见实测日志 2026-09-20
"eyes.state.get timed out"条。

**换一块板、换一个 WiFi 驱动时**：先单独测入站吞吐（往板子 POST 几 MB），
不要等 OTA 失败了再回头查网络。

## 结构体版读接口（不解析文本）

`nbootctl_bootctrl.c` 同时是个小库（product 树，`nbootctl_bootctrl.h`）：

```c
int nbootctl_handoff_read(unsigned int *medium, unsigned int *slot,
                          unsigned int *reason, uint64_t *generation);
int nbootctl_bootctrl_snapshot(struct nbootctl_state_s *state);
```

- `nbootctl_handoff_read()`：在两个 generation 字前后各读一次交接头，读到一半的
  头会被拒。`0` 成功；`-ENODEV` 无有效交接；`-EBADMSG` 字段越界。任一输出指针
  可为 NULL。**不打印。**
- `nbootctl_bootctrl_snapshot()`：填 `struct nbootctl_state_s`，不打印。
  无交接记录时返回 `0` 且 `handoff_valid=false`——此时**其余字段都无意义**
  （不知道介质就不知道 bootctrl 在哪块盘上）；bootctrl 本身读不了才返回负 errno。

```c
struct nbootctl_slot_state_s { uint8_t priority; bool successful;
                               uint64_t image_size; uint64_t image_version; };
struct nbootctl_state_s {
  bool handoff_valid;
  unsigned int medium;        /* 1 = SD, 2 = eMMC */
  unsigned int running_slot;  /* 0 = A, 1 = B */
  uint64_t bootctrl_generation;
  unsigned int nuttx_active;  struct nbootctl_slot_state_s nuttx[2];
  unsigned int amp_active;    struct nbootctl_slot_state_s amp[2];
};
```

`tries_remaining` 故意不在结构体里。

**链接注意**：这个源文件每个镜像只能链接一次。开了 `nbootctl` 命令的构建复用
它的目标文件；只有没开该命令的构建才把源文件编进调用方。两处都编会重复定义。
（读码确认，`app/nbootctl/README.md`）

## 草稿分支里的分区级写入（尚未移植）

`DISTRIBUTION.md` 描述的是更大的方案：分发包（`manifest.json` + `install` 字段）
与 `nbootctl write-part` / `write-raw` / `write-gpt`，能从运行中的系统写引导链、
MiniLoader 与分区表。这些动词**只在草稿工作树**
`tmp/amp-compute-draft-20260914/app/nbootctl/` 里，product 树没有。
动词表见 [nbootctl.md](nbootctl.md)。

产品当前实现与该文档的差异（读码确认）：

| `DISTRIBUTION.md` | product 实现 |
|---|---|
| `/ota/begin` → `/ota/upload` → `/ota/commit` 三步 HTTP | `/ota/upload` 一步 HTTP + WebSocket `update.apply` |
| 临时文件 `/tmp/ota.bin` | `/data/tmp/ota.bin` |
| 目标可为任意白名单分区或 LBA 区间 | 只写 NuttX 域的非运行槽 |
| 板端执行者 `write-part` 等 | `nbootctl_bootctrl_stage()` |

草稿动词在实测日志里**没有上板记录**，按"读码确认、未上板"对待。
