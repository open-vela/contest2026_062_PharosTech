---
name: amp-shared-memory-rpc
license: Apache-2.0
description: "在已启动的异构双OS间实现或排查可靠 RPC 和共享内存大数据，处理 wire ABI、generation、request ID、lease、cache一致性、取消和背压。不限具体AI模型。"
---

# AMP 共享内存与业务 RPC

先以双方真实实现定义wire合同：定长整数宽度/端序、最大长度、方向、状态和终止事件，不直接发送编译器C结构体。

- 将控制帧与共享数据窗口分开；记录唯一allocator、地址/长度边界、lease及谁能写header。不要让诊断pattern覆盖arena元数据。
- generation代表对端服务实例，request ID代表该实例中的请求；旧实例事件不能完成新请求。同ID双向请求还需按方向/类型区分。
- 检查两域对同物理页的cache属性和同步，单侧自检不够；双向使用已知pattern、序号与hash验证。
- 分片按首片固定total/参数、连续offset及整体上限，乱序/重复/取消/超时给出明确语义；终止事件恰好一次。
- 对RESPONSE/EVENT不再回错误响应，避免双端错误帧互弹。消费者有界队列满时反映背压，不无限堆积。

[协议与错误案例](references/runbook.md)、[源码定位](references/sources.md)。交付双方合同、边界测试和真实跨OS数据证据；模型包兼容与分发另处理。
