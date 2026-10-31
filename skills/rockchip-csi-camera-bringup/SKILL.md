---
name: rockchip-csi-camera-bringup
license: Apache-2.0
description: "在 Rockchip Linux 计算域带起 MIPI CSI 相机，从芯片ACK、sensor/DPHY/CIF媒体图到连续RAW取帧，定位回调、crop、link_freq和位对齐问题。不宣称NuttX原生CSI或完整ISP/自动对焦。"
---

# Rockchip CSI 相机连续取帧

先核对模组与转接板实际引脚、供电/复位/XCLK、lane和I2C身份。传感器裸片手册不等于具体模组接线。

1. 分别记录sensor probe、subdev、media graph、capture node与STREAMON；每阶段失败保留原始errno/驱动日志。
2. 对照目标vendor内核需要的bus/interval/crop/link_freq回调，不将主线sensor能编译视为已适配CIF链。
3. 使用真实协商后的format、width/height、stride、buffer size、Bayer顺序与RAW位布局解析，不能按文件名猜10bit packing。
4. 连续采集序号和时间戳，先在RAM有界取样，再保存；存储吞吐与采集丢帧分别计量。
5. 数据正确后才调曝光/AWB/焦点，区分板上处理和PC去马赛克/gamma/编码。未核实的VCM使能不能盲写。

[OV5647分层案例与验收](references/runbook.md)、[出处](references/sources.md)。交付RAW原件、准确解码说明和序列统计，而不只是一张看起来正常的PNG。
