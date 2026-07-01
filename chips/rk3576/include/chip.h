/* arch/arm64/include/rk3576/chip.h - Rockchip RK3576 (KICKPI-K7) */
#ifndef __ARCH_ARM64_INCLUDE_RK3576_CHIP_H
#define __ARCH_ARM64_INCLUDE_RK3576_CHIP_H

#include <nuttx/config.h>

#define KB(x)           ((x) << 10)
#define MB(x)           (KB(x) << 10)
#define GB(x)           (MB(UINT64_C(x)) << 10)

/* RK3576 uses ARM GIC-400 (GICv2). GICD @ base+0x1000, GICC @ base+0x2000.
 * GIC-400 base = 0x2A700000 (TRM). For GICv2 driver:
 *   CONFIG_GICD_BASE = Distributor, CONFIG_GICR_BASE = CPU interface (GICC).
 */
#define CONFIG_GICD_BASE          0x2A701000
#define CONFIG_GICR_BASE          0x2A702000
#define CONFIG_GICR_OFFSET        0x0

/* RK3576 Memory Map: DRAM and Device I/O.
 * Peripherals span ~0x20000000..0x2FFFFFFF (UART0 0x2AD40000, GIC 0x2A700000,
 * CRU 0x27200000, etc). DRAM base 0x40000000+ (ramoops@40110000 in vendor DTS).
 * NOTE: RAMBANK size and LOAD_BASE are TBD - verify on board (MiniLoader->BL33
 * handoff address; see rkbin RK3576_EN.md). Placeholder values to allow build.
 */
#define CONFIG_DEVICEIO_BASEADDR  0x20000000
#define CONFIG_DEVICEIO_SIZE      MB(256)
#define CONFIG_RAMBANK1_ADDR      0x40000000
#define CONFIG_RAMBANK1_SIZE      MB(1024)
#define CONFIG_LOAD_BASE          0x40200000   /* TBD verify on board */

#define MPID_TO_CLUSTER_ID(mpid)  ((mpid) & ~0xff)


#ifdef __ASSEMBLY__

.macro  get_cpu_id xreg0
  mrs    \xreg0, mpidr_el1
  ubfx   \xreg0, \xreg0, #0, #8
.endm

#endif /* __ASSEMBLY__ */
#endif /* __ARCH_ARM64_INCLUDE_RK3576_CHIP_H */
