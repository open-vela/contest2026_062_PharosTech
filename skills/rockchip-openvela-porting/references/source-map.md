# BSP 代码入口与硬件事实

本表用于决定去哪里读/改代码，不是把 K7 文件全部复制改名。
示例取自团队仓固定版本
`0a1b7ddf0eaa1ec0469d47921fc6763024ef7c5b`；路径随目标版本核对。

## 选接入方式

| 当前工程 | 接入方式 | 避免 |
|---|---|---|
| 本项目 manifest 工程 | `chips/`、`boards/` 经 linkfile 到 `vendor/rockchip/`；使用 custom chip/board 配置 | 又在 `nuttx/arch` 复制一套，实际编到另一份 |
| 新板首次移植，已有同 SoC | 复用芯片层，新增板级接线、内存/启动契约和 defconfig | 为换一个 GPIO 重写整个 SoC |
| 新 SoC，准备公共上游贡献 | 对照该 revision 的 custom/内建 chip 接入及同 IP 实现；公共架构修改单独提交 | 把竞赛 linkfile 约定写成所有 NuttX 项目的硬要求 |

## 最早故障决定读哪个入口

| 现象/工作 | 团队仓入口 | 同时查构建树 |
|---|---|---|
| 板型选择、源码没有编入 | `contest2026_062_PharosTech.xml`，`chips/rk3576/Kconfig`、Make.defs/CMakeLists | 最终 `.config`、实际 linkfile 目标、编译命令 |
| chip/board 未选中 | `boards/rk3576/kickpi-k7/configs/nsh/defconfig` 的 CUSTOM_DIR 等项 | 当前 NuttX 的 custom chip/board Kconfig；目录值有相对基准 |
| load/entry、入口前多了数据 | `boards/rk3576/kickpi-k7/scripts/dramboot.ld` | `arch/arm64/src/common/arm64_head.S`、最终 ELF/裸镜像 |
| 轮询字节不出/console 不工作 | `rk3576_lowputc.S`、`rk3576_serial.c/.h`、memorymap | 当前架构 common UART/serial 选择、`NO_SERIAL_CONSOLE` |
| 中断号或 GIC 类型不对 | `chips/rk3576/include/irq.h`、`include/chip.h` | 当前 arm64 GICv2 驱动与 vector 路径 |
| MMU、堆、DMA 或内存越界 | `include/chip.h`、`rk3576_boot.c` 的 `g_mmu_regions` / `arm64_addregion()`、`rk3576_dma_alloc.c` | 最终映射/链接脚本与固件 reserved-memory，不只看 defconfig 的 RAM_SIZE |
| 进入 C 后板初始化挂住 | `boards/.../src/kickpi_k7_boardinit.c` 及被调用模块 | `board_late_initialize`、日志缓冲及线程状态 |
| 时钟/复位/引脚 | `rk3576_clk_tree*.c`、GPIO/IOC 硬件头与板级调用 | 同板运行系统的 DTS、clock summary、上电时序 |
| SMP | `rk3576_boot.c` 的逻辑核/MPIDR 映射、`include/chip.h` 的 `get_cpu_id` 宏 | common ARM64 PSCI 启核、该版本 SMP 配置、每核栈和中断接口 |

文件名仅作定位线索；找不到时先查版本和相邻实现，不创建空文件来凑表。
K7 早期借用的 rk3399 模板混有 A64 地址/IRQ/名称，说明“模板同族”不是正确性证据。

## 建一张事实表再填写宏

每个要用的外设只记录这些会影响代码的字段：
`IP/版本、reg 基址和长度、寄存器访问宽度、IRQ 类型/编号、时钟/复位、
pinmux/电源、来源版本、验证级别`。

- DTS 的 `interrupts` 先看 `interrupt-parent` 和 `#interrupt-cells`。
  GIC binding 中 SPI 索引转 INTID 通常加32，PPI 加16；GPIO/级联中断不是这个规则。
  驱动框架可能已经做转换，必须确认加法在哪一层，不能双加。
- `reg` 按父节点的 address/size cells 与 `ranges` 解释，不能直接取文本中的一个数。
- UART 的 `reg-shift`、32位访问、实际时钟和引脚必须独立确认；波特率对不证明 IRQ 对。
- 4GiB 是物理容量，不是可把全部地址交给 malloc 的依据。扣除 BL31、OP-TEE、
  启动器工作区、其他 OS 与 DMA carveout，并防止同一内存被两个 heap 认领。
- GICv2 的 CPU interface 与 GICv3 redistributor 不同。K7 某版本宏名
  `CONFIG_GICR_BASE` 实际被 GICv2 路径用作 GICC，不能由宏名反推硬件版本。

参考源码：[固定版本芯片目录](https://github.com/open-vela/contest2026_062_PharosTech/tree/0a1b7ddf0eaa1ec0469d47921fc6763024ef7c5b/chips/rk3576)。
