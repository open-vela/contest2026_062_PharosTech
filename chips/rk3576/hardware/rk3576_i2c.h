/****************************************************************************
 * chips/rk3576/hardware/rk3576_i2c.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I2C_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I2C_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets (RK I2C block) */

#define RK3576_I2C_CON      0x0000 /* Control */
#define RK3576_I2C_CLKDIV   0x0004 /* Clock divider */
#define RK3576_I2C_MRXADDR  0x0008 /* Slave address for master receive */
#define RK3576_I2C_MRXRADDR 0x000c /* Slave register address (rx) */
#define RK3576_I2C_MTXCNT   0x0010 /* Master transmit byte count */
#define RK3576_I2C_MRXCNT   0x0014 /* Master receive byte count */
#define RK3576_I2C_IEN      0x0018 /* Interrupt enable */
#define RK3576_I2C_IPD      0x001c /* Interrupt pending (write-1-clear) */
#define RK3576_I2C_FCNT     0x0020 /* Finished byte count */
#define RK3576_I2C_TXDATA0  0x0100 /* TX data (0..7, 8 x 32-bit) */
#define RK3576_I2C_RXDATA0  0x0200 /* RX data (0..7, 8 x 32-bit) */

/* CON bits */

#define RK3576_I2C_CON_EN             (1 << 0) /* Enable controller */

#define RK3576_I2C_CON_MODE_SHIFT     1
#define RK3576_I2C_CON_MODE_MASK      (0x3 << 1)
#define RK3576_I2C_CON_MODE_TX        (0 << 1) /* Tx only */
#define RK3576_I2C_CON_MODE_TRX       (1 << 1) /* Tx addr, reg (opt) then rx */
#define RK3576_I2C_CON_MODE_RX        (2 << 1) /* Rx only */

#define RK3576_I2C_CON_START          (1 << 3) /* Generate START */
#define RK3576_I2C_CON_STOP           (1 << 4) /* Generate STOP */
#define RK3576_I2C_CON_LAST_BYTE_NACK (1 << 5) /* NAK the last rx byte */
#define RK3576_I2C_CON_ACT_TO_NAK     (1 << 6) /* Stop on slave NAK */

/* IPD / IEN bits */

#define RK3576_I2C_INT_BTF    (1 << 0) /* Byte tx finished */
#define RK3576_I2C_INT_BRF    (1 << 1) /* Byte rx finished */
#define RK3576_I2C_INT_MBTF   (1 << 2) /* Master tx finished */
#define RK3576_I2C_INT_MBRF   (1 << 3) /* Master rx finished */
#define RK3576_I2C_INT_START  (1 << 4) /* START done */
#define RK3576_I2C_INT_STOP   (1 << 5) /* STOP done */
#define RK3576_I2C_INT_NAKRCV (1 << 6) /* NAK received */
#define RK3576_I2C_INT_ALL    0x7f

/* MRXADDR/MRXRADDR: low 24 bits are the address bytes, bits 24..26 mark
 * how many address bytes are valid (bit24 = byte0 valid, ...).
 */

#define RK3576_I2C_ADDR_LOW_VLD (1 << 24) /* byte 0 (low) valid */

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I2C_H */
