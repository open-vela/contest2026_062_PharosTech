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
 * DRAM: LPDDR5 (2GB/4GB/8GB/16GB selectable via Kconfig), physically
 * contiguous from 0x40000000. Firmware reserves two holes in the low bank:
 * BL31 @0x40000000 (2MB) and OP-TEE @0x48400000 (16MB). NuttX loads and runs
 * at 0x40200000 (== CONFIG_RAM_START, matches the vendor FIT BL33 load
 * address; ELF entry verified 0x40200000).
 *
 * DRAM0 MMU region: 0x40000000..0x48400000 (132MB) mapped normal secure, i.e.
 * up to the OP-TEE reservation. NuttX only allocates above RAM_START, so the
 * 2MB BL31 hole at the base is mapped but never touched by the allocator.
 *
 * DRAM1 MMU region: 0x49400000..end_of_DDR, the remaining physical DRAM above
 * OP-TEE. Registered as a second heap region via arm64_addregion().
 */

/* Firmware reservation sizes */
#define RK3576_BL31_SIZE  MB(2)  /* BL31 @ 0x40000000 */
#define RK3576_OPTEE_SIZE MB(16) /* OP-TEE @ 0x48400000 */

/* Device I/O MMU region */
#define CONFIG_DEVICEIO_BASEADDR 0x20000000
#define CONFIG_DEVICEIO_SIZE     MB(256)

/* Bank 1: DRAM base (0x40000000) up to OP-TEE (0x48400000) */
#define CONFIG_RAMBANK1_ADDR 0x40000000
#define CONFIG_RAMBANK1_SIZE (MB(130) + RK3576_BL31_SIZE)
/* 130MB usable + 2MB BL31 hole */

/* Second DRAM bank: from after OP-TEE (0x49400000) to end of DDR */
#define CONFIG_RAMBANK2_ADDR \
  (CONFIG_RAMBANK1_ADDR + CONFIG_RAMBANK1_SIZE + RK3576_OPTEE_SIZE)
#define CONFIG_RAMBANK2_SIZE \
  (GB(CONFIG_RK3576_DDR_SIZE_GB) - RK3576_OPTEE_SIZE - CONFIG_RAMBANK1_SIZE)
/* DDR_SIZE - OP-TEE - bank1 (which includes BL31 hole) */

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
