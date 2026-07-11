/****************************************************************************
 * chips/rk3576/rk3576_i2c.c
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
 * RK3576 I2C master driver (Rockchip RK I2C block, "rk3399-i2c" compatible).
 *
 * Polled implementation: brings up the controller clock via the CRU, then
 * runs each i2c_msg over the master TX/RX FIFO with repeated-START between
 * messages.  Sufficient for the common register-access pattern (a short
 * write of the register pointer followed by a read), e.g. the on-board
 * hym8563 RTC on I2C2 (validated on hardware, i2c ACK + register read-back).
 *
 * Pin muxing is the board's responsibility (there is no pinctrl framework
 * yet); this driver only owns the controller and its clock gate.  A single
 * message payload must fit the 32-byte FIFO (<= 31 data bytes).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/i2c/i2c_master.h>

#include "arm64_internal.h"
#include "hardware/rk3576_i2c.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_i2c.h"

#ifdef CONFIG_RK3576_I2C

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_I2C_FIFO_BYTES   32
#define RK3576_I2C_POLL_LIMIT   1000000       /* busy-wait iterations */
#define RK3576_I2C_NUM           10            /* I2C0 .. I2C9 */

/* Default functional clock into the divider (Hz).  The clk_i2c mux is left
 * at its reset default; re-muxing it here is unnecessary and risks selecting
 * a stopped parent.  This 100 MHz input matches the CLKDIV the bare-metal
 * bring-up code used successfully on hardware.
 */

#define RK3576_I2C_CLKIN         100000000

/* I2C controller base addresses (TRM §32.4.1). */

static const uintptr_t g_rk3576_i2c_base[RK3576_I2C_NUM] =
{
  RK3576_I2C0_ADDR,
  RK3576_I2C1_ADDR,
  RK3576_I2C2_ADDR,
  RK3576_I2C3_ADDR,
  RK3576_I2C4_ADDR,
  RK3576_I2C5_ADDR,
  RK3576_I2C6_ADDR,
  RK3576_I2C7_ADDR,
  RK3576_I2C8_ADDR,
  RK3576_I2C9_ADDR,
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_i2c_priv_s
{
  struct i2c_master_s  dev;      /* Base class (must be first) */
  uintptr_t            base;     /* Controller base address */
  uint32_t             clkin;    /* Functional clock into the divider (Hz) */
  mutex_t              lock;      /* Serialize bus access */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_i2c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2c_ops_s g_rk3576_i2c_ops =
{
  .transfer = rk3576_i2c_transfer,
};

static struct rk3576_i2c_priv_s g_rk3576_i2c[RK3576_I2C_NUM];
static bool g_rk3576_i2c_inited[RK3576_I2C_NUM];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t i2c_getreg(struct rk3576_i2c_priv_s *priv,
                                  unsigned int off)
{
  return getreg32(priv->base + off);
}

static inline void i2c_putreg(struct rk3576_i2c_priv_s *priv,
                              unsigned int off, uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_i2c_clk_enable
 *
 * Description:
 *   Ungate the controller's bus + functional clock in the CRU.
 ****************************************************************************/

static void rk3576_i2c_clk_enable(struct rk3576_i2c_priv_s *priv, int bus)
{
  /* TODO: move CRU clock-gate knowledge into a proper cru driver.  This
   * switch is a temporary stop-gap until rk3576_cru provides a public
   * clk_enable("i2cXX_pclk") / clk_enable("i2cXX_cclk") API.
   */

  uint8_t  gate;
  uint16_t pclk;
  uint16_t cclk;

  switch (bus)
    {
      case 0:  gate = 10; pclk = 1 << 0;  cclk = 1 << 8;  break;
      case 1:  gate = 12; pclk = 1 << 0;  cclk = 1 << 12; break;
      case 2:  gate = 12; pclk = 1 << 1;  cclk = 1 << 13; break;
      case 3:  gate = 12; pclk = 1 << 2;  cclk = 1 << 14; break;
      case 4:  gate = 12; pclk = 1 << 3;  cclk = 1 << 15; break;
      case 5:  gate = 13; pclk = 1 << 12; cclk = 1 << 13; break;
      case 6:  gate = 13; pclk = 1 << 0;  cclk = 1 << 1;  break;
      case 7:  gate = 13; pclk = 1 << 2;  cclk = 1 << 3;  break;
      case 8:  gate = 13; pclk = 1 << 4;  cclk = 1 << 14; break;
      case 9:  gate = 13; pclk = 1 << 5;  cclk = 1 << 15; break;
      default: return;
    }

  putreg32((uint32_t)(pclk | cclk) << 16, RK3576_CRU_GATE(gate));
}

/****************************************************************************
 * Name: rk3576_i2c_setclk
 *
 * Description:
 *   Program CLKDIV for the requested SCL frequency.  The RK I2C SCL period
 *   is 8 * (divh + divl + 2) input-clock cycles; split evenly.
 ****************************************************************************/

static void rk3576_i2c_setclk(struct rk3576_i2c_priv_s *priv,
                              uint32_t frequency)
{
  uint32_t div;
  uint32_t divh;
  uint32_t divl;

  if (frequency == 0)
    {
      frequency = 100000;
    }

  /* total = clkin / freq = 8 * (divh + divl + 2) */

  div = (priv->clkin + (frequency * 8) - 1) / (frequency * 8);
  if (div < 2)
    {
      div = 2;
    }

  div  -= 2;
  divh  = div / 2;
  divl  = div - divh;

  i2c_putreg(priv, RK3576_I2C_CLKDIV, (divh << 16) | divl);
}

/****************************************************************************
 * Name: rk3576_i2c_poll
 *
 * Description:
 *   Spin until one of the IPD mask bits sets; returns the raw IPD or a
 *   negated errno on timeout / NAK.
 ****************************************************************************/

static int rk3576_i2c_poll(struct rk3576_i2c_priv_s *priv, uint32_t mask)
{
  uint32_t ipd;
  int t;

  for (t = 0; t < RK3576_I2C_POLL_LIMIT; t++)
    {
      ipd = i2c_getreg(priv, RK3576_I2C_IPD);
      if ((ipd & RK3576_I2C_INT_NAKRCV) != 0)
        {
          return -ENXIO;
        }

      if ((ipd & mask) != 0)
        {
          return (int)ipd;
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_i2c_write_msg
 *
 * Description:
 *   Transmit [addr|W] followed by the message payload through the TX FIFO.
 ****************************************************************************/

static int rk3576_i2c_write_msg(struct rk3576_i2c_priv_s *priv,
                                struct i2c_msg_s *msg)
{
  uint32_t fifo[8] = { 0 };
  uint32_t total = (uint32_t)msg->length + 1;
  uint32_t i;
  int ret;

  if (total > RK3576_I2C_FIFO_BYTES)
    {
      return -EINVAL;
    }

  fifo[0] = (uint32_t)(msg->addr << 1);          /* addr | W in byte 0 */
  for (i = 0; i < (uint32_t)msg->length; i++)
    {
      fifo[(i + 1) >> 2] |= (uint32_t)msg->buffer[i] << (((i + 1) & 3) * 8);
    }

  for (i = 0; i < ((total + 3) >> 2); i++)
    {
      i2c_putreg(priv, RK3576_I2C_TXDATA0 + i * 4, fifo[i]);
    }

  /* Program TX mode + START in the same CON write, then trigger the
   * transfer by loading MTXCNT.  Per TRM §32.6 Fig.32-6, MTXCNT is the
   * final write that kicks off the transaction.
   */

  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);
  i2c_putreg(priv, RK3576_I2C_CON,
             RK3576_I2C_CON_EN | RK3576_I2C_CON_START |
             RK3576_I2C_CON_MODE_TX);
  i2c_putreg(priv, RK3576_I2C_MTXCNT, total);

  ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_MBTF);
  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_MBTF);
  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: rk3576_i2c_read_msg
 *
 * Description:
 *   Receive msg->length bytes.  The controller emits [addr|R] from MRXADDR
 *   itself (no register-pointer byte), then clocks in the data.
 ****************************************************************************/

static int rk3576_i2c_read_msg(struct rk3576_i2c_priv_s *priv,
                               struct i2c_msg_s *msg)
{
  uint32_t i;
  int ret;

  if (msg->length == 0 || msg->length > RK3576_I2C_FIFO_BYTES)
    {
      return -EINVAL;
    }

  i2c_putreg(priv, RK3576_I2C_MRXADDR,
             (uint32_t)((msg->addr << 1) | 1) | RK3576_I2C_ADDR_LOW_VLD);
  i2c_putreg(priv, RK3576_I2C_MRXRADDR, 0);

  /* Issue the (repeated) START together with the receive mode.  This block
   * uses TRX mode with one valid MRXADDR byte and zero MRXRADDR bytes: the
   * controller sends [addr|R] from MRXADDR and then clocks in the data --
   * verified on hardware (pure RX mode 2 was NAKed on this IP).  START must
   * be in the same CON write, else the repeated START is consumed and the
   * read address is never sent.
   */

  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);
  i2c_putreg(priv, RK3576_I2C_CON,
             RK3576_I2C_CON_EN | RK3576_I2C_CON_START |
             RK3576_I2C_CON_MODE_TRX);
  i2c_putreg(priv, RK3576_I2C_MRXCNT, (uint32_t)msg->length);

  ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_MBRF);
  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_MBRF);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < (uint32_t)msg->length; i++)
    {
      uint32_t w = i2c_getreg(priv, RK3576_I2C_RXDATA0 + (i >> 2) * 4);
      msg->buffer[i] = (uint8_t)(w >> ((i & 3) * 8));
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_stop
 ****************************************************************************/

static void rk3576_i2c_stop(struct rk3576_i2c_priv_s *priv)
{
  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);
  i2c_putreg(priv, RK3576_I2C_CON, RK3576_I2C_CON_EN | RK3576_I2C_CON_STOP);
  rk3576_i2c_poll(priv, RK3576_I2C_INT_STOP);
  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_STOP);
  i2c_putreg(priv, RK3576_I2C_CON, 0);
}

/****************************************************************************
 * Name: rk3576_i2c_transfer
 ****************************************************************************/

static int rk3576_i2c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count)
{
  struct rk3576_i2c_priv_s *priv = (struct rk3576_i2c_priv_s *)dev;
  int ret = OK;
  int attempt;
  int i;

  nxmutex_lock(&priv->lock);

  /* The controller drops the very first START it is asked to generate after
   * power-up; a single retry (with a STOP + disable to reset the bus in
   * between) makes that first transfer reliable and is harmless afterwards.
   */

  for (attempt = 0; attempt < 2; attempt++)
    {
      for (i = 0; i < count; i++)
        {
          rk3576_i2c_setclk(priv, msgs[i].frequency);

          if ((msgs[i].flags & I2C_M_READ) != 0)
            {
              ret = rk3576_i2c_read_msg(priv, &msgs[i]);
            }
          else
            {
              ret = rk3576_i2c_write_msg(priv, &msgs[i]);
            }

          if (ret < 0)
            {
              break;
            }
        }

      rk3576_i2c_stop(priv);
      if (ret >= 0)
        {
          break;
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_i2c_initialize
 ****************************************************************************/

struct i2c_master_s *rk3576_i2c_initialize(int bus)
{
  struct rk3576_i2c_priv_s *priv;

  if (bus < 0 || bus >= RK3576_I2C_NUM)
    {
      i2cerr("ERROR: unsupported I2C bus %d\n", bus);
      return NULL;
    }

  priv = &g_rk3576_i2c[bus];

  if (!g_rk3576_i2c_inited[bus])
    {
      g_rk3576_i2c_inited[bus] = true;
      priv->dev.ops  = &g_rk3576_i2c_ops;
      priv->base     = g_rk3576_i2c_base[bus];
      priv->clkin    = RK3576_I2C_CLKIN;
      nxmutex_init(&priv->lock);
      rk3576_i2c_clk_enable(priv, bus);
      i2c_putreg(priv, RK3576_I2C_CON, 0);
    }

  return &priv->dev;
}

#endif /* CONFIG_RK3576_I2C */
