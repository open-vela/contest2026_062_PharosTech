/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_gpio.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_GPIO_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_GPIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of GPIO banks and pins ********************************************/

#define RK3576_GPIO_NPORTS      5       /* GPIO0 - GPIO4 */
#define RK3576_GPIO_NPINS       32      /* 32 pins per port */

/* GPIO per-bank register offsets (Rockchip GPIO v2 controller)
 *
 * Verified against Linux pinctrl-rockchip struct rockchip_gpio_regs:
 *   port_dr, port_ddr, int_en, int_mask, int_type, int_polarity,
 *   int_bothedge, int_status, int_rawstatus, debounce, dbclk_div_en,
 *   dbclk_div_con, port_eoi, ext_port, version_id
 */

#define RK3576_GPIO_SWPORTA_DR_OFFSET      0x0000  /* Data register */
#define RK3576_GPIO_SWPORTA_DDR_OFFSET     0x0004  /* Data direction (0=in, 1=out) */
#define RK3576_GPIO_INTEN_OFFSET           0x0030  /* Interrupt enable */
#define RK3576_GPIO_INTMASK_OFFSET         0x0034  /* Interrupt mask */
#define RK3576_GPIO_INTTYPE_LEVEL_OFFSET   0x0038  /* Interrupt type (0=level, 1=edge) */
#define RK3576_GPIO_INT_POLARITY_OFFSET    0x003C  /* Interrupt polarity (0=low/fall, 1=high/rise) */
#define RK3576_GPIO_INT_BOTHEDGE_OFFSET    0x0068  /* Interrupt both-edge trigger */
#define RK3576_GPIO_INT_STATUS_OFFSET      0x0040  /* Interrupt status (masked) */
#define RK3576_GPIO_INT_RAWSTATUS_OFFSET   0x0044  /* Raw interrupt status */
#define RK3576_GPIO_DEBOUNCE_OFFSET        0x0048  /* Debounce enable */
#define RK3576_GPIO_DBCLK_DIV_EN_OFFSET    0x004C  /* Debounce clock divider enable / port_eoi */
#define RK3576_GPIO_DBCLK_DIV_CON_OFFSET   0x0050  /* Debounce clock divider config / ext_port */
#define RK3576_GPIO_EXT_PORTA_OFFSET       0x0050  /* External port data (same as DBCLK_DIV_CON) */
#define RK3576_GPIO_PORTA_EOI_OFFSET       RK3576_GPIO_DBCLK_DIV_EN_OFFSET  /* Port end-of-interrupt */
#define RK3576_GPIO_LS_SYNC_OFFSET         0x0060  /* Level-sensitive sync */
#define RK3576_GPIO_VERSION_ID_OFFSET      0x0078  /* GPIO controller version */

/* Register address macros (use lookup table - banks are NOT uniformly spaced) */

#define RK3576_GPIO_SWPORTA_DR(port)       (g_gpio_base[port] + RK3576_GPIO_SWPORTA_DR_OFFSET)
#define RK3576_GPIO_SWPORTA_DDR(port)      (g_gpio_base[port] + RK3576_GPIO_SWPORTA_DDR_OFFSET)
#define RK3576_GPIO_INTEN(port)            (g_gpio_base[port] + RK3576_GPIO_INTEN_OFFSET)
#define RK3576_GPIO_INTMASK(port)          (g_gpio_base[port] + RK3576_GPIO_INTMASK_OFFSET)
#define RK3576_GPIO_INTTYPE_LEVEL(port)    (g_gpio_base[port] + RK3576_GPIO_INTTYPE_LEVEL_OFFSET)
#define RK3576_GPIO_INT_POLARITY(port)     (g_gpio_base[port] + RK3576_GPIO_INT_POLARITY_OFFSET)
#define RK3576_GPIO_INT_BOTHEDGE(port)     (g_gpio_base[port] + RK3576_GPIO_INT_BOTHEDGE_OFFSET)
#define RK3576_GPIO_INT_STATUS(port)       (g_gpio_base[port] + RK3576_GPIO_INT_STATUS_OFFSET)
#define RK3576_GPIO_INT_RAWSTATUS(port)    (g_gpio_base[port] + RK3576_GPIO_INT_RAWSTATUS_OFFSET)
#define RK3576_GPIO_DEBOUNCE(port)         (g_gpio_base[port] + RK3576_GPIO_DEBOUNCE_OFFSET)
#define RK3576_GPIO_EXT_PORTA(port)        (g_gpio_base[port] + RK3576_GPIO_EXT_PORTA_OFFSET)
#define RK3576_GPIO_PORTA_EOI(port)        (g_gpio_base[port] + RK3576_GPIO_PORTA_EOI_OFFSET)
#define RK3576_GPIO_LS_SYNC(port)          (g_gpio_base[port] + RK3576_GPIO_LS_SYNC_OFFSET)
#define RK3576_GPIO_VERSION_ID(port)       (g_gpio_base[port] + RK3576_GPIO_VERSION_ID_OFFSET)

/* GPIO base address table. Banks NOT uniformly spaced. */

extern const uint32_t g_gpio_base[RK3576_GPIO_NPORTS];

/* Bit manipulation macros */

#define RK3576_GPIO_PIN_BIT(pin)           (1u << (pin))

/* GPIO_SWPORTA_DDR: data direction register */

#define RK3576_GPIO_DDR_INPUT              0
#define RK3576_GPIO_DDR_OUTPUT             1

/* GPIO_INTTYPE_LEVEL: interrupt trigger type (register bit values) */

#define RK3576_GPIO_INTTYPE_LEVEL_VAL      0
#define RK3576_GPIO_INTTYPE_EDGE_VAL       1

/* GPIO_INT_POLARITY: interrupt polarity */

#define RK3576_GPIO_INTPOL_LOW_FALLING     0
#define RK3576_GPIO_INTPOL_HIGH_RISING     1

/* GPIO interrupt IRQ numbers — see include/irq.h */

/* =========================================================================
 * IOC (I/O Control) Register Definitions
 *
 * On RK3576, pin configuration (mux, pull, drive, schmitt) is controlled
 * through the IOC region, separate from the GPIO data registers.
 * Verified against Linux pinctrl-rockchip RK3576 data.
 *
 * The IOC cluster starts at PMU0_IOC and spans PMU1/TOP/VCCIO/VCCIO6/7:
 *   PMU0_IOC   @ 0x26040000  (GPIO0 MUX/Attribute, pin 0-11)
 *   PMU1_IOC   @ 0x26042000  (GPIO0 MUX/Attribute, pin 12-31)
 *   TOP_IOC    @ 0x26044000  (GPIO1/2/3/4 MUX)
 *   VCCIO_IOC  @ 0x26046000  (GPIO1/2/3/4 Attribute: pull/drive/schmitt)
 *   VCCIO6_IOC @ 0x2604A000  (GPIO4 MUX/Attribute, pin 16-23)
 *   VCCIO7_IOC @ 0x2604B000  (GPIO4 MUX/Attribute, pin 24-31)
 *
 * All offsets in this driver are relative to PMU0_IOC as the unified base.
 *
 * All IOC registers use "hiword-mask" write scheme:
 *   Upper 16 bits: bitmask indicating which lower bits to modify
 *   Lower 16 bits: actual values
 * ========================================================================= */

/* Unified IOC base address (PMU0_IOC) — all offsets are relative to this */
#define RK3576_IOC_BASE                 0x26040000

/* Hiword-mask write helper: write 'v' into bits [h:l] */
#define RK3576_WRITE_MASK(h, l, v) \
  ((((1u << ((h) - (l) + 1)) - 1) << (l)) << 16 | ((v) << (l)))

/* Single-bit hiword-mask */
#define RK3576_WRITE_BIT(bit, v) \
  ((1u << ((bit) + 16)) | ((v) << (bit)))

/* =========================================================================
 * IOMUX register layout (4-bit width per pin, IOMUX_WIDTH_4BIT)
 *
 * Each 8-pin group uses 2 consecutive registers (4 pins each).
 * Per-bank IOMUX offset table (from Linux RK3576 pin_banks):
 *
 *   GPIO0: 0x0000(A), 0x0008(B*), 0x2004(C), 0x200C(D)
 *          * B group (pins 8-15): pins 12-15 add +0x1FF4 extra offset
 *   GPIO1: 0x4020, 0x4028, 0x4030, 0x4038
 *   GPIO2: 0x4040, 0x4048, 0x4050, 0x4058
 *   GPIO3: 0x4060, 0x4068, 0x4070, 0x4078
 *   GPIO4: 0x4080, 0x4088, 0xA390, 0xB398
 * ========================================================================= */

/* Per 8-pin group IOMUX register info */
struct rk3576_iomux_group
{
  uint32_t offset;  /* First register offset for this 8-pin group */
};

extern const struct rk3576_iomux_group
  g_iomux_groups[RK3576_GPIO_NPORTS][4];

/* =========================================================================
 * Pull-up/down register layout (2 bits/pin, 8 pins/reg, PULL_TYPE_IO_1V8_ONLY)
 *
 * Per-bank pull offset table (from Linux rk3576_calc_pull_reg_and_bit):
 *
 *   GPIO0A(pin 0-11):  offset 0x0020, GPIO0BH(pin 12-31): offset 0x2024
 *   GPIO1:             offset 0x6110
 *   GPIO2:             offset 0x6120
 *   GPIO3:             offset 0x6130
 *   GPIO4A(pin 0-15):  offset 0x6140, GPIO4CL(pin 16-23): offset 0xA140,
 *   GPIO4DL(pin 24-31): offset 0xB140
 *
 * Pull values (PULL_TYPE_IO_1V8_ONLY ordering):
 *   0 = disable (float)
 *   1 = pull-down
 *   2 = reserved (equivalent to disable)
 *   3 = pull-up
 * ========================================================================= */

#define RK3576_PULL_DISABLE          0
#define RK3576_PULL_DOWN             1
#define RK3576_PULL_UP               3

#define RK3576_PULL_BITS_PER_PIN     2
#define RK3576_PULL_PINS_PER_REG     8

/* =========================================================================
 * Drive-strength register layout (4 bits/pin, 4 pins/reg)
 *
 * Per-bank drive offset table (from Linux rk3576_calc_drv_reg_and_bit):
 *
 *   GPIO0A(pin 0-11):  offset 0x0010, GPIO0BH(pin 12-31): offset 0x2008
 *   GPIO1:             offset 0x6020
 *   GPIO2:             offset 0x6040
 *   GPIO3:             offset 0x6060
 *   GPIO4A(pin 0-15):  offset 0x6080, GPIO4CL(pin 16-23): offset 0xA080,
 *   GPIO4DL(pin 24-31): offset 0xB080
 *
 * Drive value encoding (from Linux rockchip_set_drive_perpin RK3576 case):
 *   hw_val = ((strength & BIT(2)) >> 2) |
 *            ((strength & BIT(0)) << 2) |
 *            (strength & BIT(1))
 *
 * This is a bit-permutation of the 3-bit level (0-7):
 *   level  hw[3:0]   ~ current
 *     0     0000      2 mA
 *     1     0100      4 mA
 *     2     0010      8 mA
 *     3     0110      12 mA (default)
 *     4     0001      level 4
 *     5     0101      level 5
 *     6     0011      level 6
 *     7     0111      level 7
 * ========================================================================= */

#define RK3576_DRV_BITS_PER_PIN      4
#define RK3576_DRV_PINS_PER_REG      4

/* Convert drive level (0-7) to hardware register value */
#define RK3576_DRIVE_LEVEL_TO_HW(l) \
  ((((l) & 0x4) >> 2) | (((l) & 0x1) << 2) | ((l) & 0x2))

/* Common drive strength levels */
#define RK3576_DRIVE_LEVEL_2MA        0
#define RK3576_DRIVE_LEVEL_4MA        1
#define RK3576_DRIVE_LEVEL_8MA        2
#define RK3576_DRIVE_LEVEL_12MA       3   /* Default */
#define RK3576_DRIVE_LEVEL_DEFAULT    3

/* =========================================================================
 * Schmitt-trigger register layout (1 bit/pin, 8 pins/reg)
 *
 * Per-bank schmitt offset table (from Linux rk3576_calc_schmitt_reg_and_bit):
 *
 *   GPIO0A(pin 0-11):  offset 0x0030, GPIO0BH(pin 12-31): offset 0x203C
 *   GPIO1:             offset 0x6210
 *   GPIO2:             offset 0x6220
 *   GPIO3:             offset 0x6230
 *   GPIO4A(pin 0-15):  offset 0x6240, GPIO4CL(pin 16-23): offset 0xA240,
 *   GPIO4DL(pin 24-31): offset 0xB240
 * ========================================================================= */

#define RK3576_SMT_BITS_PER_PIN      1
#define RK3576_SMT_PINS_PER_REG      8

#define RK3576_GPIO_INTPOL_LOW_FALLING     0
#define RK3576_GPIO_INTPOL_HIGH_RISING     1

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_GPIO_H */
