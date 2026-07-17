/* arch/arm64/include/rk3576/chip.h - Rockchip RK3576 (KICKPI-K7) */
#ifndef __ARCH_ARM64_INCLUDE_RK3576_CHIP_H
#define __ARCH_ARM64_INCLUDE_RK3576_CHIP_H

#include <nuttx/config.h>

#define KB(x) ((x) << 10)
#define MB(x) (KB(x) << 10)
#define GB(x) (MB(UINT64_C(x)) << 10)

/* RK3576 uses ARM GIC-400 (GICv2). GICD @ base+0x1000, GICC @ base+0x2000.
 * GIC-400 base = 0x2A700000 (TRM). For GICv2 driver:
 *   CONFIG_GICD_BASE = Distributor, CONFIG_GICR_BASE = CPU interface (GICC).
 */
#define CONFIG_GICD_BASE   0x2A701000
#define CONFIG_GICR_BASE   0x2A702000
#define CONFIG_GICR_OFFSET 0x0

/* RK3576 Memory Map: DRAM and Device I/O. Confirmed on-board (KICKPI-K7, SD
 * boot, NuttX as BL33 -> NSH, 2026-07-04).
 *
 * Device I/O window: peripherals span 0x20000000..0x2FFFFFFF, e.g. UART0
 * 0x2AD40000 (cmdline earlycon confirmed), GIC-400 0x2A700000, CRU 0x27200000.
 *
 * DRAM: 4GB LPDDR5, physically contiguous 0x40000000..0x13FFFFFFF
 * (/proc/iomem). Firmware reserves two holes in the low bank: BL31 @0x40000000
 * (2MB) and OP-TEE
 * @0x48400000 (16MB). NuttX loads and runs at 0x40200000 (== CONFIG_RAM_START,
 * matches the vendor FIT BL33 load address; ELF entry verified 0x40200000).
 *
 * DRAM0 MMU region: 0x40000000..0x48400000 (132MB) mapped normal secure, i.e.
 * up to the OP-TEE reservation. NuttX only allocates above RAM_START, so the
 * 2MB BL31 hole at the base is mapped but never touched by the allocator.
 */
#define CONFIG_DEVICEIO_BASEADDR 0x20000000
#define CONFIG_DEVICEIO_SIZE     MB(256)
#define CONFIG_RAMBANK1_ADDR     0x40000000
#define CONFIG_RAMBANK1_SIZE     MB(132) /* base..0x48400000 (OP-TEE) */

#define MPID_TO_CLUSTER_ID(mpid) ((mpid) & ~0xff)

/* clang-format off */
#ifdef __ASSEMBLY__

.macro  get_cpu_id xreg0
  mrs    \xreg0, mpidr_el1
  ubfx   \xreg0, \xreg0, #0, #8
.endm

#endif /* __ASSEMBLY__ */
/* clang-format on */

#endif /* __ARCH_ARM64_INCLUDE_RK3576_CHIP_H */
