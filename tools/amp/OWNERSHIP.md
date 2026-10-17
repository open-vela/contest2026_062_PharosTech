# 最小AMP所有权

> 当前计算服务与摄像头所有权以 [COMPUTE_CURRENT.md](COMPUTE_CURRENT.md) 为准。下文保留当时的实验记录；Linux 摄像头服务不再属于本次交付。

| 资源 | 管理方与约束 |
|---|---|
| A53 MPIDR 0..3 | openvela；Linux DTB不保留这些CPU节点 |
| A72 MPIDR 0x100..0x103 | Linux；openvela SMP映射仅覆盖A53 |
| GIC distributor初始化 | Linux；openvela跳过全局SPI与GICD_CTLR重置 |
| SPI route/priority/trigger | Linux按AMP DTS静态配置；openvela不做共享word读改写 |
| SGI/PPI、GICC | 各OS仅操作自己的CPU |
| UART0 IRQ108 | openvela；Linux禁用UART0与FIQ debugger |
| MAILBOX3 A2B IRQ174 | Linux发送，openvela接收；启动器预使能接收并保留首个kick |
| MAILBOX0/3 B2A | openvela发送，Linux接收 |
| RPMsg buffer pool | Linux分配；openvela消费并维护cache |
| SD/eMMC | 最小AMP不启用存储host，启动期由N-Boot使用 |

Linux关闭CPU频率/空闲/系统挂起调节，保留unused clocks和power domains，
避免首次闭环关闭openvela仍使用的资源。完整产品资源划分尚未在此配置启用。

通用GIC补丁见nuttx/gicv2-amp.patch；两个选项默认关闭，普通配置保持原行为。
本方案是协作式AMP，不提供对恶意/失控内核的硬件隔离，不支持单边独立重启。
