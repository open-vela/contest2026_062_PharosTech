---
name: nuttx-usb-host-porting
license: Apache-2.0
description: "将 SoC DWC3/xHCI 接入 NuttX USB Host，排查 PHY/VBUS、Hub 拓扑、设备寻址、ring/context 和 MSC 数据路径。用于主控制器与总线层，不自动包含全部 USB 类驱动。"
---

# NuttX USB Host 与 Hub 移植

先证明控制器和实际端口连线，再解释class driver失败。复用已有xHCI core，SoC层仅提供MMIO、IRQ、PHY/时钟/VBUS attachment。

- 对照控制器能力寄存器确定context stride、scratchpad、ERST等；容量先在足够宽的类型计算再限到实现范围。
- 记录root port、Hub port、route string、TT/MTT和设备速度。64-byte context或SuperSpeed Hub不能按USB2默认解释。
- 从reset/address/configure的command completion与event ring定位停点；PORTSC的W1C/change位不可普通读改写。
- 多TD数据传输检查cache、64KiB边界、Link TRB/CHAIN及completion长度。cancel或detach先停硬件再归还资源。
- 用MSC已知文件做数据验证，再测热拔插/重插和ADB共存；未实现的class不列为Host验收成功。

[阶段检查与K7案例](references/runbook.md)、[固定版本源码](references/sources.md)。交付控制器/Hub/设备拓扑、有效数据和失败后重枚举结果。
