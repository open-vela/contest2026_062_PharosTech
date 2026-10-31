# Nyabula engineering skills
Pharos Tech在RK3576/KICKPI-K7、openvela产品与端侧AI研发中沉淀的24份工程Skill。音乐不在本集合中。

每个目录可单独使用：读取SKILL.md作为入口，按需加载references和scripts。示例参数绑定对应板卡与历史版本，不是跨SoC通用默认值。

| Skill | 适用任务 |
|---|---|
| [rockchip-openvela-porting](rockchip-openvela-porting/SKILL.md) | 将新的 Rockchip ARM64 板移植到 openvela/NuttX，或定位构建、镜像交接、早期串口、GIC/MMU 与 NSH 启动故障。提供 BSP 代码入口、启动契约、故障判别和 K7 实证；已有系统的 N-Boot 刷写恢复使用 nboot-control。 |
| [nboot-control](nboot-control/SKILL.md) | 操作与维护 N-Boot（KICKPI-K7 / RK3576 的 U-Boot 下游发行版）：A/B 槽与 bootctrl 元数据、NuttX/AMP 双域镜像的制作与刷写、fastboot 与串口恢复通道、eMMC/SD 介质选择，在 openvela/NuttX 侧用 nbootctl 读取启动交接、控制重启目标、从运行中的系统写槽（OTA），以及用 bootamp 内存启动迭代 AMP 镜像。用于刷机、槽位切换、OTA、启动排错、系统卡死自救、N-Boot 自身的升级；不用于应用层开发或未确认目标介质的盲写。 |
| [rockchip-clock-pinctrl-debug](rockchip-clock-pinctrl-debug/SKILL.md) | 定位 Rockchip 外设的时钟、门控、复位、电源域或 pinmux 故障，包括冷启动可用但热重启失效、频率设置无效和 MMIO 访问挂起。用于已有控制器接入，不替代整板启动移植。 |
| [arm64-dma-debug](arm64-dma-debug/SKILL.md) | 诊断 ARM64 嵌入式 DMA 的数据损坏、地址不可达、缓存一致性、对齐回退和停止回收竞态。适用于 PL330、IDMAC、ADMA2、xHCI 等已有传输路径。 |
| [combo-sdio-bringup](combo-sdio-bringup/SKILL.md) | 排查 WiFi/BT combo 芯片 CMD5 无响应、SDIO 枚举或固件下载失败，分离 host、供电/控制脚、上电时序与固件协议。以芯片枚举和 ready 为终点，不处理 WPA 联网。 |
| [fullmac-wifi-porting](fullmac-wifi-porting/SKILL.md) | 把已能枚举的 FullMAC WiFi 接入 NuttX/openvela，实现命令事件、Ethernet 数据面、STA/SoftAP、安全握手及故障恢复。适用于 SV6621 等固件协议适配，不用于 SoftMAC mac80211 移植。 |
| [nuttx-bluetooth-hci-porting](nuttx-bluetooth-hci-porting/SKILL.md) | 将蓝牙 HCI transport 和控制器生命周期接入 NuttX/ZBlue，定位 H4、ACL、SCO、ISO 或 profile 建链问题。按协议层验收，不把控制面成功当成可听双向音频。 |
| [sdhci-emmc-bringup](sdhci-emmc-bringup/SKILL.md) | 将 eMMC SDHCI/DW-CM-SHC 从识别和 PIO 推进到 ADMA2、HS200 tuning、HS400 Enhanced Strobe，定位协商成功但慢、CMD21超时及错误恢复问题。不处理出货分区烧录。 |
| [nuttx-codec-audio-bringup](nuttx-codec-audio-bringup/SKILL.md) | 带起或修复 NuttX I2S/DMA/codec 录放，定位位宽/声道/采样率错误、爆音、暂停停止卡住和全双工生命周期。适用于 SAI+ES8388 类路径，不把离线DSP效果当硬件验收。 |
| [nuttx-usb-host-porting](nuttx-usb-host-porting/SKILL.md) | 将 SoC DWC3/xHCI 接入 NuttX USB Host，排查 PHY/VBUS、Hub 拓扑、设备寻址、ring/context 和 MSC 数据路径。用于主控制器与总线层，不自动包含全部 USB 类驱动。 |
| [nuttx-usb-audio-porting](nuttx-usb-audio-porting/SKILL.md) | 在已工作的 USB Host 上接入 UAC1/UAC2 音频，处理 AudioControl/Streaming 归组、等时包、采样反馈、音量与停止回收。分别验收 Speaker OUT、Mic IN 和全双工。 |
| [dual-lcd-te-pipeline](dual-lcd-te-pipeline/SKILL.md) | 构建或排查共享 QSPI 的双 LCD 显示流水，处理 TE 时序、双缓冲、CPU/DMA 所有权、RGB565端序和 LVGL 单线程访问。用于双屏实际刷新，不把Web预览帧率当板测。 |
| [rockchip-csi-camera-bringup](rockchip-csi-camera-bringup/SKILL.md) | 在 Rockchip Linux 计算域带起 MIPI CSI 相机，从芯片ACK、sensor/DPHY/CIF媒体图到连续RAW取帧，定位回调、crop、link_freq和位对齐问题。不宣称NuttX原生CSI或完整ISP/自动对焦。 |
| [arm64-linux-nuttx-amp](arm64-linux-nuttx-amp/SKILL.md) | 建立或排查 ARM64 Linux/openvela 双OS启动，明确 CPU、RAM、GIC、时钟和外设所有权，完成首个mailbox/RPMsg握手。用于双域bring-up，不处理已有链路上的模型业务。 |
| [amp-shared-memory-rpc](amp-shared-memory-rpc/SKILL.md) | 在已启动的异构双OS间实现或排查可靠 RPC 和共享内存大数据，处理 wire ABI、generation、request ID、lease、cache一致性、取消和背压。不限具体AI模型。 |
| [embedded-model-delivery](embedded-model-delivery/SKILL.md) | 将模型、运行库与固件分开版本化，制作和验证带来源/兼容性/哈希的模型包，并处理断点上传、跨域按需拉取及缓存复用。不负责推理数值验证或启动槽烧写。 |
| [rkllm-model-adaptation](rkllm-model-adaptation/SKILL.md) | 定位 RKLLM 模型加载、乱码、chat template、tokenizer、工具输出及性能问题，建立与原模型参考实现一致的token输入链。用于指定模型适配，不假定所有模型共用同一模板。 |
| [rknn-model-conversion-validation](rknn-model-conversion-validation/SKILL.md) | 把ONNX模型或可拆分子图转换到RKNN，验证预处理、布局、动态长度、量化/FP16和CPU/NPU数值一致性。用于指定模型的正确转换，不把转换成功当完整产品效果通过。 |
| [edge-ai-concurrency-benchmark](edge-ai-concurrency-benchmark/SKILL.md) | 设计和分析端侧多模型冷启动、同驻、并发与资源基准，区分wall-clock、CPU时间、PSS、系统可用内存、推理RTF和回调延迟。用于部署/调度决策，不用单模型峰值拼整机性能。 |
| [small-model-device-agent](small-model-device-agent/SKILL.md) | 把能力有限的本地小模型接成可验证的设备Agent，通过确定性意图、单工具补参、权限/审批和事实结果渲染执行动作。用于小模型多工具命中率不足，不替代一般聊天后端。 |
| [nuttx-plugin-runtime-integration](nuttx-plugin-runtime-integration/SKILL.md) | 将现有 QuickJS/WAMR 插件运行时接入 NuttX 产品，处理 provider/Broker、签名授权、异步完成、版本槽和持久化。用于可信本地插件集成，不承诺任意恶意插件的强隔离。 |
| [embedded-local-control-panel](embedded-local-control-panel/SKILL.md) | 把 SoftAP配网、QR入口、自托管HTTP/WebSocket面板和设备状态接成真实嵌入式产品闭环，定位DHCP、重连、认证、TCP吞吐和持久化问题。不把模拟器页面当真机验收。 |
| [embedded-mcp-integration](embedded-mcp-integration/SKILL.md) | 在设备产品中接入MCP客户端或服务器，处理独立凭据、principal/scope、工具发现、逐次审批、异步取消和会话隔离。用于真实设备能力适配，不默认公开监听或连接外部账号。 |
| [openvela-release-reproduction](openvela-release-reproduction/SKILL.md) | 把能在开发树运行的openvela项目整理成可复现的构建交付，固定多仓manifest、未提交补丁、生成资源、外部输入和产物身份。用于CI/干净工作区差异，不重复通用环境安装教程。 |

## 已执行的验证

- 24份Skill结构和107个包内相对链接检查；新增Skill的57处源码引用在固定Git tree核对。
- RK证据解析器18项回归；N-Boot bootctrl自测与刷写工具CLI帮助解析。
- 总包独立解压后重新检查与运行离线脚本；单项包逐文件核对一致。

在仓库根可重跑：

```sh
python3 -B skills/rockchip-openvela-porting/scripts/test_validate_evidence_log.py
python3 -B skills/rockchip-openvela-porting/scripts/validate_evidence_log.py skills/rockchip-openvela-porting/references/skill-validation.md --strict --json
python3 -B skills/nboot-control/scripts/check_bootctrl.py --selftest
python3 -B skills/nboot-control/scripts/nboot_flash.py --help
```

刷写工具的help检查需要pyserial；其他上述测试使用Python标准库。结构检查另使用环境提供的skill-creator quick_validate，不将代理环境工具硬编码为工程依赖。

本轮没有重新构建固件、操作板卡或进行独立模型前向测试。历史案例、宿主测试、目标编译和真机结果分别陈述；验收情境是后续执行标准，不代表已经全部执行。

## 范围与许可

只纳入Skill源码、参考资料与必要脚本，不含模型权重、私有无线固件、vendor库、AI会话日志或机器凭据。Skill不会替代具体任务授权；写盘、发布与PR合入仍遵守项目边界。第三方实现保留其来源，不能由Skill的Apache标记推断外部资产许可。
