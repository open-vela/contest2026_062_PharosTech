---
name: arm64-linux-nuttx-amp
license: Apache-2.0
description: "建立或排查 ARM64 Linux/openvela 双OS启动，明确 CPU、RAM、GIC、时钟和外设所有权，完成首个mailbox/RPMsg握手。用于双域bring-up，不处理已有链路上的模型业务。"
---

# ARM64 Linux 与 openvela AMP 启动

先输出两域所有权表，再改启动代码。CPU分组、启动EL/寄存器、内存保留、共享中断控制器和外设关闭范围必须来自同一版本的启动器、DTB及固件。

1. 从已验证单域启动保留可恢复基线，确认CPU_ON/MPIDR和每核栈，不靠改镜像load地址强行拼双系统。
2. Linux DTB只暴露它拥有的CPU与设备；NuttX跳过另一域负责的GIC distributor全局重置，只处理自己可拥有的接口/路由。
3. 保留BL31/OP-TEE、双方镜像/heap、共享内存、DMA池，检查运行时大小而不只看文件字节数。
4. 建立启动顺序及握手条件，确认mailbox RX使能、首个kick、vring/resource table与动态端点发现。
5. health/info成功后才逐个加入产品外设，记录clock/PD/IRQ的共同依赖与冲突。

查[ABI2案例与验收门](references/runbook.md)、[固定源码](references/sources.md)。已有N-Boot介质写入/恢复任务另用nboot-control；本Skill不默认刷槽。
