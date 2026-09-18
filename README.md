# Nyabula — 基于 openvela 的 RK3576 桌面 AI 陪伴猫

> 2026 首届 openvela AI 硬件开发者大赛 · 队伍 #062 PharosTech
> 赛道：**新硬件适配**（RK3576 / KICKPI-K7 全新芯片 BSP）+ **AI 硬件产品创新**（端侧 AI 陪伴桌宠）

## 一、作品简介

Nyabula 是一只活在桌面上的 openvela 猫：两块圆屏是它的眼睛，openvela/NuttX 掌管人格、双屏表情、语音、媒体、定时、设备与权限；Linux 只作为 AMP 计算协处理器（NPU LLM / ASR / TTS / 摄像头），随时可拔、可降级。

为此我们把 openvela 完整移植到 **RK3576（4×A72 + 4×A53，NuttX 此前无该芯片支持）**，从零写出 `chips/rk3576` 与 `boards/rk3576/kickpi-k7`，并做了一批**可复用到其他 openvela 设备**的扩展：

| 领域 | 内容（★ = 板上实测确认） |
|---|---|
| 芯片/板级 | ★ 启动到 NSH（MiniLoader→BL31→OP-TEE→NuttX BL33）、GICv2、MMU、PSCI、SMP、时钟树框架、GPIO/IOMUX、UART、I2C、SPI、PWM、SARADC、TSADC、WDT、RTC、DMA、FSPI |
| 存储/引导 | ★ SDMMC / eMMC host、GPT+FAT `/data`；★ 清洁室 **N-Boot**（U-Boot proper）双域 A/B 启动 + bootctrl；★ 板上热更新 `k7flash`/Ymodem，不拔卡迭代 |
| 无线 | ★ SV6621（SWT6621S）**Apache 清洁室原生 FullMAC WiFi 驱动**：STA WPA2/WPA3-SAE/PMF/hidden/漫游/后台扫描/固件自恢复，**SoftAP** Open/WPA2/WPA3；★ 同芯片 SDIO 共道 **蓝牙**：BLE/GATT/SMP、Classic、A2DP/AVRCP/SPP，HFP 进行中 |
| USB | ★ RK3576 xHCI 主机、USB 音频/大容量自动挂载、ADB |
| 音频 | ★ SAI + ES8388 立体声出声、`audioctl` 路由 |
| 显示 | ★ 双 GC9B72 圆屏（ST77916 驱动，FSPI，TE 同步）、LVGL、**Core Eye** 13 表情 / 25 场景、NEON 优化 |
| Nyabula Core | ★ 插件运行时（WAMR/QuickJS）、Permission Broker、签名/吊销；◐ 原生产品服务：计时/闹钟、媒体、天气、简报、主动陪伴、Web 控制台、**基于官方 `ai_agent` 的 Nyabot** 与 MCP Server（PR #91） |
| AMP | ★ 4×A53 openvela + 4×A72 Linux 共存，RPMsg 控制面，★ 4 MiB 共享内存双向零误差；◐ ASR/TTS/LLM 计算服务协议（PR #87）；旧探针镜像 ★ MiniCPM-1B RKLLM、sherpa 中文 ASR、Melo TTS 三模型同驻、OV5647 1080P 连续帧 |

置信分级贯穿全部文档：`实测确认 / 编译通过 / 仅推断`，原始串口证据见 `实测日志.md`（交付包内）。

## 二、选题方向

- **新硬件适配**：RK3576 是 NuttX/openvela 全新芯片；BSP、驱动、清洁室 WiFi/BT、N-Boot A/B 均可反哺上游。
- **AI 硬件产品创新**：基于 openvela `ai_agent` 的端侧陪伴智能体，Linux 仅做计算卸载，符合"产品主逻辑运行在 openvela"的判定。

## 三、目录结构

```
chips/rk3576/                  RK3576 芯片层（arch 级驱动：clk/gpio/i2c/spi/sdmmc/emmc/usb/sai/dma/rptun…）
boards/rk3576/kickpi-k7/       板级：boardinit、LCD、音频、WiFi/BT transport、USB、存储；configs/{nsh,amp}
drivers/                       自研设备驱动：sv6621（WiFi/BT）、st77916（圆屏）、pcf8563
configs/                       项目配置：dev（WiFi/BT/音频/USB）、display、core_eye；product（全功能，进行中）
app/nyabula_core/              Nyabula Core（插件运行时、权限、产品服务、Nyabot、MCP）
app/nyabula/ app/nyabula_display/  Eye Engine 与双屏显示服务
app/nyampctl/ tools/amp/       AMP：openvela 侧控制、Linux 侧 nyampd、协议、DTS、FIT 打包、验证脚本
app/k7flash/ app/nbootctl/ app/audioctl/ app/usbsw/  板上工具
tools/k7_pack/ tools/k7_abpack/ tools/k7_ota/      SD/eMMC 线刷包、bootctrl、PC 端 OTA
logs/                          AI Coding 日志
```

## 四、运行方式

```sh
# 1. 拉工程
repo init -u https://github.com/open-vela/contest2026_062_PharosTech -b dev-ai-contest-2026 -m contest2026_062_PharosTech.xml
repo sync -c -j8

# 2. 编译（在工作区根目录）。三个常用配置：
./build.sh contest2026_062_PharosTech/configs/dev -j8        # WiFi/BT/音频/USB/ADB
./build.sh contest2026_062_PharosTech/configs/core_eye -j8   # 双屏 + Core + Eye（先跑 app/nyabula/tools/generate_fonts.py）
./build.sh contest2026_062_PharosTech/boards/rk3576/kickpi-k7/configs/amp -j8   # AMP 域（A53）

# 3. 打包与烧录
cd contest2026_062_PharosTech/tools/k7_pack
./fetch_rkbin.sh rkbin
./build_emmc.sh ../../../nuttx/nuttx.bin ../../boards/rk3576/kickpi-k7/nboot rkbin out-emmc   # RKDevTool 线刷包
./build_sd.sh   ../../../nuttx/nuttx.bin ../../boards/rk3576/kickpi-k7/nboot rkbin out-sd     # SD 整盘镜像
# 已烧板后的日常迭代：python3 tools/k7_ota/k7_ota.py --port COMx --img <fit>   (串口 1500000 8N1)
```

评委可直接使用交付包中的 **eMMC 聚合线刷包**（RKDevTool：MiniLoaderAll.bin + parameter.txt + 各分区镜像，含 SHA256SUMS）。

## 五、进行中的 PR（未合入，供评审）

- #87 AMP 板载 AI 与摄像头计算服务（共享内存 ★、ASR/TTS 协议、AMP 域音频）
- #91 Nyabula Core 原生产品服务（计时/媒体/天气/Nyabot/Web/MCP）
- BeaconCat/N-Boot #9 AMP 槽启动修复（★）

## 六、AI Coding 使用说明

全流程由 AI 编码代理（Codex / Claude Code / OpenCode）驱动，人类负责决策、上板与验收：

- **调研**：反编 vendor DTS 取基址/中断、TRM 交叉核对、多智能体并行评估 WiFi 芯片许可路线（最终识别真芯片为 SV6621 并选择 Apache 清洁室路线）。
- **编码**：芯片层与板级驱动、SV6621 FullMAC WiFi/BT 协议栈、N-Boot A/B、Core/Eye、AMP 协议均由代理生成，遵循"一 commit 一事""英文代码注释/中文内部文档""实测/编译/推断三级置信"规约。
- **调试**：代理直接操作串口/OTA/构建机进行上板回归（WiFi 20 轮连接、SoftAP 20 轮启停、BLE 100 轮 GATT 等），错误原文回填 `实测日志.md`。
- **沉淀**：全流程浓缩为可复用的「Rockchip SoC → openvela 移植」Skill。

完整对话日志见 `logs/`。
