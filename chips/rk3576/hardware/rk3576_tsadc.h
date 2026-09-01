/****************************************************************************
 * chips/rk3576/hardware/rk3576_tsadc.h
 *
 * SPDX-License-Identifier: Apache-2.0
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

/****************************************************************************
 * RK3576 Temperature-Sensor ADC (TS-ADC) register definitions.
 *
 * Source: RK3576 TRM, Chapter 19 "Temperature-Sensor ADC (TS-ADC)",
 *         V1.2 20240624.  Register offsets, bit fields and reset values are
 *         transcribed from that public hardware specification.
 *
 * The TS-ADC measures on-die temperature at six probe locations.  It works
 * in two modes selected by TSADC_AUTO_CON[0]:
 *   - auto mode  (bit0 = 1): controller periodically converts all channels
 *     enabled in TSADC_AUTO_SRC and keeps the latest result in DATA0..5.
 *     This is the mode used by the NuttX thermal framework (polling).
 *   - user mode  (bit0 = 0): software triggers a single conversion of the
 *     channel selected by TSADC_USER_CON[3:0].
 *
 * All data registers below are 32-bit.  For the register fields that occupy
 * only the lower 16 bits, writes are masked by a per-bit write-enable in the
 * upper 16 bits ("write_enable"): each bit of [31:16] gates the matching bit
 * of [15:0].  The HIWORD macro produces a full 32-bit write value.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_TSADC_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_TSADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets (relative to RK3576_TSADC_ADDR) **********************/

#define RK3576_TSADC_USER_CON            0x0000 /* User control             */
#define RK3576_TSADC_AUTO_CON            0x0004 /* Auto control             */
#define RK3576_TSADC_AUTO_STATUS         0x0008 /* Auto-mode status         */
#define RK3576_TSADC_AUTO_SRC            0x000c /* Channel select (auto)    */
#define RK3576_TSADC_LT_EN               0x0010 /* Low-temp check enable    */
#define RK3576_TSADC_HT_INT_EN           0x0014 /* High-temp int enable     */
#define RK3576_TSADC_GPIO_EN             0x0018 /* Violation->GPIO enable   */
#define RK3576_TSADC_CRU_EN              0x001c /* Violation->CRU enable    */
#define RK3576_TSADC_LT_INT_EN           0x0020 /* Low-temp int enable      */
#define RK3576_TSADC_HLT_INT_PD          0x0024 /* H/L int status (W1C)     */
#define RK3576_TSADC_EOC_HSHUT_PD        0x0028 /* EOC/TSHUT status (W1C)   */
#define RK3576_TSADC_DATA0               0x002c /* Channel 0 data           */
#define RK3576_TSADC_DATA1               0x0030 /* Channel 1 data           */
#define RK3576_TSADC_DATA2               0x0034 /* Channel 2 data           */
#define RK3576_TSADC_DATA3               0x0038 /* Channel 3 data           */
#define RK3576_TSADC_DATA4               0x003c /* Channel 4 data           */
#define RK3576_TSADC_DATA5               0x0040 /* Channel 5 data           */
#define RK3576_TSADC_COMP0_INT           0x006c /* High int threshold ch0   */
#define RK3576_TSADC_COMP1_INT           0x0070 /* High int threshold ch1   */
#define RK3576_TSADC_COMP2_INT           0x0074 /* High int threshold ch2   */
#define RK3576_TSADC_COMP3_INT           0x0078 /* High int threshold ch3   */
#define RK3576_TSADC_COMP4_INT           0x007c /* High int threshold ch4   */
#define RK3576_TSADC_COMP5_INT           0x0080 /* High int threshold ch5   */
#define RK3576_TSADC_COMP0_SHUT          0x010c /* TSHUT threshold ch0      */
#define RK3576_TSADC_COMP1_SHUT          0x0110 /* TSHUT threshold ch1      */
#define RK3576_TSADC_COMP2_SHUT          0x0114 /* TSHUT threshold ch2      */
#define RK3576_TSADC_COMP3_SHUT          0x0118 /* TSHUT threshold ch3      */
#define RK3576_TSADC_COMP4_SHUT          0x011c /* TSHUT threshold ch4      */
#define RK3576_TSADC_COMP5_SHUT          0x0120 /* TSHUT threshold ch5      */
#define RK3576_TSADC_HIGH_INT_DEBOUNCE   0x014c /* High int debounce        */
#define RK3576_TSADC_HIGH_TSHUT_DEBOUNCE 0x0150 /* TSHUT debounce          */
#define RK3576_TSADC_AUTO_PERIOD         0x0154 /* Auto conversion period   */
#define RK3576_TSADC_AUTO_PERIOD_HT      0x0158 /* Auto period (hot)        */
#define RK3576_TSADC_COMP0_LOW_INT       0x015c /* Low threshold ch0        */
#define RK3576_TSADC_COMP1_LOW_INT       0x0160 /* Low threshold ch1        */
#define RK3576_TSADC_COMP2_LOW_INT       0x0164 /* Low threshold ch2        */
#define RK3576_TSADC_COMP3_LOW_INT       0x0168 /* Low threshold ch3        */
#define RK3576_TSADC_COMP4_LOW_INT       0x016c /* Low threshold ch4        */
#define RK3576_TSADC_COMP5_LOW_INT       0x0170 /* Low threshold ch5        */
#define RK3576_TSADC_T_SETUP             0x019c /* Power-up->start timing   */
#define RK3576_TSADC_T_PW_EN             0x0200 /* Start-assert timing      */
#define RK3576_TSADC_T_EN_CLK            0x0204 /* Start-neg->clk timing    */
#define RK3576_TSADC_T_NON_OV            0x0208 /* Bit-to-bit timing        */
#define RK3576_TSADC_T_HOLD              0x020c /* Clk-neg->data-valid      */
#define RK3576_TSADC_Q_MAX               0x0210 /* Value for inverted output */
#define RK3576_TSADC_STATIC_CON          0x0214 /* Static control / trim    */
#define RK3576_TSADC_CLK_CH_PERIOD       0x021c /* CLK_CH_TS period         */
#define RK3576_TSADC_T_PW_CLK            0x0220 /* Per-bit assert timing    */

/* Number of channels.  Channel -> probe location (TRM Table 19-1):
 *   0: near chip center, 1: big core, 2: little core,
 *   3: GPU, 4: NPU, 5: DDR. */

#define RK3576_TSADC_NCHANNELS 6

/* Data register field [9:0] holds the conversion code (10-bit). */

#define RK3576_TSADC_DATA_MASK  0x3ff
#define RK3576_TSADC_DATA_SHIFT 0

/* TSADC_USER_CON (0x0000) ***********************************************/

#define TSADC_USER_CON_POWER_CONTROL (1 << 4) /* 1 = power up    */
#define TSADC_USER_CON_START_MODE    (1 << 5) /* 1 = sw-controlled */
#define TSADC_USER_CON_START         (1 << 7) /* W1: start conversion */
#define TSADC_USER_CON_EOC_INTEN     (1 << 8) /* Enable EOC int   */
#define TSADC_USER_CON_CH_MASK       (0xf)    /* input_src_sel    */

/* TSADC_AUTO_CON (0x0004) ***********************************************/

#define TSADC_AUTO_CON_AUTO_EN      (1 << 0) /* 1 = auto mode    */
#define TSADC_AUTO_CON_Q_SEL        (1 << 1) /* 1 = inverted out */
#define TSADC_AUTO_CON_ROUND_INT_EN (1 << 2) /* Round int enable */
#define TSADC_AUTO_CON_TSHUT_POL    (1 << 8) /* 1 = high active  */

/* TSADC_AUTO_STATUS (0x0008) ********************************************/

#define TSADC_AUTO_STATUS_IN_PROGRESS    (1 << 2) /* 1 = auto running */
#define TSADC_AUTO_STATUS_LAST_TSHUT_CRU (1 << 1) /* W1C: last tshut->CRU */
#define TSADC_AUTO_STATUS_LAST_TSHUT_GPIO                           \
  (1 << 0)                                 /* W1C: last tshut->GPIO \
                                            */
#define TSADC_AUTO_STATUS_HT_WARM (1 << 3) /* 1 = > shut temp */

/* HIWORD write-mask helper: enable the lower 16 bits to be written. */

#define TSADC_HIWORD_ENABLE     (0xffffu << 16)
#define TSADC_WRITE_MASKED(val) (((val)&0xffff) | TSADC_HIWORD_ENABLE)

/* Cru gate bits (CRU_GATE_CON13, 0x0834).  SET_TO_DISABLE semantics:
 * writing 1 disables the clock; the clk-tree APIs handle the inversion,
 * but the driver must request both the pclk and the conversion clock. */

#define RK3576_CRU_TSADC_PCLK_GATE_BIT 8 /* pclk_tsadc_en */
#define RK3576_CRU_TSADC_CLK_GATE_BIT  9 /* clk_tsadc_en  */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_TSADC_H */
