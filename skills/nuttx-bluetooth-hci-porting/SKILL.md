---
name: nuttx-bluetooth-hci-porting
license: Apache-2.0
description: "将蓝牙 HCI transport 和控制器生命周期接入 NuttX/ZBlue，定位 H4、ACL、SCO、ISO 或 profile 建链问题。按协议层验收，不把控制面成功当成可听双向音频。"
---

# NuttX 蓝牙 HCI 与 Profile 接入

从控制器ready、Reset/Version/NVDS开始，确认host栈所需HCI类型、MTU和流控，再测试具体profile。WiFi/BT combo还要定义共享firmware重启时的offline/re-init合同。

1. 对照NuttX lower-half、H4 framing、ZBlue构建入口，校验command/event与ACL/SCO/ISO各自的长度和buffer池。
2. 带错误类型字节、分段包和背压验证重同步；不得将字节丢失后的任意位置当新包头。
3. 从BLE双向GATT、Classic配对/SPP逐步选择本次需要的profile。每层记录控制连接、媒体数据、PCM路由三种结果。
4. profile callback与host锁的上下文要明确；不能在锁内回调启动操作而形成重入死锁。
5. stop/recovery前协调已打开HCI、在途命令和RX线程；等旧使用者退出再关闭SDIO service。

查[能力分层及对端限制](references/runbook.md)和[来源](references/sources.md)。

交付每项profile的已实现/已编译/空口/音频结果。遇到对端没有所需profile或媒体源时，停在可证明层，不编造双向完成。
