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
 * Name: rk3576_i2c_tx
 *
 * Description:
 *   Handle tx.
 *   Returns OK on success, negated errno on failure.
 *
 ****************************************************************************/

static int rk3576_i2c_tx(struct rk3576_i2c_priv_s *priv,
                         struct i2c_msg_s *p_msg)
{
  int ret;
  const uint32_t length = (uint32_t)p_msg->length;
  uint32_t remaining = length;

  /* first byte initiallized with device addr */
  uint8_t tx_byte_cnt = 1;
  uint32_t pack = (p_msg->addr << 1) << 0;
  uint8_t byte_offset;

  /* enable i2c and start tx */

  i2c_putreg(priv, RK3576_I2C_CON,
             RK3576_I2C_CON_EN | RK3576_I2C_CON_START |
                 /* stop tx if NACK received */
                 RK3576_I2C_CON_ACT_TO_NAK |
                 /* tx-only mode */
                 RK3576_I2C_CON_MODE_TX);

  ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_START);
  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_START);
  if (ret < 0)
    {
      return ret;
    }

  while (remaining >= 32)
    {
      /* fill FIFO */
      while (tx_byte_cnt < 32)
        {
          byte_offset = tx_byte_cnt % 4;
          pack |= (p_msg->buffer[length - remaining]) << (byte_offset * 8);
          remaining--;
          tx_byte_cnt++;
          if (byte_offset == 3)
            {
              i2c_putreg(priv, RK3576_I2C_TXDATA0 + tx_byte_cnt - 4, pack);
              pack = 0;
            }
        }

      /* FIFO full, send and poll */

      i2c_putreg(priv, RK3576_I2C_MTXCNT, 32);

      ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_MBTF);
      i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_MBTF);
      if (ret < 0)
        {
          return ret;
        }

      /* reset byte cnt */

      tx_byte_cnt = 0;
    }

  /* send remaining bytes */

  if (remaining)
    {
      while (remaining)
        {
          byte_offset = tx_byte_cnt % 4;
          pack |= (p_msg->buffer[length - remaining]) << (byte_offset * 8);
          remaining--;
          if (!remaining || byte_offset == 3)
            {
              i2c_putreg(priv, RK3576_I2C_TXDATA0 + (tx_byte_cnt / 4) * 4,
                         pack);
              pack = 0;
            }
          tx_byte_cnt++;
        }

      i2c_putreg(priv, RK3576_I2C_MTXCNT, tx_byte_cnt);

      ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_MBTF);
      i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_MBTF);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_rx
 *
 * Description:
 *   Handle rx.
 *   Returns OK on success, negated errno on failure.
 *
 ****************************************************************************/

static int rk3576_i2c_rx(struct rk3576_i2c_priv_s *priv,
                         struct i2c_msg_s *p_msg)
{
  int ret;
  const uint32_t length = (uint32_t)p_msg->length;
  uint32_t remaining = length;
  uint8_t rx_byte_cnt;
  uint32_t pack;
  uint8_t byte_offset;
  bool first = true;

  /* If the total request fits in one FIFO (≤32 bytes), the very first
   * transfer is also the last — we must tell the controller to NACK the
   * final byte so the slave releases the bus.
   */
  bool gen_nack = remaining <= 32;

  /* enable i2c and start rx */

  i2c_putreg(priv, RK3576_I2C_CON,
             RK3576_I2C_CON_EN | RK3576_I2C_CON_START |
                 (gen_nack ? RK3576_I2C_CON_LAST_BYTE_NACK : 0) |
                 /* use TRX mode to send device address
                  * and receive first chunk of data
                  */
                 RK3576_I2C_CON_MODE_TRX);

  ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_START);
  i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_START);
  if (ret < 0)
    {
      return ret;
    }

  /* set device rx address */

  i2c_putreg(priv, RK3576_I2C_MRXADDR,
             (uint32_t)((p_msg->addr << 1) | 1) | RK3576_I2C_ADDR_LOW_VLD);

  /* set mrxr to 0, do not send any register addresses */

  i2c_putreg(priv, RK3576_I2C_MRXRADDR, 0);

  /* receive from device */

  while (remaining >= 32)
    {
      i2c_putreg(priv, RK3576_I2C_MRXCNT, 32);

      ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_MBRF);
      i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_MBRF);
      if (ret < 0)
        {
          return ret;
        }

      for (uint8_t i = 0; i < 8; i++)
        {
          uint8_t *buffer = &(p_msg->buffer[length - remaining]);
          pack = i2c_getreg(priv, RK3576_I2C_RXDATA0 + i * 4);
          buffer[0] = (pack >> 0) & 0xff;
          buffer[1] = (pack >> 8) & 0xff;
          buffer[2] = (pack >> 16) & 0xff;
          buffer[3] = (pack >> 24) & 0xff;
          remaining -= 4;
        }

      /* If the total length was an exact multiple of 32, the FIFO
       * loop above consumed everything — remaining == 0 now and the
       * next iteration won't execute. Generate NACK.
       *
       * If this is the very first chunk
       * we need to switch to rx-only mode
       * to continue receving following chunk.
       */

      gen_nack = (remaining == 0);
      if (first || gen_nack)
        {
          i2c_putreg(priv, RK3576_I2C_CON,
                     RK3576_I2C_CON_EN |
                         (gen_nack ? RK3576_I2C_CON_LAST_BYTE_NACK : 0) |
                         RK3576_I2C_CON_MODE_RX);
          first = false;
        }
    }

  /* receive remaining bytes */

  if (remaining)
    {
      /* If we reach here, remaining > 0 and gen_nack is false — the
       * FIFO loop above didn't finish the transfer, so we haven't
       * enabled LAST_BYTE_NACK yet.  Do it now for this final chunk.
       */
      if (!gen_nack)
        {
          /* enable nack gen*/
          i2c_putreg(priv, RK3576_I2C_CON,
                     RK3576_I2C_CON_EN | RK3576_I2C_CON_LAST_BYTE_NACK |
                         RK3576_I2C_CON_MODE_RX);
        }

      i2c_putreg(priv, RK3576_I2C_MRXCNT, remaining);

      ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_MBRF);
      i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_MBRF);
      if (ret < 0)
        {
          return ret;
        }

      rx_byte_cnt = 0;
      pack = i2c_getreg(priv, RK3576_I2C_RXDATA0);
      while (remaining)
        {
          byte_offset = rx_byte_cnt % 4;
          p_msg->buffer[length - remaining] =
              (pack >> (byte_offset * 8)) & 0xff;
          remaining--;
          rx_byte_cnt++;
          if (remaining && byte_offset == 3)
            {
              pack = i2c_getreg(priv, RK3576_I2C_RXDATA0 + rx_byte_cnt);
            }
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_transfer
 ****************************************************************************/

static int rk3576_i2c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count)
{
  struct rk3576_i2c_priv_s *priv = (struct rk3576_i2c_priv_s *)dev;
  int ret = OK;

  nxmutex_lock(&priv->lock);

  for (int i = 0; i < count; i++)
    {
      struct i2c_msg_s *msg = &msgs[i];

      if (msg->length == 0)
        {
          ret = -EINVAL;
          break;
        }

      /* set i2c clock speed */

      rk3576_i2c_setclk(priv, msg->frequency);

      /* clear interrupts and configure CON for the transaction start. */

      i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_ALL);

      if (msg->flags & I2C_M_READ)
        {
          ret = rk3576_i2c_rx(priv, msg);
        }
      else
        {
          ret = rk3576_i2c_tx(priv, msg);
        }

      /* TODO: support I2C_M_NOSTOP */

      /* send stop */

      i2c_putreg(priv, RK3576_I2C_CON,
                 RK3576_I2C_CON_EN | RK3576_I2C_CON_STOP);
      ret = rk3576_i2c_poll(priv, RK3576_I2C_INT_STOP);
      i2c_putreg(priv, RK3576_I2C_IPD, RK3576_I2C_INT_STOP);

      /* disable i2c to reset all status */

      i2c_putreg(priv, RK3576_I2C_CON, 0);

      if (ret < 0)
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
