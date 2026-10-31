# ABI2案例与验收门

## 最小所有权表

每项写 `物理资源 / owner / peer必须禁用什么 / 启动时谁初始化 / 运行时谁可改 / 复位范围`。
GIC、CRU和PD属于共享硬件，即使两个OS用不同外设也可能写同一寄存器word；避免跨域未同步的读改写。

## K7 ABI2的已测拓扑

N-Boot在A53 MPIDR0，经匹配Rockchip BL31合同把Linux启动在A72 0x100；Linux再启0x101..0x103。待GIC/RPMsg首通知后，CPU0进入openvela，openvela再启A53 1..3。Linux DTB需删除A53 CPU节点和相关引用，该vendor内核中仅写status=disabled不够。

UART0 IRQ108和mailbox IRQ174是该K7合同，不是其他板子的默认。GICv2 external/preinitialized配置必须避免重置另一域的SPI；SGI/PPI/GICC按本核处理。

## 成功与失败案例

9月10日四A53/四A72真实health/info往返，`online=4`。早期PSCI未probe、mailbox接收未使能、resource table CPUNAME和READY动态地址学习分别阻断链路，不能只用“mailbox寄存器有值”替代最终RPC。

9月20日product加入更多外设时，16MiB镜像区之外的heap须额外保留和映射；挪heap后漏掉镜像自身映射导致早期异常。DMA/音频PD和时钟也需要保持，不能从最小health镜像直接推产品配置可用。

## 验收门

- 镜像预检：每payload hash/load/entry/runtime extent、DTB CPU集合与RAM保留都匹配。
- 双域存活：各域报告真实核数/MPIDR，单独观察日志，不能两边都数同一组逻辑编号。
- 传输：真实request/response、generation、timeout，不是本地loopback。
- 产品扩展：一次引入一组外设，确认对端节点/IRQ/clock被妥善划分。
- 恢复：只承诺实际跑过的整机恢复，不宣称支持单域独立重启或恶意内核安全隔离。

当用户只有RAM试跑授权时，预检和内存启动足够，不把“为了更稳定”当成写持久槽理由。
