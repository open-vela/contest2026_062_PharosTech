---
name: dual-lcd-te-pipeline
license: Apache-2.0
description: "构建或排查共享 QSPI 的双 LCD 显示流水，处理 TE 时序、双缓冲、CPU/DMA 所有权、RGB565端序和 LVGL 单线程访问。用于双屏实际刷新，不把Web预览帧率当板测。"
---

# 双圆屏 TE 与渲染流水

先确认屏幕真实controller、分辨率、像素格式、QSPI模式与TE接线。K7的GC9B72使用ST77916-family驱动中的专用初始化，不沿用默认controller配置。

- 列CPU绘制、DMA发送、面板扫描三个阶段以及buffer持有者；共享总线调度不能把两块面板的TE视为同一个信号。
- 用稳定图案验证端序、方向、窗口、clear和量化，再测运动/撕裂。renderer与底层只由正确一层维护DMA cache。
- LVGL/Eye只由所有者线程tick/attach/detach；插件/网络提交有界typed命令，不跨线程直接建控件。
- 按render、queue wait、transfer、scan分别计时；优化先针对实测热点。LRU/dirty-region只有底层能利用时才有意义。
- 最后回归来源优先级、lease、blink、scene切换和错误命令；命令入队成功不等于屏幕扫描完成。

[时序、画面与案例](references/runbook.md)、[实现出处](references/sources.md)。交付画面证据和测量口径；SMP/产品并发另测。
