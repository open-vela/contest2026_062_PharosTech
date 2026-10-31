---
name: combo-sdio-bringup
license: Apache-2.0
description: "排查 WiFi/BT combo 芯片 CMD5 无响应、SDIO 枚举或固件下载失败，分离 host、供电/控制脚、上电时序与固件协议。以芯片枚举和 ready 为终点，不处理 WPA 联网。"
---

# 组合无线芯片 SDIO 带起

先核验实物芯片身份、板版本及电源/复位拓扑。DTS标签和模块商品名不是chip-id。

1. 读取host原始错误，区分RTO、CRC、命令完成与数据阶段；确认命令真正发出。不要只看上层-110。
2. 对照可工作的系统，从芯片上电前到枚举完成连续看clock、enable、reset、BT侧控制脚及UART空闲状态。静态寄存器相同不排除时间窗口差异。
3. 以低速保守模式完成工作条件协商、RCA/select、function enable、block size及最短芯片ID读取。高速/tuning不是枚举前提。
4. 再实现固件下载需要的CMD52/CMD53原语；核对byte/block模式、fixed/increment地址、对齐、长度与下载后校验/ready事件。
5. 执行冷启动、热重启、重复下载及失败后恢复。WiFi/BT共享复位不可在另一服务运行时随意拉低。

见[分层步骤及反例](references/runbook.md)、[证据出处](references/sources.md)。只缺写盘/接线关键参数时补问，其余先查源码和只读证据。
