---
name: nuttx-usb-audio-porting
license: Apache-2.0
description: "在已工作的 USB Host 上接入 UAC1/UAC2 音频，处理 AudioControl/Streaming 归组、等时包、采样反馈、音量与停止回收。分别验收 Speaker OUT、Mic IN 和全双工。"
---

# NuttX USB Audio 录放接入

先保存真实设备完整描述符和USB拓扑，确认Host周期端点可配置；Playing状态不能证明等时DMA在工作。

1. UAC1按AC Header的baInterfaceNr集合绑定接口，UAC2按IAD/功能关系处理。不能假定接口号连续，也不能把独立HID并进音频实例。
2. 解析PCM alternate、速率/位宽/声道、同步类型与反馈，计算每帧有效字节。44.1kHz须允许fractional packet，Mic变长包不能丢跨应用buffer尾部。
3. 沿Mixer/Selector/Feature Unit图寻找音量控制，区分master与逐声道，不能仅看第一个unit。
4. 串行化start/stop/disconnect/close；回调可能仍在飞，繁忙开放句柄需要延迟回收，不能hot-unplug就释放底层内存。
5. Speaker用真实非静音完整文件，Mic用实际输入和回采，duplex单列；缺对端/麦克风时只交已覆盖层。

见[包边界及案例](references/runbook.md)、[证据](references/sources.md)。不扩展为USB全类适配。
