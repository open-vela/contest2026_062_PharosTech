# 第一部分 RK3576 原生 openvela 适配

[返回参赛主页](../../README.md) · [Nyabula 产品](../product/README.md) · [能力全表](../CAPABILITIES.md) · [构建运行](../GETTING_STARTED.md)

本赛道的成果是一套可独立使用的 RK3576 / KICKPI-K7 openvela 平台。驱动、配置和测试入口既支撑 Nyabula，也可以被其他基于同类硬件的项目复用。

## 从第一行串口到多媒体平台

移植以 openvela 已有 RK3399 ARM64 支持为参考。通过原厂 DTS、TRM、串口与板上观察确认 RK3576 的地址、中断、时钟和启动契约，依次完成 UART putc、异常级处理、GICv2、页表/Cache、PSCI、generic timer 和 NSH。

普通 openvela 镜像入口为 `0x40200000`；产品 AMP 使用另一套 `0x4a400000` 地址契约。产品固定四 A53，Linux 只保留四 A72。B23、B66 有两个系统各自四核运行的启动证据，旧 UP 产品固件不作为 4+4 验收依据。

芯片与板级通过 `ARCH_CHIP_CUSTOM` / `ARCH_BOARD_CUSTOM` 和 manifest linkfile 接入。最终产品还使用配套 NuttX/external/ZBlue fork 与依赖补丁，构建入口统一收在本仓；芯片事实、板级接线与通用设备协议分别维护。

## 芯片与基础控制器

| 模块 | 实现与用途 | 入口 |
|---|---|---|
| ARM64 系统 | 入口/EL、GIC-400/GICv2、MMU、Cache、PSCI、timer、SMP/AMP | [chips/rk3576](../../chips/rk3576) |
| UART | UART0 早期 putc 与完整 DW16550；1500000 8N1 调试 | [UART 适配 #20](https://github.com/open-vela/contest2026_062_PharosTech/pull/20) |
| GPIO / IOMUX | 五 bank、输入输出、中断、引脚复用与公共 API | [GPIO #65](https://github.com/open-vela/contest2026_062_PharosTech/pull/65) |
| CRU | 时钟树、父源/分频/门控，CPU/总线/UART/SAI/存储等依赖 | [时钟 #69](https://github.com/open-vela/contest2026_062_PharosTech/pull/69) |
| I2C / SPI / PWM / FSPI | I2C 事务、SPI、PWM v4、双屏 QSPI | [芯片源码](../../chips/rk3576) |
| DMA | PL330 microcode、分配与生命周期；IDMAC/ADMA2 与缓存/对齐约束 | [DMA #45](https://github.com/open-vela/contest2026_062_PharosTech/pull/45) |
| 时间与环境 | HYM8563/PCF8563 RTC、SARADC、TSADC、timer/watchdog | [板级初始化](../../boards/rk3576/kickpi-k7/src) |
| 电源 | RK806 buck/LDO 开关、电压读写与基础 regulator 电源树 | [RK806 #61](https://github.com/open-vela/contest2026_062_PharosTech/pull/61) |

RK806 已于 2026-10-27 合入。作者记录了 regulator 电压读取和 VCCIO SD 切换到 1.8 V；电源树仍有扩展空间，discharge 等功能未纳入本次实现。

## 存储与持续调试

SDMMC 从 PIO 推进到 IDMAC，支撑最初的 FAT `/data` 和 `k7flash`/Ymodem 热更新。eMMC 实现 SDHCI、8-bit PIO、ADMA2、HS200 tuning、HS400 Enhanced Strobe 和 DMA-safe bounce，避免未对齐缓冲区反复回退 PIO。

产品使用 GPT、独立配置/数据分区和启动槽，空白 data 首启创建文件系统。双圆屏与 SD 共享 FSPI 引脚，因此双屏配置使用 eMMC。旧 SD 固定扇区刷写工具用于对应历史布局；当前产品按 N-Boot、nbootctl 和包内槽契约更新。

## 原生无线与有线网络

### SV6621 FullMAC WiFi

真实 combo 为 SeekWave SWT6621S / SV6621，chip ID 为 SV6160LITE。团队通过丝印、Android 日志和 SDIO VID/PID 纠正早期 DTS 中 RTL8822CS/AP6256 的错误标签，建立协议核心与 RK3576 transport 分离的 Apache host 驱动。

已实现固件装载/校准、CMD52/CMD53、控制事件、Ethernet 数据面、STA/SoftAP、WPA2/WPA3-SAE、PMF、密钥更新、隐藏 SSID、双频扫描、连接态扫描、漫游和固件自恢复。CMD5 的突破来自上电时钟与 combo 环境组合调整，经验保留在 [combo-sdio-bringup](../../skills/combo-sdio-bringup/SKILL.md)。

### 蓝牙与千兆以太网

蓝牙与 WiFi 共用 SDIO combo，完成 H4、ACL/SCO/ISO transport 和 NuttX/ZBlue 接入，独立 profile 包含 BLE/GATT、Classic、RFCOMM/SPP、A2DP/AVRCP 及 HFP 部分链路。产品级双设备音乐体验见[第二部分](../product/README.md#音频与蓝牙音乐)。

千兆网由 RK3576 GMAC 与 MAE0621A PHY 驱动组成。[#101](https://github.com/open-vela/contest2026_062_PharosTech/pull/101)作者使用板上双网口、单根网线做回环，完成千兆协商、ping 和 30 分钟 TCP 大批量传输。此能力以 `configs/ethernet` 独立交付。

## 原生显示与触控

### 双圆屏

两块 GC9B72，360×360 RGB565；FSPI 总线共享、GPIO TE、双缓冲及 CPU/DMA 流水。ST77916-family 驱动采用 GC9B72 初始化序列。显示调度与 Eye 内容引擎分开，便于独立验证。

*B23 从真实设备渲染页读取的左右眼。物理面板扫描与页面镜像是不同观测点。*

### VOP HDMI MIPI DSI 与 GT911

[#99](https://github.com/open-vela/contest2026_062_PharosTech/pull/99)新增相关时钟、DCPHY 的 DPHY 部分、HDPTXPHY、MIPI DSI、HDMI、VOP 与 GT911 触摸。VOP 当前为 **esmart0 透传**，多层混合等效果尚未实现；这不是 Mali GPU 驱动。独立配置为 `configs/hdmi`、`configs/mipi_dsi`。

## openvela 原生摄像头与 H264

[#100](https://github.com/open-vela/contest2026_062_PharosTech/pull/100)交付的链路：

```text
OV5647 → CSI0 DPHY / VICAP → 软件 NEON 解拜耳与 NV12
       → VEPU510 H.264 → Annex-B / fMP4 → WebSocket → 浏览器 MSE
```

- 控制器：CSI DPHY、VICAP 与 OV5647，当前仅 CSI0。
- 图像处理：软件解拜耳，以及自动曝光、增益、白平衡闭环。
- 编码：NV12 输入、H.264 输出、固定 QP，提供 V4L2 接口。
- 应用：`camenc` 独立推流示例，支持模式与曝光/增益/白平衡控制。
- 默认模式为 1280×720@30；1920×1080 的编码对齐高度为 1088，使用拷贝路径；对齐条件满足时可直接交接缓冲区。

尚未实现硬件 ISP、CSI1—4、H.265、码率控制/AQ/ROI。模式表是传感器/配置能力，实际输出帧率还受软件图像处理与网络服务影响。

**当前资源归属：采集和编码由 openvela 管理，Linux 相机节点已裁剪。** 独立 camera 配置已交付；本轮 `product_amp` 候选完成采集、编码与预览的实机验证，远程媒体仍待联调。详见 [COMPUTE_CURRENT](../../tools/amp/COMPUTE_CURRENT.md)、[camenc 使用说明](../../app/camenc/README.md)、[VEPU 接口](../../chips/rk3576/vepu/README.md)。

## 音频 USB 与灯带

- **SAI / PL330 / ES8388**：录音、播放、同格式全双工、双麦、单双声道、16/24/32位和输出路由；历史采集矩阵覆盖66项。
- **USB Device**：DWC3 UDC / ADB；SMP IRQ临界区、复位传输资源和重连生命周期均有后续修复。
- **USB Host**：DWC3/xHCI、USB2/3、GL3510 Hub、MSC/exFAT、自动挂载。
- **USB Audio**：真实 Speaker OUT 已验证并接入产品低音；Mic IN 与 UAC 全双工仍需真实设备验收。
- **WS2812**：SPI3 字符设备 `/dev/ws2812`、RGB整帧写入、实际SPI频率计算脉宽、空闲拉低且释放总线；[配置与测试程序](../../configs/ws2812/README.md)。

## 启动与恢复

[N-Boot](https://github.com/BeaconCat/N-Boot)是 U-Boot 的 K7 下游发行版，配合本仓 [nbootctl](../../app/nbootctl/README.md)提供双介质、bootctrl、NuttX/AMP A/B、镜像校验、Fastboot、stage/clone/confirm、运行态交接与升级。

N-Boot #9/#10/#11 仍为开放 PR，最新 AMP 维护功能需要配套启动器版本。槽校验回退、运行态确认和启动器原位更新各有不同故障模型；不要将其统一称为断电原子升级或 OS 挂死自动回滚。

## 代表性实测

| 项目 | 结果 | 条件与证据 |
|---|---|---|
| eMMC读取 | 75.5 MiB/s；PIO对照12.6 MiB/s | 64 MiB、512 KiB请求、只读；[#54](https://github.com/open-vela/contest2026_062_PharosTech/pull/54) |
| 以太网 | 30分钟、29.4 GB，无丢包/误码记录 | 双网口回环，作者板测；[#101](https://github.com/open-vela/contest2026_062_PharosTech/pull/101) |
| WiFi SoftAP | 20/20通过 | WPA2/WPA3控制与数据面；[#60](https://github.com/open-vela/contest2026_062_PharosTech/pull/60) |
| USB3 BOT | 峰值约36 MiB/s | 指定ASMedia/Hub，不含UAS；[#68](https://github.com/open-vela/contest2026_062_PharosTech/pull/68) |
| ADB | 260次帧拉取、8 MiB双向传输通过 | B24；含并发、主机重连、板重启；近期仍有延迟offline、原因未定；[记录](../validation/README.md#平台与传输) |
| Codec | 66项检测到目标基频 | 两项右路削顶仍需归因；[#58](https://github.com/open-vela/contest2026_062_PharosTech/pull/58) |
| Eye渲染 | 每屏每帧40→4 ms | 1.8 GHz指定场景；[#86](https://github.com/open-vela/contest2026_062_PharosTech/pull/86) |

## 原生 NPU 实验路线

独立 `feat/native-npu` 分支包含驱动、Mesa MIT 卷积编码器适配、矩阵乘映射、`/dev/npu0` 和 `rknpu` 命令。已有7种形状的命令字节/数值离线对照与产品编译记录；**未上板验证实际寄存器、中断或数值**。它不进入当前“原生NPU已可用”的能力清单，也不与产品 Linux NPU 同时认领硬件。

## 复用与工程沉淀

按需从 [15套配置](../GETTING_STARTED.md#选择配置)进入。通用方法收录在[工程 Skill](../../skills/README.md)：启动契约、时钟/引脚、DMA、combo上电、FullMAC、蓝牙HCI、eMMC、音频、USB和双屏。历史 Linux 相机案例仍有取证价值，但当前所有权与操作入口以本页和 `COMPUTE_CURRENT.md` 为准。
