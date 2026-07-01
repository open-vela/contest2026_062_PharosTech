# KICKPI-K7 (RK3576) — openvela 板级适配

> 队伍 062 · Pharos Tech · 项目 Nyabula
> 状态：**编译通过，板上待验证**

## 一、概述

将 openvela（NuttX）移植到 **KICKPI-K7** 开发板（Rockchip **RK3576**，4×Cortex-A72 + 4×Cortex-A53 + Cortex-M0，6 TOPS NPU，中断控制器 GIC-400/GICv2）。本 BSP **基于 openvela 自带的 rk3399 移植改写** —— RK3399 与 RK3576 同族、同为 armv8-a、同款 DesignWare UART，改地址、改 GIC 版本、改板级配置即可，不从零起。

## 二、架构：完全 out-of-tree（零改生产仓）

芯片与板级代码**全部放在 vendor 侧**，通过 openvela 的 `ARCH_CHIP_CUSTOM` / `ARCH_BOARD_CUSTOM` 机制接入，**对 nuttx 等生产仓零改动**（参照 `vendor/artinchip` 先例）。队伍仓的子目录经 manifest `<linkfile>` 软链进 openvela 工程：

```
队伍仓                      →  openvela 工作树
chips/rk3576/               →  vendor/rockchip/chips/rk3576/            （芯片 arch）
boards/rk3576/kickpi-k7/    →  vendor/rockchip/boards/rk3576/kickpi-k7/ （板级）
```

好处：调试期只动队伍仓、一步步 commit；nuttx 公共仓一行不改；仍能编出完整 openvela 固件。

## 三、关键适配点

- **中断控制器**：GIC-400 = **GICv2**（区别于 RK3588 的 GICv3）。`CONFIG_ARM64_GIC_VERSION=2`，GICD `0x2A701000` / GICC `0x2A702000`。
- **调试串口**：**UART0 @ `0x2AD40000`**，DesignWare 16550，`reg-shift=2`。取自厂方 vendor DTS 实证（`earlycon=uart8250,mmio32,0x2ad40000`），波特率 `1500000`。
- **中断号**：从 vendor DTS 提取（GIC SPI 号 + 32，例：UART0 = 108）。
- **外设基址**：取自 RK3576 TRM。
- **启动**：厂方 `MiniLoaderAll` 作 DDR init + SPL，openvela 作 BL33 接续。

## 四、构建

```bash
# 在 openvela 工作区根目录
./build.sh ../vendor/rockchip/boards/rk3576/kickpi-k7/configs/nsh -j8
# 产出：nuttx/nuttx.bin（ARM aarch64）
```

## 五、板上待验证

- `LOAD_BASE` / `RAM_START`（MiniLoader→BL33 交接地址）当前为占位值。
- M2 里程碑：串口打印（"NuttX on K7"）。
- 之后：时钟、GIC、MMU、SMP、外设驱动。
