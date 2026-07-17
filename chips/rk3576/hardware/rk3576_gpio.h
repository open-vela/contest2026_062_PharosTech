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

#define RK3576_GPIO_NPORTS 5  /* GPIO0 - GPIO4 */
#define RK3576_GPIO_NPINS  32 /* 32 pins per port */

/* GPIO per-bank register offsets (Rockchip GPIO v2 controller) */

#define RK3576_GPIO_SWPORTA_DR_OFFSET 0x0000 /* port_dr: Data register */
#define RK3576_GPIO_SWPORTA_DDR_OFFSET \
  0x0008 /* port_ddr: Data direction (0=in, 1=out) */
#define RK3576_GPIO_INTEN_OFFSET   0x0010 /* int_en: Interrupt enable */
#define RK3576_GPIO_INTMASK_OFFSET 0x0018 /* int_mask: Interrupt mask */
#define RK3576_GPIO_INTTYPE_LEVEL_OFFSET \
  0x0020 /* int_type: Interrupt type (0=level, 1=edge) */
#define RK3576_GPIO_INT_POLARITY_OFFSET \
  0x0028 /* int_polarity: Interrupt polarity */
#define RK3576_GPIO_INT_BOTHEDGE_OFFSET \
  0x0030 /* int_bothedge: Both-edge trigger */
#define RK3576_GPIO_DEBOUNCE_OFFSET 0x0038 /* debounce: Debounce enable */
#define RK3576_GPIO_DBCLK_DIV_EN_OFFSET \
  0x0040 /* dbclk_div_en: Debounce clk divider enable / port_eoi */
#define RK3576_GPIO_DBCLK_DIV_CON_OFFSET \
  0x0048 /* dbclk_div_con: Debounce clk divider config */
#define RK3576_GPIO_INT_STATUS_OFFSET \
  0x0050 /* int_status: Interrupt status (masked) */
#define RK3576_GPIO_INT_RAWSTATUS_OFFSET \
  0x0058 /* int_rawstatus: Raw interrupt status */
#define RK3576_GPIO_PORTA_EOI_OFFSET \
  0x0060 /* port_eoi: End-of-interrupt (write-1-clear) */
#define RK3576_GPIO_EXT_PORT_OFFSET \
  0x0070 /* ext_port: External port data (actual pin level) */
#define RK3576_GPIO_VERSION_ID_OFFSET \
  0x0078 /* version_id: Controller version */

/* GPIO base address table. Banks NOT uniformly spaced. */

extern const uint32_t g_gpio_base[RK3576_GPIO_NPORTS];

/* Register address macros (use lookup table — banks are NOT uniformly spaced)
 */

#define RK3576_GPIO_SWPORTA_DR(port) \
  (g_gpio_base[port] + RK3576_GPIO_SWPORTA_DR_OFFSET)
#define RK3576_GPIO_SWPORTA_DDR(port) \
  (g_gpio_base[port] + RK3576_GPIO_SWPORTA_DDR_OFFSET)
#define RK3576_GPIO_INTEN(port) (g_gpio_base[port] + RK3576_GPIO_INTEN_OFFSET)
#define RK3576_GPIO_INTMASK(port) \
  (g_gpio_base[port] + RK3576_GPIO_INTMASK_OFFSET)
#define RK3576_GPIO_INTTYPE_LEVEL(port) \
  (g_gpio_base[port] + RK3576_GPIO_INTTYPE_LEVEL_OFFSET)
#define RK3576_GPIO_INT_POLARITY(port) \
  (g_gpio_base[port] + RK3576_GPIO_INT_POLARITY_OFFSET)
#define RK3576_GPIO_INT_BOTHEDGE(port) \
  (g_gpio_base[port] + RK3576_GPIO_INT_BOTHEDGE_OFFSET)
#define RK3576_GPIO_DEBOUNCE(port) \
  (g_gpio_base[port] + RK3576_GPIO_DEBOUNCE_OFFSET)
#define RK3576_GPIO_DBCLK_DIV_EN(port) \
  (g_gpio_base[port] + RK3576_GPIO_DBCLK_DIV_EN_OFFSET)
#define RK3576_GPIO_DBCLK_DIV_CON(port) \
  (g_gpio_base[port] + RK3576_GPIO_DBCLK_DIV_CON_OFFSET)
#define RK3576_GPIO_INT_STATUS(port) \
  (g_gpio_base[port] + RK3576_GPIO_INT_STATUS_OFFSET)
#define RK3576_GPIO_INT_RAWSTATUS(port) \
  (g_gpio_base[port] + RK3576_GPIO_INT_RAWSTATUS_OFFSET)
#define RK3576_GPIO_PORTA_EOI(port) \
  (g_gpio_base[port] + RK3576_GPIO_PORTA_EOI_OFFSET)
#define RK3576_GPIO_EXT_PORT(port) \
  (g_gpio_base[port] + RK3576_GPIO_EXT_PORT_OFFSET)
#define RK3576_GPIO_VERSION_ID(port) \
  (g_gpio_base[port] + RK3576_GPIO_VERSION_ID_OFFSET)

/* Bit manipulation macros */

#define RK3576_GPIO_PIN_BIT(pin) (1u << (pin))

/* Hiword-mask write helpers.
 *
 * ALL registers on RK3576 — both GPIO controller (DR, DDR, INTEN, etc.)
 * and IOC (IOMUX, pull, drive, schmitt) — use the "hiword-mask" scheme:
 *   Upper 16 bits (31:16) = write mask — 1 enables write for that bit
 *   Lower 16 bits (15:0)  = value to write
 *
 * Without setting the mask bit, the corresponding value bit is ignored.
 */

/* Write 'v' into bits [h:l]; multi-bit variant */
#define RK3576_WRITE_MASK(h, l, v) \
  ((((1u << ((h) - (l) + 1)) - 1) << (l)) << 16 | ((v) << (l)))

/* Write 'v' into a single bit */
#define RK3576_WRITE_BIT(bit, v) ((1u << ((bit) + 16)) | ((v) << (bit)))

/* GPIO v2 single-bit write with 16-bit split register handling.
 *
 * GPIO v2 32-bit registers are split into two 16-bit halves:
 *   reg+0 covers pins 0-15, reg+4 covers pins 16-31.
 * This macro handles the split transparently — callers just
 * provide the full 0-31 pin number.
 *
 * IOC registers do NOT use this; they cover ≤8 pins per reg
 * with bit offsets always < 16.  Use RK3576_WRITE_BIT directly.
 */
#define RK3576_GPIO_V2_WRITE_BIT(reg_base, pin, v)                 \
  do                                                               \
    {                                                              \
      if ((pin) >= 16)                                             \
        putreg32(RK3576_WRITE_BIT((pin)-16, (v)), (reg_base) + 4); \
      else                                                         \
        putreg32(RK3576_WRITE_BIT((pin), (v)), (reg_base));        \
    }                                                              \
  while (0)

/* GPIO_SWPORTA_DDR: data direction register */

#define RK3576_GPIO_DDR_INPUT  0
#define RK3576_GPIO_DDR_OUTPUT 1

/* GPIO_INTTYPE_LEVEL: interrupt trigger type (register bit values) */

#define RK3576_GPIO_INTTYPE_LEVEL_VAL 0
#define RK3576_GPIO_INTTYPE_EDGE_VAL  1

/* GPIO_INT_POLARITY: interrupt polarity */

#define RK3576_GPIO_INTPOL_LOW_FALLING 0
#define RK3576_GPIO_INTPOL_HIGH_RISING 1

/* =========================================================================
 * IOMUX register layout (4-bit width per pin, IOMUX_WIDTH_4BIT)
 *
 * Each 8-pin group uses 2 consecutive registers (4 pins each).
 * Per-bank IOMUX offset table:
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
  uint32_t offset; /* First register offset for this 8-pin group */
};

extern const struct rk3576_iomux_group g_iomux_groups[RK3576_GPIO_NPORTS][4];

/* =========================================================================
 * Pull-up/down register layout (2 bits/pin, 8 pins/reg, PULL_TYPE_IO_1V8_ONLY)
 *
 * Per-bank pull offset table:
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

#define RK3576_PULL_DISABLE      0
#define RK3576_PULL_DOWN         1
#define RK3576_PULL_UP           3

#define RK3576_PULL_BITS_PER_PIN 2
#define RK3576_PULL_PINS_PER_REG 8

/* =========================================================================
 * Drive-strength register layout (4 bits/pin, 4 pins/reg)
 *
 * RK3576 has two classes of GPIO drive strength:
 *
 *   4-level GPIOs: GPIO0_A[0:7], GPIO0_B[0:3], GPIO4_D[0:1]
 *     Lv0=0b0000(100 ohms), Lv1=0b0010(50 ohms), Lv2=0b0001(33 ohms),
 * Lv3=0b0011(25 ohms)
 *
 *   6-level GPIOs: GPIO0_B[4:7], GPIO1, GPIO2, GPIO3, GPIO4_A/B/C
 *     Lv0=0b0000(100 ohms), Lv1=0b0100(66 ohms),  Lv2=0b0010(50 ohms),
 *     Lv3=0b0110(40 ohms),  Lv4=0b0001(33 ohms),  Lv5=0b0101(25 ohms)
 *
 *   GPIO4_D[2:7] — not specified in TRM (drive strength unsupported).
 *
 * Per-bank drive offset table:
 *
 *   GPIO0A(pin 0-11):  offset 0x0010, GPIO0BH(pin 12-31): offset 0x2008
 *   GPIO1:             offset 0x6020
 *   GPIO2:             offset 0x6040
 *   GPIO3:             offset 0x6060
 *   GPIO4A(pin 0-15):  offset 0x6080, GPIO4CL(pin 16-23): offset 0xA080,
 *   GPIO4DL(pin 24-31): offset 0xB080
 * ========================================================================= */

#define RK3576_DRV_BITS_PER_PIN 4
#define RK3576_DRV_PINS_PER_REG 4

/* Common drive strength levels (logical level, NOT hardware register value).
 * Use rk3576_drive_level_to_hw() to convert to hardware format.
 */
#define RK3576_DRIVE_LEVEL_0 0 /* 100 ohms */
#define RK3576_DRIVE_LEVEL_1                                                \
  1                            /*  66 ohms (6-level only; error on 4-level) \
                                */
#define RK3576_DRIVE_LEVEL_2 2 /*  50 ohms */
#define RK3576_DRIVE_LEVEL_3                                                \
  3                            /*  40 ohms (6-level only; error on 4-level) \
                                */
#define RK3576_DRIVE_LEVEL_4 4 /*  33 ohms */
#define RK3576_DRIVE_LEVEL_5 5 /*  25 ohms */

/* Determine whether a GPIO uses 4-level or 6-level drive strength */
#define RK3576_DRIVE_IS_4LEVEL(port, pin)                           \
  (((port) == 0 && (pin) < 12) || /* GPIO0_A[0:7] + GPIO0_B[0:3] */ \
   ((port) == 4 && (pin) >= 24 && (pin) <= 25)) /* GPIO4_D[0:1] */

/* =========================================================================
 * Schmitt-trigger register layout (1 bit/pin, 8 pins/reg)
 *
 * Per-bank schmitt offset table:
 *
 *   GPIO0A(pin 0-11):  offset 0x0030, GPIO0BH(pin 12-31): offset 0x203C
 *   GPIO1:             offset 0x6210
 *   GPIO2:             offset 0x6220
 *   GPIO3:             offset 0x6230
 *   GPIO4A(pin 0-15):  offset 0x6240, GPIO4CL(pin 16-23): offset 0xA240,
 *   GPIO4DL(pin 24-31): offset 0xB240
 * ========================================================================= */

#define RK3576_SMT_BITS_PER_PIN        1
#define RK3576_SMT_PINS_PER_REG        8

#define RK3576_GPIO_INTPOL_LOW_FALLING 0
#define RK3576_GPIO_INTPOL_HIGH_RISING 1

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_GPIO_H */
