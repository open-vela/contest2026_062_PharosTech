---
name: fullmac-wifi-porting
license: Apache-2.0
description: "把已能枚举的 FullMAC WiFi 接入 NuttX/openvela，实现命令事件、Ethernet 数据面、STA/SoftAP、安全握手及故障恢复。适用于 SV6621 等固件协议适配，不用于 SoftMAC mac80211 移植。"
---

# FullMAC WiFi 原生驱动移植

先确认芯片确为FullMAC、host已具备可验证的传输原语，再划分协议核心、OS适配、transport与板级电源。参考来源的许可和二进制固件许可分开核对。

- 先闭环命令ID/序号/响应/事件及ready状态，再接scan、关联、授权和Ethernet TX/RX；关联事件不等于密钥安装或DHCP完成。
- 建立连接generation和命令寿命，断线、换AP、固件重启后旧事件不得推进新连接。TX信用/背压不能只以发送函数返回值判断。
- 优先用现有系统接口表达模式、密钥、频率和状态；避免“实现WPA3”却由CLI默默改回WPA2。
- 将STA与AP逐peer状态、EAPOL/replay、PMF/IGTK与rekey分开验证；遵循现有经过验证的密码实现，不自创算法。
- firmware assert/命令超时/SDIO错进入异步恢复，重建数据面与用户可见状态。恢复工作不能堵在它自己依赖的单线程队列。

[验收矩阵与SV6621案例](references/runbook.md)用于裁剪本轮范围；[源码](references/sources.md)用于复用。

交付能力矩阵和具体对端/配置/时长。没有测试的模式标未验，不从连接速率外推吞吐。
