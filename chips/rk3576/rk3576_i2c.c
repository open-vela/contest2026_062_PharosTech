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
 * yet); this driver only owns the controller and its clock gate.
 * Transfers larger than the 32-byte FIFO are split into chunks internally.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <debug.h>
#include <errno.h>
#include <nuttx/config.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "hardware/rk3576_i2c.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_cru.h"
#include "rk3576_i2c.h"

#ifdef CONFIG_RK3576_I2C

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_I2C_FIFO_BYTES 32
#define RK3576_I2C_POLL_LIMIT 1000000 /* busy-wait iterations */
#define RK3576_I2C_NUM        10      /* I2C0 .. I2C9 */

/* Default functional clock into the divider (Hz).
 * TODO: calculate clock frequency automatically
 * after cru driver is ready
 */

#define RK3576_I2C_CLKIN 200000000

/* I2C controller base addresses (TRM §32.4.1). */

static const uintptr_t g_rk3576_i2c_base[RK3576_I2C_NUM] = {
  RK3576_I2C0_ADDR, RK3576_I2C1_ADDR, RK3576_I2C2_ADDR, RK3576_I2C3_ADDR,
  RK3576_I2C4_ADDR, RK3576_I2C5_ADDR, RK3576_I2C6_ADDR, RK3576_I2C7_ADDR,
  RK3576_I2C8_ADDR, RK3576_I2C9_ADDR,
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_i2c_priv_s
{
  struct i2c_master_s dev; /* Base class (must be first) */
  uintptr_t base;          /* Controller base address */
  uint32_t clkin;          /* Functional clock into the divider (Hz) */
  mutex_t lock;            /* Serialize bus access */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_i2c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2c_ops_s g_rk3576_i2c_ops = {
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

static inline void i2c_putreg(struct rk3576_i2c_priv_s *priv, unsigned int off,
                              uint32_t val)
{
  putreg32(val, priv->base + off);
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

  div -= 2;
  divh = div / 2;
  divl = div - divh;

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
 * Name: rk3576_i2c_write_chunk
 *
 * Description:
 *   Transmit len payload bytes.  The first chunk includes START + device
 *   address; subsequent chunks are pure data (no START, no address) and
 *   ride on the same I2C transaction.
 ****************************************************************************/

static int rk3576_i2c_write_chunk(struct rk3576_i2c_priv_s *priv,
                                  uint16_t addr, FAR const uint8_t *buf,
                                  uint32_t len, bool first)
{
  uint32_t fifo[8] = { 0 };
  uint32_t total;
  uint32_t i;

  if (first)
    {
      /* First chunk: address byte + len payload bytes */

      total = len + 1;
      DEBUGASSERT(total <= RK3576_I2C_FIFO_BYTES && len > 0);

      fifo[0] = (uint32_t)(addr << 1); /* addr | W */
      for (i = 0; i < len; i++)
        {
          fifo[(i + 1) >> 2] |= (uint32_t)buf[i] << (((i + 1) & 3) * 8);
        }
    }
  else
    {
      /* Subsequent chunk: pure data, no address byte */

      total = len;
      DEBUGASSERT(total <= RK3576_I2C_FIFO_BYTES && len > 0);

      for (i = 0; i < len; i++)
        {
          fifo[i >> 2] |= (uint32_t)buf[i] << ((i & 3) * 8);
        }
    }

  for (i = 0; i < ((total + 3) >> 2); i++)
    {
      i2c_putreg(priv, RK3576_I2C_TXDATA0 + i * 4, fifo[i]);
    }

  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);
  i2c_putreg(priv, RK3576_I2C_CON,
             RK3576_I2C_CON_EN | (first ? RK3576_I2C_CON_START : 0) |
                 RK3576_I2C_CON_MODE_TX);
  i2c_putreg(priv, RK3576_I2C_MTXCNT, total);

  return rk3576_i2c_poll(priv, RK3576_I2C_INT_MBTF);
}

/****************************************************************************
 * Name: rk3576_i2c_write_msg
 *
 * Description:
 *   Transmit msg->buffer bytes.  If the payload exceeds the 32-byte FIFO
 *   the transfer is split into chunks internally; all but the first chunk
 *   use a repeated START without STOP.
 ****************************************************************************/

static int rk3576_i2c_write_msg(struct rk3576_i2c_priv_s *priv,
                                struct i2c_msg_s *msg)
{
  uint32_t remaining = (uint32_t)msg->length;
  uint32_t offset = 0;
  bool first = true;
  int ret;

  if (remaining == 0)
    {
      return -EINVAL;
    }

  while (remaining > 0)
    {
      uint32_t limit =
          first ? (RK3576_I2C_FIFO_BYTES - 1) : RK3576_I2C_FIFO_BYTES;
      uint32_t chunk = remaining;

      if (chunk > limit)
        {
          chunk = limit;
        }

      ret = rk3576_i2c_write_chunk(priv, msg->addr, msg->buffer + offset,
                                   chunk, first);
      if (ret < 0)
        {
          i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);
          return ret;
        }

      i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);
      first = false;
      remaining -= chunk;
      offset += chunk;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_read_chunk
 *
 * Description:
 *   Receive a chunk of up to 32 bytes.  The first chunk uses TRX mode
 *   (START + MRXADDR + MRXRADDR + restart + receive).  Subsequent chunks
 *   switch to RX-only mode and continue receiving without a new START or
 *   address, as documented in TRM §32.6 Fig.32-8 (mix mode flow chart).
 ****************************************************************************/

static int rk3576_i2c_read_chunk(struct rk3576_i2c_priv_s *priv, uint16_t addr,
                                 FAR uint8_t *buf, uint32_t len, bool first)
{
  uint32_t i;
  int ret;

  DEBUGASSERT(len > 0 && len <= RK3576_I2C_FIFO_BYTES);

  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);

  if (first)
    {
      /* TRX mode: START + send MRXADDR[M] + MRXRADDR + restart +
       * send MRXADDR[R] + receive len bytes.
       */

      i2c_putreg(priv, RK3576_I2C_MRXADDR,
                 (uint32_t)((addr << 1) | 1) | RK3576_I2C_ADDR_LOW_VLD);
      i2c_putreg(priv, RK3576_I2C_MRXRADDR, 0);

      i2c_putreg(priv, RK3576_I2C_CON,
                 RK3576_I2C_CON_EN | RK3576_I2C_CON_START |
                     RK3576_I2C_CON_MODE_TRX);
    }
  else
    {
      /* RX-only mode: continue receiving on the same transaction, no
       * new START, no address.  Set LASTACK=0 so the controller sends
       * ACK after the last byte (not NAK), keeping the bus alive for
       * the next chunk.
       */

      i2c_putreg(priv, RK3576_I2C_CON,
                 RK3576_I2C_CON_EN | RK3576_I2C_CON_MODE_RX);
    }

  i2c_putreg(priv, RK3576_I2C_MRXCNT, len);

  ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_MBRF);
  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_MBRF);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < len; i++)
    {
      uint32_t w = i2c_getreg(priv, RK3576_I2C_RXDATA0 + (i >> 2) * 4);
      buf[i] = (uint8_t)(w >> ((i & 3) * 8));
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_read_msg
 *
 * Description:
 *   Receive msg->length bytes in chunks of up to 32 bytes.  All but the
 *   first chunk use repeated START.
 ****************************************************************************/

static int rk3576_i2c_read_msg(struct rk3576_i2c_priv_s *priv,
                               struct i2c_msg_s *msg)
{
  uint32_t remaining = (uint32_t)msg->length;
  uint32_t offset = 0;
  bool first = true;
  int ret;

  if (remaining == 0)
    {
      return -EINVAL;
    }

  while (remaining > 0)
    {
      uint32_t chunk = remaining;
      if (chunk > RK3576_I2C_FIFO_BYTES)
        {
          chunk = RK3576_I2C_FIFO_BYTES;
        }

      /* First chunk: TRX mode.  Subsequent chunks: RX-only mode,
       * continuing the same transaction per TRM §32.6 Fig.32-8.
       */

      ret = rk3576_i2c_read_chunk(priv, msg->addr, msg->buffer + offset, chunk,
                                  first);
      if (ret < 0)
        {
          i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);
          return ret;
        }

      i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);
      first = false;
      remaining -= chunk;
      offset += chunk;
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
      priv->dev.ops = &g_rk3576_i2c_ops;
      priv->base = g_rk3576_i2c_base[bus];
      priv->clkin = RK3576_I2C_CLKIN;
      nxmutex_init(&priv->lock);
      rk3576_cru_set_i2c_clock_gate(bus, true, true);
      i2c_putreg(priv, RK3576_I2C_CON, 0);
    }

  return &priv->dev;
}

#endif /* CONFIG_RK3576_I2C */
