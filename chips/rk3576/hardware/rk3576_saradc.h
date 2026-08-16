/****************************************************************************
 * chips/rk3576/hardware/rk3576_saradc.h
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
 * RK3576 SAR-ADC (SARADC) controller register definitions.
 *
 * Reference: Rockchip RK3576 TRM, Chapter 18 "SAR-ADC".
 *
 * The SARADC is an 8-channel single-ended 12-bit SAR A/D converter
 * (input range 0V..1.8V, up to 1 MSps @ 20 MHz).  Registers are 32-bit;
 * several use the Rockchip "hiword-mask" write scheme where the upper 16
 * bits are per-bit write enables.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SARADC_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SARADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "rk3576_memorymap.h"
#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets (TRM §18.4.2). */

#define RK3576_SARADC_CONV_CON   0x0000 /* Conversion control        */
#define RK3576_SARADC_T_PD_SOC   0x0004 /* Timing: PD -> SOC         */
#define RK3576_SARADC_T_AS_SOC   0x0008 /* Timing: assert SOC        */
#define RK3576_SARADC_T_DAS_SOC  0x000C /* Timing: deassert SOC      */
#define RK3576_SARADC_T_SEL_SOC  0x0010 /* Timing: ch-sel -> SOC     */
#define RK3576_SARADC_HIGH_COMP0 0x0014 /* High threshold ch0        */
#define RK3576_SARADC_HIGH_COMP1 0x0018 /* High threshold ch1        */
#define RK3576_SARADC_HIGH_COMP2 0x001C /* High threshold ch2        */
#define RK3576_SARADC_HIGH_COMP3 0x0020 /* High threshold ch3        */
#define RK3576_SARADC_HIGH_COMP4 0x0024 /* High threshold ch4        */
#define RK3576_SARADC_HIGH_COMP5 0x0028 /* High threshold ch5        */
#define RK3576_SARADC_HIGH_COMP6 0x002C /* High threshold ch6        */
#define RK3576_SARADC_HIGH_COMP7 0x0030 /* High threshold ch7        */
#define RK3576_SARADC_LOW_COMP0  0x0054 /* Low threshold ch0         */
#define RK3576_SARADC_LOW_COMP1  0x0058 /* Low threshold ch1         */
#define RK3576_SARADC_LOW_COMP2  0x005C /* Low threshold ch2         */
#define RK3576_SARADC_LOW_COMP3  0x0060 /* Low threshold ch3         */
#define RK3576_SARADC_LOW_COMP4  0x0064 /* Low threshold ch4         */
#define RK3576_SARADC_LOW_COMP5  0x0068 /* Low threshold ch5         */
#define RK3576_SARADC_LOW_COMP6  0x006C /* Low threshold ch6         */
#define RK3576_SARADC_LOW_COMP7  0x0070 /* Low threshold ch7         */
#define RK3576_SARADC_DEBOUNCE   0x0094 /* Threshold debounce        */
#define RK3576_SARADC_HT_INT_EN  0x0098 /* High threshold int en     */
#define RK3576_SARADC_LT_INT_EN  0x009C /* Low threshold int en      */
#define RK3576_SARADC_MT_INT_EN  0x0100 /* Middle threshold int en   */
#define RK3576_SARADC_END_INT_EN 0x0104 /* End-of-conversion int en  */
#define RK3576_SARADC_ST_CON     0x0108 /* ADC static control        */
#define RK3576_SARADC_STATUS     0x010C /* ADC status                */
#define RK3576_SARADC_END_INT_ST 0x0110 /* End-of-conversion int st  */
#define RK3576_SARADC_HT_INT_ST  0x0114 /* High threshold int st     */
#define RK3576_SARADC_LT_INT_ST  0x0118 /* Low threshold int st      */
#define RK3576_SARADC_MT_INT_ST  0x011C /* Middle threshold int st   */
#define RK3576_SARADC_DATA0      0x0120 /* ADC data ch0              */
#define RK3576_SARADC_DATA1      0x0124 /* ADC data ch1              */
#define RK3576_SARADC_DATA2      0x0128 /* ADC data ch2              */
#define RK3576_SARADC_DATA3      0x012C /* ADC data ch3              */
#define RK3576_SARADC_DATA4      0x0130 /* ADC data ch4              */
#define RK3576_SARADC_DATA5      0x0134 /* ADC data ch5              */
#define RK3576_SARADC_DATA6      0x0138 /* ADC data ch6              */
#define RK3576_SARADC_DATA7      0x013C /* ADC data ch7              */
#define RK3576_SARADC_AUTO_CH_EN 0x0160 /* Auto channel enable       */

/* CONV_CON bits (TRM §18.4.3).  write_enable is hiword-masked [31:16]. */

#define SARADC_CONV_EN_SHIFT    16   /* hiword wren       */
#define SARADC_CONV_CHSEL_MASK  0x0f /* channel_sel [3:0] */
#define SARADC_CONV_CHSEL_SHIFT 0
#define SARADC_CONV_START       (1 << 4) /* start_adc         */
#define SARADC_CONV_SINGLE_PD   (1 << 5) /* single_pd_mode    */
#define SARADC_CONV_AUTO_CH     (1 << 6) /* auto_channel_mode */
#define SARADC_CONV_END         (1 << 7) /* end_conv          */

/* STATUS bits (TRM §18.4.3). */

#define SARADC_STATUS_BUSY      (1 << 0) /* conv_st           */
#define SARADC_STATUS_PD        (1 << 1) /* power down        */
#define SARADC_STATUS_SEL_SHIFT 2        /* channel select    */
#define SARADC_STATUS_SEL_MASK  (0x3f << 2)

/* END_INT_EN / END_INT_ST bit. */

#define SARADC_END_INT_EN_BIT 0
#define SARADC_END_INT_ST_BIT 0

/* ST_CON defaults (ictrl = bits[4:2], reset 0x4). */

#define SARADC_ST_CON_ICTRL_SHIFT 2
#define SARADC_ST_CON_ICTRL_MASK  (0x7 << SARADC_ST_CON_ICTRL_SHIFT)

/* Timing register reset values (TRM §18.4.2). */

#define SARADC_T_PD_SOC_RST  0x13
#define SARADC_T_AS_SOC_RST  0x05
#define SARADC_T_DAS_SOC_RST 0x07
#define SARADC_T_SEL_SOC_RST 0x03

/* Number of SARADC channels. */

#define RK3576_SARADC_NCHANNELS 8

/* 12-bit conversion result mask. */

#define SARADC_DATA_MASK 0x0fff

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SARADC_H */
