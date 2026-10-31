---
name: nuttx-codec-audio-bringup
license: Apache-2.0
description: "带起或修复 NuttX I2S/DMA/codec 录放，定位位宽/声道/采样率错误、爆音、暂停停止卡住和全双工生命周期。适用于 SAI+ES8388 类路径，不把离线DSP效果当硬件验收。"
---

# NuttX Codec 音频录放排错

固定一个已知音源、音量、接线和格式，先验证采集/播放分别正常，再做兼容格式双工。

1. 在WAV/PCM解码、音频upper-half、codec、SAI、DMA之间逐层核对sample bits、container bits、channels、frame bytes及实际时钟。
2. 把自然EOF、stop、pause/resume、错误后的再次start分别跑通；入队失败时谁归还buffer必须明确，不能让播放器永久等不存在的完成消息。
3. 输出路由/功放/耳机检测留板层，codec硬件控制走公共ioctl。录放共享资源不能在单向shutdown时破坏另一方向。
4. 起播/起录瞬态按时间窗采样，区分数字满幅、模拟偏置和功放使能。静音预热后的改善不证明首次上电也无爆音。
5. 分格式记录数据、频率、削顶、线程/heap及听感；两项失败不能被其他项目通过抵消。

用[格式和生命周期矩阵](references/runbook.md)选择回归，用[来源](references/sources.md)复用已证实边界。交付实际覆盖配置和未测模式。
