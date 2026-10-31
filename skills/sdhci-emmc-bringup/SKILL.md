---
name: sdhci-emmc-bringup
license: Apache-2.0
description: "将 eMMC SDHCI/DW-CM-SHC 从识别和 PIO 推进到 ADMA2、HS200 tuning、HS400 Enhanced Strobe，定位协商成功但慢、CMD21超时及错误恢复问题。不处理出货分区烧录。"
---

# SDHCI eMMC 高速链路带起

先保存已能重复读取的低速/PIO基线和卡身份，再逐层增加总线宽度、DMA、时钟/tuning和HS400。不要一次打开所有高速模式。

- 读取EXT_CSD容量/DEVICE_TYPE/版本/strobe能力，使用目标卡与host共同支持的模式。容量算术和末扇区检查不能溢出。
- 8-bit PIO先对照固定内容；ADMA2再检查可达地址、descriptor长度/边界和cache，保留PIO或bounce fallback。
- 设速要验证实际CRU父源与控制器时钟，不只看SDHCI分频字段。将DLL lock、tuning与普通数据完成语义分别取证。
- HS400ES按支持的协议顺序切换，失败恢复到已验证的High Speed，随后再读数据确认能继续工作。
- 性能记录请求大小、对齐、模式与校验。读取成功不授权破坏性写测试，写入另用明确测试范围。

[K7参数与测试矩阵](references/runbook.md)、[源码入口](references/sources.md)。交付可重复读写/恢复证据，不把卡识别速度或总线标称速率当应用吞吐。
