---
name: embedded-local-control-panel
license: Apache-2.0
description: "把 SoftAP配网、QR入口、自托管HTTP/WebSocket面板和设备状态接成真实嵌入式产品闭环，定位DHCP、重连、认证、TCP吞吐和持久化问题。不把模拟器页面当真机验收。"
---

# 嵌入式配网与本地控制面板

先以设备端状态机作为权威，浏览器只是客户端。确认实际固件启动了network/Core/Web/Eye服务及所需文件系统。

1. 为未配网、连接中、已联网、错误回退定义状态/超时/重试；错误SSID或密码不得把设备留在永久不可达状态。
2. SoftAP验证真实DHCP租约、网关和HTTP可达，再验证QR与手机操作。每层拿实际设备证据，不只看AP广播。
3. 持久化凭据/设备名与默认恢复分开，校验目标FAT长文件名/目录错误语义，重启后重新确认；不把内存里JSON更新当落盘。
4. HTTP/WS framing、backlog、写缓冲/乱序/SACK与驱动RX队列逐层测；LAN直连和ADB转发是不同链路，分别定位。
5. 认证与CAS revision以同一Core实现为准；UI只有成功响应后更新状态，提交失败不得清掉设备侧visible标志。

[状态与网络反例](references/runbook.md)、[源码](references/sources.md)。交付手机配网至重启重连的过程和并发/错误用例；外网云中继另做。
