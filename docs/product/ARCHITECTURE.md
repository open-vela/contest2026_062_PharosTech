# Nyabula 系统架构

[产品总览](README.md) · [原生平台](../platform/README.md) · [验证记录](../validation/README.md)

设计中心是**产品状态和执行权归openvela，模型计算按接口协作**。同一套状态、权限和工具服务同时面对语音、浏览器、插件、MCP与Cloud，而不是每个入口分别实现一套设备控制。

## 资源所有权

| 资源 | 管理方 | 契约 |
|---|---|---|
| A53 MPIDR 0..3 | openvela | `CONFIG_SMP=y`，`CONFIG_SMP_NCPUS=4`，默认CPUSET `0xf` |
| A72 MPIDR 0x100..0x103 | Linux | DTB只保留四个A72；裁剪冲突设备 |
| eMMC与产品数据 | openvela | 配置、SQLite、模型和插件；Linux不挂同一文件系统 |
| 双眼、音频、WiFi/BT、USB | openvela | 原生驱动与Core生命周期 |
| 摄像头采集与编码 | openvela | CSI/VICAP/OV5647/VEPU；本轮产品候选已实机采集、编码和预览 |
| NPU与本地模型 | Linux产品计算域 | RKLLM/RKNN及语音服务；原生NPU分支另行验证 |
| 共享GIC | 双方按静态约定 | Linux初始化共享分发器；openvela不重置全局SPI状态 |
| 启动选择与恢复 | N-Boot | 介质、运行域/槽、bootctrl、镜像检查和交接 |

双方共享SoC，依赖可信系统遵守约定；尚未提供恶意内核隔离或任一域独立重启而完全不影响另一域的能力。

## Core 与请求链

```text
语音 / Web / 插件 / MCP / Cloud
          ↓ 身份、请求ID、权限
       Product RPC / Broker
          ↓
 状态与原生服务 ─── Agent与模型请求
          ↓               ↓
  持久化 / 设备动作    文本或工具意图
          └──── 实际结果与事件 ────→ UI / Eye / 播报
```

状态存储使用SQLite和CAS revision，避免旧页面覆盖新状态。异步任务带取消、配额、完成事件和调用方身份；模型回复、工具步骤、审批决定与副作用标记分别保留。B77真实允许/拒绝测试证明执行控制有效，也暴露模型可能只给口头承诺，因此UI以真实执行记录帮助用户判断结果。

主要代码：[`ny_product.c`](../../app/nyabula_core/ny_product.c)、[`ny_access.c`](../../app/nyabula_core/ny_access.c)、[`ny_broker.c`](../../app/nyabula_core/ny_broker.c)、[`ny_agent.c`](../../app/nyabula_core/ny_agent.c)。

## AMP 控制与共享数据

mailbox/RPTUN/OpenAMP/RPMsg承载控制消息，共享窗口承载模型、采集音频和PCM。双方使用相同wire ABI、generation、request ID、sequence、deadline和lease；取消、背压、终止事件与资源归还属于协议，而不是依靠固定等待时间猜测。

| 地址或服务 | 用途 |
|---|---|
| `0x47800000..0x47a00000` | RPMsg vring保留区 |
| `0x47a00000..0x47c00000` | RPMsg DMA pool |
| `0x47c00000..0x48000000` | 4 MiB服务共享区 |
| `0x4a400000`起16 MiB | AMP openvela镜像契约区 |
| `0x70000000`起256 MiB | Linux no-map，openvela第二堆 |
| HEALTH / LLM / BLOB | 健康/生成/按逻辑名称拉模型 |
| KWS / ASR / TTS | 流式唤醒、识别与合成 |
| VISION / MEDIA / SPEAKER | 本轮候选支持视觉计算与声纹服务；声纹真人登记和匹配尚未验收 |

Linux摄像头、CIF/ISP/VPSS、SCCB及相应MMU/VEPU节点由所有权脚本检查和裁剪；FIT打包也检查最终DTB。详见[当前计算域边界](../../tools/amp/COMPUTE_CURRENT.md)与[协议](../../tools/amp/protocol/README.md)。

## 模型与固件独立交付

模型在openvela的 `/data/models`，Linux通过BLOB按名称拉到tmpfs。固件、模型、tokenizer/lexicon和vendor runtime分别版本化与校验，避免固件升级重复携带近GB权重。

4 MiB共享区曾双向校验1,048,572 words一致；875,760,324-byte模型传输段约44 MiB/s，首次包含完整摘要的流程82.839秒，缓存复用7ms。三者属于不同测量阶段。

包工具检查路径、大小、摘要、来源和compatibility；外部模型源revision与许可缺口继续在清单中保留。参见[模型交付](../amp-model-delivery.md)、[资源包](../../tools/amp/resources/README.md)。

## 语音与媒体调度

openvela采集16kHz S16到预录环；KWS命中后ASR从对应偏移开始，识别结果交给Agent，回答经G2P/TTS返回PCM。默认CODEC_HALF播报时闭麦，以符合ES8388录放格式约束。

云语音采用有界生产者/消费者队列：网络线程接收PCM，播放线程独占重采样和输出；首批PCM前不启动静音流，短间隙按队列余量补给，退出先唤醒并join生产者。B86对应修复有长句与低音实际验收。

文件、A2DP和TTS共享DSP输出，双设备时钟按队列反馈校正，播放暂停采用软件契约而非调用不存在的USB硬件pause。HFP保持独立8/16kHz路径，真实通话仍待验证。参见[音频服务](../audio-plugin-services-20261018.md)与[云语音验收](../voice-stream-validation-20261101.md)。

## 插件和 Agent 创作事务

插件包是签名manifest、源码/字节码与受限资源集合。安装、信任、授权、运行、更新与回滚分开；声明能力不会自动授予权限。成员凭据在后端鉴权，角色变更/撤销使旧会话失效。

Agent只在主人开启后生成自己签名的 `agent-*` 工具；使用 `baseVersion` 防止覆盖新版本。候选在数据副本上启动和发布页面，失败恢复原代码、数据及授权。真实事件测试从已发布输入和绑定产生，禁止使用UI里不存在的隐藏字段制造“通过”。

## 外部连接边界

- **Cloud**：设备主动连接；Cloud账号认领与设备密码鉴权是两层关系。WSS验证证书链与主机名。Cloud终止TLS，属于可信服务，并非端到端加密。
- **入站MCP**：默认loopback，ADB/SSH隧道访问；限期scope凭据、隔离聊天、撤销/到期拒绝；不能代替主人审批。
- **出站MCP**：HTTPS发现工具，调用参数逐次审批；schema变化需重确认，结果不确定的写操作不自动重试。
- **HTML插件页**：隔离iframe，声明事件、受控消息和宿主视图；它不是任意设备API通行证。

## 启动 升级与服务恢复

N-Boot选择普通或AMP域与槽，并发布handoff。Core/nbootctl读取当前身份，OTA写非运行槽，重启后确认；模型和用户数据使用独立分区。Fastboot、已知好槽和CLI构成恢复入口。

Web服务对可报告的监听错误退避重建，主动停止不自动拉回。它不覆盖强制杀死整个任务或内存破坏；B88另有一次旧轮日志缺失的突发重启记录，整机长稳仍需独立验证。
