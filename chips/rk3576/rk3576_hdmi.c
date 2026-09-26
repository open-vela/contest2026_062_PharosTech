/****************************************************************************
 * chips/rk3576/rk3576_hdmi.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/mutex.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_hdmi.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_hdmi.h"
#include "rk3576_hdptxphy.h"

#ifdef CONFIG_RK3576_HDMI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How long to wait for the controller's register bank / timers to come up
 * after the clocks have been enabled.  The controller has no documented
 * startup delay; these are generous margins around a poll that normally
 * succeeds on the first read.
 */

#define RK3576_HDMI_BANK_READY_TRIES 100
#define RK3576_HDMI_BANK_READY_DELAY 10

/* Settle time between deasserting the CRU resets and the first register
 * access.  Same rationale as above.
 */

#define RK3576_HDMI_RESET_SETTLE_US 100

/* DDC transfer timing.  One EDID byte is a handful of I2C frames at the ~92
 * kHz the SCL configuration produces, so a few hundred microseconds is the
 * normal cost and 20 ms is a generous ceiling -- a sink that has not answered
 * by then is not going to.  The poll interval is short enough that a good
 * transfer is not measurably slowed and long enough not to hammer the APB.
 */

#define RK3576_HDMI_DDC_TIMEOUT_US 20000
#define RK3576_HDMI_DDC_POLL_US    50

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* How one EDID descriptor slot is classified.  Declared as an enum rather than
 * reusing errno values so that every rejection reason has a name of its own:
 * the reader logs each slot, and "interlaced" versus "malformed" versus "not a
 * timing at all" are three different answers to "why was this mode not used".
 */

enum rk3576_hdmi_dtd_status_e
{
  RK3576_HDMI_DTD_STATUS_OK = 0, /* A usable progressive timing             */
  RK3576_HDMI_DTD_STATUS_NOT_TIMING, /* Not a detailed timing (a display
                                        descriptor) */
  RK3576_HDMI_DTD_STATUS_INTERLACED,
  RK3576_HDMI_DTD_STATUS_SYNC_SCHEME,
  RK3576_HDMI_DTD_STATUS_MALFORMED,
};

/* Driver state.  One instance: RK3576 has a single HDMI TX controller on
 * VO0, and the HDPTX PHY it drives is shared with the eDP controller, so a
 * second instance could not legally exist anyway.
 */

struct rk3576_hdmi_s
{
  mutex_t lock; /* Serialises initialize/enable/disable      */

  uintptr_t base; /* HDMI controller SFR block                 */
  uintptr_t grf;  /* VO0_GRF (routing and colour format)       */
  uintptr_t ioc;  /* VCCIO6 IOC (HPD pin sampling)             */
  uintptr_t cru;  /* Main CRU (APB and reference resets)       */
  uintptr_t pmu1; /* PMU1CRU (HPD detector reset)              */

  FAR struct clk_s *pclk; /* pclk_hdmitx0: the controller's APB clock  */
  FAR struct clk_s *ref;  /* clk_hdmitx0_ref: timer reference clock    */
  FAR struct clk_s *hdp;  /* clk_hdmitxhpd: HPD sampling clock         */

  uint32_t ref_hz;      /* Cached clk_get_rate(ref)                  */
  uint32_t pixel_clock; /* Pixel clock of the enabled mode, Hz       */
  uint8_t bpc;          /* Bits per colour component of that mode    */

  bool initialized; /* Clocks up, resets released, GRF programmed */
  bool streaming;   /* A mode has been enabled                   */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_hdmi_s g_rk3576_hdmi = {
  .lock = NXMUTEX_INITIALIZER,
  .base = RK3576_HDMITX_ADDR,
  .grf = RK3576_VO0_GRF_ADDR,
  .ioc = RK3576_VCCIO6_IOC_ADDR,
  .cru = RK3576_CRU_ADDR,
  .pmu1 = RK3576_PMU1_CRU_ADDR,
  .initialized = false,
  .streaming = false,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdmi_getreg
 *
 * Description:
 *   Read one of the controller's 32-bit registers.
 *
 ****************************************************************************/

static inline uint32_t rk3576_hdmi_getreg(struct rk3576_hdmi_s *priv,
                                          uint32_t offset)
{
  return getreg32(priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_hdmi_putreg
 *
 * Description:
 *   Write one of the controller's 32-bit registers.
 *
 ****************************************************************************/

static inline void rk3576_hdmi_putreg(struct rk3576_hdmi_s *priv,
                                      uint32_t offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_hdmi_modifyreg
 *
 * Description:
 *   Read-modify-write one of the controller's 32-bit registers.
 *
 *   Only usable on registers without write-one-to-clear fields; the
 *   interrupt status registers in this controller are read-only or
 *   clear-on-write, so they are always written whole.
 *
 * Input Parameters:
 *   priv    - Driver state.
 *   offset  - Register byte offset.
 *   clrbits - Bits to clear.
 *   setbits - Bits to set.
 *
 ****************************************************************************/

static void rk3576_hdmi_modifyreg(struct rk3576_hdmi_s *priv, uint32_t offset,
                                  uint32_t clrbits, uint32_t setbits)
{
  uint32_t reg = getreg32(priv->base + offset);

  reg &= ~clrbits;
  reg |= setbits;
  putreg32(reg, priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_hdmi_grf_write
 *
 * Description:
 *   Write a masked field of a Rockchip "hiword-mask" register: bits [31:16]
 *   of the written word enable the per-bit writes of bits [15:0], so a field
 *   can be updated without a read-modify-write and without disturbing the
 *   neighbouring fields that share the register with it.  VO0_GRF, the IOC
 *   block and the CRU soft-reset registers all use this convention.
 *
 *   A read-modify-write would also be functionally wrong on the soft-reset
 *   registers, whose write-enable bits read back as zero -- reading them
 *   yields a value that would silently disable the very write being made.
 *
 * Input Parameters:
 *   base   - Block base address (VO0_GRF, IOC or CRU).
 *   offset - Register byte offset within that block.
 *   mask   - Field mask, already positioned at bits [15:0].
 *   value  - Field value, already positioned and already masked.
 *
 ****************************************************************************/

static void rk3576_hdmi_grf_write(uintptr_t base, uint32_t offset,
                                  uint32_t mask, uint32_t value)
{
  putreg32(((mask & 0xffffu) << 16) | (value & mask), base + offset);
}

/****************************************************************************
 * Name: rk3576_hdmi_ddc_init
 *
 * Description:
 *   Park the controller's embedded I2C (DDC) master in its idle, base-block
 *   state: reset the bus clock generator, program the SCL timing, and select
 *   EDID segment 0.
 *
 *   Called at bring-up and again after an aborted transfer, so that a bus a
 *   sink has wedged is left in exactly the state a fresh boot produces.
 *
 *   The SCL counts are the reference driver's and assume the 396 MHz reference
 *   clock this board programs: 0x085c = 2140 counts is a ~5.4 us half period,
 *   i.e. ~92 kHz, the standard-mode DDC rate every sink must accept.  DDC is
 *   the slow path that must not fail, so the conservative value is used
 *   deliberately rather than deriving a faster one.
 *
 * Input Parameters:
 *   priv - Driver state.
 *
 ****************************************************************************/

static void rk3576_hdmi_ddc_init(struct rk3576_hdmi_s *priv)
{
  /* Pulse the master's software reset, then program its clock.  The order
   * matters if the reset clears the timing registers, so the configuration
   * follows the reset exactly as it does in the reference driver.
   */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_I2CM_CONTROL0,
                     RK3576_HDMI_I2CM_CONTROL0_SWRESET);
  rk3576_hdmi_putreg(priv, RK3576_HDMI_I2CM_FM_SCL_CONFIG0,
                     RK3576_HDMI_I2CM_FM_SCL_CONFIG0_VAL);

  /* Drop any command left programmed and leave fast-mode off.  The command
   * field sits inside RK3576_HDMI_I2CM_WR_MASK, so it is cleared by writing 0
   * through that mask.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_I2CM_INTERFACE_CONTROL0,
                        RK3576_HDMI_I2CM_WR_MASK | RK3576_HDMI_I2CM_FM_EN, 0);

  /* Select segment 0 (the EDID base block).  Only extended reads use the
   * segment pointer, but clearing it here means a segment left over by an
   * earlier stage cannot silently redirect an ordinary read.
   */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_I2CM_INTERFACE_CONTROL1, 0);
}

/****************************************************************************
 * Name: rk3576_hdmi_ddc_read
 *
 * Description:
 *   Read len bytes from the sink's EDID EEPROM, starting at offset 0, using
 *   the controller's built-in DDC master.
 *
 *   One byte is transferred per transaction, exactly as the reference driver
 *   does it: the master sends the slave address, then the sub-address it is
 *   being given through I2CM_ADDR, then a repeated start and reads one byte.
 *   That is far slower than a block read would be, but it is the only mode
 *   this master's windowed interface exposes for an arbitrary offset, and it
 *   is a few tens of milliseconds in total.
 *
 *   No interrupt handler exists for this controller, so the transfer is driven
 *   by polling MAINUNIT_1_INT_STATUS.  The two completion bits are unmasked
 *for the duration anyway: on this controller the mask is applied to the
 *latched status, and the reference driver unmasks them around every transfer,
 *so matching that behaviour costs nothing and removes a guess.
 *
 * Input Parameters:
 *   priv - Driver state.
 *   buf  - Destination, len bytes.
 *   len  - Number of bytes to read; at most one EDID base block.
 *
 * Returned Value:
 *   Zero (OK) on success; -EINVAL for a bad length, -ETIMEDOUT if a byte never
 *   completed, or -EIO if the sink did not acknowledge.
 *
 ****************************************************************************/

static int rk3576_hdmi_ddc_read(struct rk3576_hdmi_s *priv, FAR uint8_t *buf,
                                size_t len)
{
  uint32_t elapsed;
  uint32_t status;
  uint32_t subaddr = 0;
  size_t i;
  int ret = OK;

  if (len == 0 || len > RK3576_HDMI_EDID_LENGTH)
    {
      return -EINVAL;
    }

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_MAINUNIT_1_INT_MASK_N, 0,
                        RK3576_HDMI_I2CM_OP_DONE_IRQ |
                            RK3576_HDMI_I2CM_NACK_RCVD_IRQ);

  /* The EEPROM stays selected for the whole read; each transaction re-sends
   * the address together with the next sub-address.
   */

  rk3576_hdmi_modifyreg(
      priv, RK3576_HDMI_I2CM_INTERFACE_CONTROL0, RK3576_HDMI_I2CM_SLVADDR,
      (uint32_t)RK3576_HDMI_DDC_EDID_ADDR << RK3576_HDMI_I2CM_SLVADDR_SHIFT);

  for (i = 0; i < len; i++)
    {
      /* Retire the previous byte's completion first, so the poll below cannot
       * observe it and read a stale data register.
       */

      rk3576_hdmi_putreg(priv, RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                         RK3576_HDMI_MAINUNIT_1_INT_CLEAR_VAL);

      /* Sub-address for this byte, then start a one-byte read.  Note that the
       * command is written THROUGH I2CM_WR_MASK -- I2CM_FM_READ is a value
       * inside that field, not a bit beside it.
       */

      rk3576_hdmi_modifyreg(priv, RK3576_HDMI_I2CM_INTERFACE_CONTROL0,
                            RK3576_HDMI_I2CM_ADDR,
                            subaddr << RK3576_HDMI_I2CM_ADDR_SHIFT);
      rk3576_hdmi_modifyreg(priv, RK3576_HDMI_I2CM_INTERFACE_CONTROL0,
                            RK3576_HDMI_I2CM_WR_MASK,
                            RK3576_HDMI_I2CM_FM_READ);
      subaddr++;

      for (elapsed = 0;; elapsed += RK3576_HDMI_DDC_POLL_US)
        {
          status = rk3576_hdmi_getreg(priv, RK3576_HDMI_MAINUNIT_1_INT_STATUS);
          if ((status & (RK3576_HDMI_I2CM_OP_DONE_IRQ |
                         RK3576_HDMI_I2CM_NACK_RCVD_IRQ)) != 0)
            {
              break;
            }

          if (elapsed >= RK3576_HDMI_DDC_TIMEOUT_US)
            {
              _err("ERROR: RK3576 HDMI DDC byte %u timed out after %u us "
                   "(MAINUNIT_1_INT_STATUS=%08" PRIx32 ")\n",
                   (unsigned int)i, (unsigned int)RK3576_HDMI_DDC_TIMEOUT_US,
                   status);
              ret = -ETIMEDOUT;
              goto err;
            }

          up_udelay(RK3576_HDMI_DDC_POLL_US);
        }

      if ((status & RK3576_HDMI_I2CM_NACK_RCVD_IRQ) != 0)
        {
          _err("ERROR: RK3576 HDMI DDC byte %u was not acknowledged "
               "(no sink, no +5 V on the connector, or a wedged bus)\n",
               (unsigned int)i);
          ret = -EIO;
          goto err;
        }

      *buf++ = (uint8_t)(rk3576_hdmi_getreg(
                             priv, RK3576_HDMI_I2CM_INTERFACE_RDDATA_0) &
                         0xffu);

      /* End the transaction. */

      rk3576_hdmi_modifyreg(priv, RK3576_HDMI_I2CM_INTERFACE_CONTROL0,
                            RK3576_HDMI_I2CM_WR_MASK, 0);
    }

  rk3576_hdmi_modifyreg(
      priv, RK3576_HDMI_MAINUNIT_1_INT_MASK_N,
      RK3576_HDMI_I2CM_OP_DONE_IRQ | RK3576_HDMI_I2CM_NACK_RCVD_IRQ, 0);
  return OK;

err:
  /* Abort the transaction and park the master, then restore the interrupt
   * mask.  The caller decides what to do about the failure.
   */

  rk3576_hdmi_ddc_init(priv);
  rk3576_hdmi_modifyreg(
      priv, RK3576_HDMI_MAINUNIT_1_INT_MASK_N,
      RK3576_HDMI_I2CM_OP_DONE_IRQ | RK3576_HDMI_I2CM_NACK_RCVD_IRQ, 0);
  return ret;
}

/****************************************************************************
 * Name: rk3576_hdmi_edid_valid
 *
 * Description:
 *   Apply the two checks every EDID reader must apply to a base block: the
 *   eight-byte header, and the checksum (the 128 bytes sum to 0 mod 256).
 *
 *   This is not paranoia.  DDC is an open-drain bus on a connector, so a
 *   half-seated cable produces a plausible-looking transfer of garbage, and
 *   the timing decoded from that garbage would be programmed into the VOP.
 *
 * Input Parameters:
 *   edid - 128-byte base block.
 *
 * Returned Value:
 *   true if the block is well formed.
 *
 ****************************************************************************/

static bool rk3576_hdmi_edid_valid(FAR const uint8_t *edid)
{
  static const uint8_t magic[RK3576_HDMI_EDID_MAGIC_SIZE] = {
    0x00u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x00u
  };

  unsigned int sum = 0;
  unsigned int i;

  if (memcmp(edid, magic, sizeof(magic)) != 0)
    {
      return false;
    }

  for (i = 0; i < RK3576_HDMI_EDID_LENGTH; i++)
    {
      sum += edid[i];
    }

  return (sum & 0xffu) == 0;
}

/****************************************************************************
 * Name: rk3576_hdmi_edid_dtd
 *
 * Description:
 *   Decode one 18-byte EDID detailed timing descriptor into a
 *   struct rk3576_hdmi_timing_s.
 *
 *   The bit placement is the classic EDID trap and is spelled out rather than
 *   compressed, because the high bits of every field are scattered across
 *   bytes 4, 7 and 11 in three different arrangements:
 *
 *     byte  4  [7:4] hactive[11:8]   [3:0] hblank[11:8]
 *     byte  7  [7:4] vactive[11:8]   [3:0] vblank[11:8]
 *     byte 11  [7:6] hsync offset[9:8]   [5:4] hsync width[5:4]
 *              [3:2] vsync offset[5:4]   [1:0] vsync width[5:4]
 *
 *   Rejections are deliberate and are the difference between "no picture" and
 *   "no picture with a reason in the log":
 *
 *     - a zero pixel clock means this descriptor is not a timing at all (the
 *       slots normally hold the monitor name, serial number and range limits);
 *     - an interlaced descriptor cannot be expressed by this driver's
 *       progressive scan-out;
 *     - a sync scheme other than digital separate cannot be expressed by the
 *       single hsync/vsync polarity pair rk3576_vop_config carries;
 *     - a blanking interval too short for the sync pulse and the porches is
 *       malformed and would underflow the porch arithmetic below.
 *
 *   SILENT BY DESIGN, and the reason is the descriptor scan: a sink
 *legitimately lists 1080i in a later slot of an otherwise progressive EDID, so
 *a decoder that warned about every unusable timing would fill the boot log
 *with warnings about modes nobody asked for.  The status is returned instead
 *and the one caller that logs (rk3576_hdmi_edid_dump()) reports it per slot.
 *
 *   Image borders (descriptor bytes 15 and 16) are deliberately ignored: they
 *   are contained within the blanking intervals, so absorbing them into the
 *   back porch reproduces the same totals, which is what the VOP programs.
 *
 * Input Parameters:
 *   desc   - The 18 descriptor bytes.
 *   timing - Receives the decoded timing when the status is OK; untouched
 *            otherwise.
 *
 * Returned Value:
 *   How the descriptor was classified; see the enum for the meanings.
 *
 ****************************************************************************/

static enum rk3576_hdmi_dtd_status_e
rk3576_hdmi_edid_dtd(FAR const uint8_t *desc,
                     FAR struct rk3576_hdmi_timing_s *timing)
{
  uint32_t pixclk10k;
  uint32_t hactive;
  uint32_t hblank;
  uint32_t hsync_off;
  uint32_t hsync_wid;
  uint32_t vactive;
  uint32_t vblank;
  uint32_t vsync_off;
  uint32_t vsync_wid;
  uint8_t flags;

  pixclk10k = (uint32_t)desc[RK3576_HDMI_DTD_PIXCLOCK_LO] |
              ((uint32_t)desc[RK3576_HDMI_DTD_PIXCLOCK_HI] << 8);

  if (pixclk10k == 0)
    {
      return RK3576_HDMI_DTD_STATUS_NOT_TIMING;
    }

  hactive = (uint32_t)desc[RK3576_HDMI_DTD_HACTIVE_LO] |
            (((uint32_t)desc[RK3576_HDMI_DTD_H_MSBITS] &
              RK3576_HDMI_DTD_HACTIVE_MSB_MASK)
             << RK3576_HDMI_DTD_HACTIVE_MSB_SHIFT);
  hblank = (uint32_t)desc[RK3576_HDMI_DTD_HBLANK_LO] |
           (((uint32_t)desc[RK3576_HDMI_DTD_H_MSBITS] &
             RK3576_HDMI_DTD_HBLANK_MSB_MASK)
            << RK3576_HDMI_DTD_HBLANK_MSB_SHIFT);

  vactive = (uint32_t)desc[RK3576_HDMI_DTD_VACTIVE_LO] |
            (((uint32_t)desc[RK3576_HDMI_DTD_V_MSBITS] &
              RK3576_HDMI_DTD_VACTIVE_MSB_MASK)
             << RK3576_HDMI_DTD_VACTIVE_MSB_SHIFT);
  vblank = (uint32_t)desc[RK3576_HDMI_DTD_VBLANK_LO] |
           (((uint32_t)desc[RK3576_HDMI_DTD_V_MSBITS] &
             RK3576_HDMI_DTD_VBLANK_MSB_MASK)
            << RK3576_HDMI_DTD_VBLANK_MSB_SHIFT);

  hsync_off = (uint32_t)desc[RK3576_HDMI_DTD_HSYNC_OFF_LO] |
              (((uint32_t)desc[RK3576_HDMI_DTD_SYNC_MSBITS] &
                RK3576_HDMI_DTD_HSYNC_OFF_MSB_MASK)
               << RK3576_HDMI_DTD_HSYNC_OFF_MSB_SHIFT);
  hsync_wid = (uint32_t)desc[RK3576_HDMI_DTD_HSYNC_WID_LO] |
              (((uint32_t)desc[RK3576_HDMI_DTD_SYNC_MSBITS] &
                RK3576_HDMI_DTD_HSYNC_WID_MSB_MASK)
               << RK3576_HDMI_DTD_HSYNC_WID_MSB_SHIFT);

  vsync_off = ((uint32_t)desc[RK3576_HDMI_DTD_VSYNC_LO] >>
               RK3576_HDMI_DTD_VSYNC_OFF_LO_SHIFT) |
              (((uint32_t)desc[RK3576_HDMI_DTD_SYNC_MSBITS] &
                RK3576_HDMI_DTD_VSYNC_OFF_MSB_MASK)
               << RK3576_HDMI_DTD_VSYNC_OFF_MSB_SHIFT);
  vsync_wid = ((uint32_t)desc[RK3576_HDMI_DTD_VSYNC_LO] &
               RK3576_HDMI_DTD_VSYNC_WID_LO_MASK) |
              (((uint32_t)desc[RK3576_HDMI_DTD_SYNC_MSBITS] &
                RK3576_HDMI_DTD_VSYNC_WID_MSB_MASK)
               << RK3576_HDMI_DTD_VSYNC_WID_MSB_SHIFT);

  flags = desc[RK3576_HDMI_DTD_FLAGS];

  if ((flags & RK3576_HDMI_DTD_INTERLACED) != 0)
    {
      return RK3576_HDMI_DTD_INTERLACED;
    }

  if ((flags & RK3576_HDMI_DTD_SYNCTYPE_MASK) !=
      RK3576_HDMI_DTD_SYNCTYPE_SEPARATE)
    {
      return RK3576_HDMI_DTD_STATUS_SYNC_SCHEME;
    }

  /* Validate before any subtraction, so the porches below cannot underflow.
   * An EDID is untrusted input: it arrives over a cable and must not be able
   * to size a framebuffer allocation or wrap a total.
   */

  if (hactive == 0 || vactive == 0 || hactive > RK3576_HDMI_DTD_MAX_ACTIVE ||
      vactive > RK3576_HDMI_DTD_MAX_ACTIVE || hsync_wid == 0 ||
      vsync_wid == 0 || hblank <= hsync_off + hsync_wid ||
      vblank <= vsync_off + vsync_wid)
    {
      return RK3576_HDMI_DTD_STATUS_MALFORMED;
    }

  memset(timing, 0, sizeof(*timing));

  /* The descriptor carries the pixel clock in 10 kHz units. */

  timing->pixel_clock = pixclk10k * 10000u;

  timing->xres = (uint16_t)hactive;
  timing->yres = (uint16_t)vactive;

  timing->hfront_porch = (uint16_t)hsync_off;
  timing->hsync_len = (uint16_t)hsync_wid;
  timing->hback_porch = (uint16_t)(hblank - hsync_off - hsync_wid);

  timing->vfront_porch = (uint16_t)vsync_off;
  timing->vsync_len = (uint16_t)vsync_wid;
  timing->vback_porch = (uint16_t)(vblank - vsync_off - vsync_wid);

  timing->hsync_positive = (flags & RK3576_HDMI_DTD_HSYNC_POLARITY) != 0;
  timing->vsync_positive = (flags & RK3576_HDMI_DTD_VSYNC_POLARITY) != 0;

  return RK3576_HDMI_DTD_STATUS_OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_dtd_status_str
 *
 * Description:
 *   Name a DTD classification for the log.
 *
 ****************************************************************************/

static FAR const char *
rk3576_hdmi_dtd_status_str(enum rk3576_hdmi_dtd_status_e status)
{
  switch (status)
    {
      case RK3576_HDMI_DTD_STATUS_OK:
        return "usable";

      case RK3576_HDMI_DTD_STATUS_NOT_TIMING:
        return "not a timing";

      case RK3576_HDMI_DTD_STATUS_INTERLACED:
        return "interlaced";

      case RK3576_HDMI_DTD_STATUS_SYNC_SCHEME:
        return "sync is not digital separate";

      case RK3576_HDMI_DTD_STATUS_MALFORMED:
        return "malformed";

      default:
        return "unknown";
    }
}

/****************************************************************************
 * Name: rk3576_hdmi_edid_manufacturer
 *
 * Description:
 *   Decode the EDID's three-letter manufacturer ID.  The 16-bit field is three
 *   five-bit letters, most significant first, each biased from '@'.
 *
 * Input Parameters:
 *   edid - The base block.
 *   buf  - Receives 4 bytes: three letters plus a NUL.
 *
 ****************************************************************************/

static void rk3576_hdmi_edid_manufacturer(FAR const uint8_t *edid,
                                          FAR char *buf)
{
  uint16_t code = ((uint16_t)edid[RK3576_HDMI_EDID_MANUFACTURER] << 8) |
                  (uint16_t)edid[RK3576_HDMI_EDID_MANUFACTURER + 1u];

  buf[0] = (char)('@' + ((code >> 10) & 0x1fu));
  buf[1] = (char)('@' + ((code >> 5) & 0x1fu));
  buf[2] = (char)('@' + (code & 0x1fu));
  buf[3] = '\0';
}

/****************************************************************************
 * Name: rk3576_hdmi_edid_text
 *
 * Description:
 *   Copy one of a descriptor's ASCII fields (monitor name, serial number,
 *   free-form text) into a NUL-terminated buffer.
 *
 *   Two details make this more than a memcpy: the field is thirteen bytes
 *   padded with spaces and terminated by a newline rather than a NUL, so a
 *   straight %s would run straight on into the next descriptor; and it is
 *   remote input, so every non-printable byte is replaced rather than risk
 *   emitting terminal control sequences into the log.
 *
 * Input Parameters:
 *   desc   - The 18 descriptor bytes.
 *   buf    - Destination.
 *   buflen - Size of buf; at least RK3576_HDMI_DESC_TEXT_LEN + 1.
 *
 ****************************************************************************/

static void rk3576_hdmi_edid_text(FAR const uint8_t *desc, FAR char *buf,
                                  size_t buflen)
{
  size_t n = 0;
  size_t i;

  for (i = 0; i < RK3576_HDMI_DESC_TEXT_LEN && n + 1u < buflen; i++)
    {
      uint8_t c = desc[RK3576_HDMI_DESC_TEXT + i];

      if (c == '\n' || c == '\r')
        {
          break;
        }

      buf[n++] = (char)((c >= 0x20u && c < 0x7fu) ? (char)c : '.');
    }

  /* Trim the space padding the field is filled out with. */

  while (n > 0 && buf[n - 1] == ' ')
    {
      n--;
    }

  buf[n] = '\0';
}

/****************************************************************************
 * Name: rk3576_hdmi_edid_dump_descriptor
 *
 * Description:
 *   Log one of the four EDID descriptor slots: its detailed timing if it has
 *   one, otherwise whatever display descriptor it is.
 *
 *   Both cases are reported, and that includes the ones this driver cannot
 *use. A sink whose preferred timing is interlaced is a real and fairly common
 *   case, and "descriptor 1: interlaced" answers at a glance the question that
 *   would otherwise take a DDC trace to answer.
 *
 * Input Parameters:
 *   index - 1-based slot number, for the log.
 *   desc  - The 18 descriptor bytes.
 *
 ****************************************************************************/

static void rk3576_hdmi_edid_dump_descriptor(unsigned int index,
                                             FAR const uint8_t *desc)
{
  struct rk3576_hdmi_timing_s timing;
  enum rk3576_hdmi_dtd_status_e status;
  char text[RK3576_HDMI_DESC_TEXT_LEN + 1u];
  uint32_t htotal;
  uint32_t vtotal;

  status = rk3576_hdmi_edid_dtd(desc, &timing);
  if (status == RK3576_HDMI_DTD_STATUS_OK)
    {
      htotal = (uint32_t)timing.xres + timing.hfront_porch + timing.hsync_len +
               timing.hback_porch;
      vtotal = (uint32_t)timing.yres + timing.vfront_porch + timing.vsync_len +
               timing.vback_porch;

      _info("EDID descriptor %u: %ux%u @ %u Hz, pixel clock %" PRIu32
            " Hz, %u/%u/%u/%u h, %u/%u/%u/%u v, sync %c%c\n",
            index, (unsigned int)timing.xres, (unsigned int)timing.yres,
            (unsigned int)(timing.pixel_clock / (htotal * vtotal)),
            timing.pixel_clock, (unsigned int)timing.xres,
            (unsigned int)timing.hfront_porch, (unsigned int)timing.hsync_len,
            (unsigned int)timing.hback_porch, (unsigned int)timing.yres,
            (unsigned int)timing.vfront_porch, (unsigned int)timing.vsync_len,
            (unsigned int)timing.vback_porch,
            timing.hsync_positive ? '+' : '-',
            timing.vsync_positive ? '+' : '-');
      return;
    }

  if (status != RK3576_HDMI_DTD_STATUS_NOT_TIMING)
    {
      _info("EDID descriptor %u: timing not usable here (%s)\n", index,
            rk3576_hdmi_dtd_status_str(status));
      return;
    }

  switch (desc[RK3576_HDMI_DESC_TYPE])
    {
      case RK3576_HDMI_DESC_TYPE_NAME:
        rk3576_hdmi_edid_text(desc, text, sizeof(text));
        _info("EDID descriptor %u: monitor name \"%s\"\n", index, text);
        break;

      case RK3576_HDMI_DESC_TYPE_SERIAL:
        rk3576_hdmi_edid_text(desc, text, sizeof(text));
        _info("EDID descriptor %u: serial number \"%s\"\n", index, text);
        break;

      case RK3576_HDMI_DESC_TYPE_TEXT:
        rk3576_hdmi_edid_text(desc, text, sizeof(text));
        _info("EDID descriptor %u: text \"%s\"\n", index, text);
        break;

      case RK3576_HDMI_DESC_TYPE_LIMITS:
        /* The maximum pixel clock is in 10 MHz units, so 17 prints as 170 MHz.
         * Reporting it matters more than the refresh limits: it is the sink's
         * own statement of the fastest pixel clock it will accept, which is
         * exactly the number the board-level cap is compared against.
         */

        _info("EDID descriptor %u: range limits %u-%u Hz v, %u-%u kHz h, "
              "max pixel clock %" PRIu32 " MHz\n",
              index, (unsigned int)desc[RK3576_HDMI_DESC_LIMITS_MIN_VFREQ],
              (unsigned int)desc[RK3576_HDMI_DESC_LIMITS_MAX_VFREQ],
              (unsigned int)desc[RK3576_HDMI_DESC_LIMITS_MIN_HFREQ],
              (unsigned int)desc[RK3576_HDMI_DESC_LIMITS_MAX_HFREQ],
              (uint32_t)desc[RK3576_HDMI_DESC_LIMITS_MAX_CLOCK] *
                  RK3576_HDMI_DESC_LIMITS_MAX_CLOCK_UNIT / 1000000u);
        break;

      default:
        _info("EDID descriptor %u: display descriptor type 0x%02x (unused "
              "here)\n",
              index, (unsigned int)desc[RK3576_HDMI_DESC_TYPE]);
        break;
    }
}

/****************************************************************************
 * Name: rk3576_hdmi_edid_dump
 *
 * Description:
 *   Log the sink's EDID in decoded form: the identification fields and all
 *four descriptor slots.
 *
 *   Decoded rather than raw.  This used to also print the 128 bytes in hex, to
 *   preserve anything the decoder did not understand, but everything a sink
 *has said in this project so far fits the decoded form -- and eight lines of
 *hex have to be hand-decoded to be useful.  If a sink ever needs its
 *   chromaticity, established-timing or standard-timing sections read out,
 *dump just those bytes rather than restoring the whole block.
 *
 *   Every descriptor slot is reported, including the ones this driver cannot
 *   use: a sink whose preferred timing is interlaced is a real and fairly
 *   common case, and "descriptor 1: interlaced" answers at a glance what would
 *   otherwise take a DDC trace to answer.
 *
 * Input Parameters:
 *   edid - The 128-byte base block.
 *   hpd  - The hot-plug-detect level, reported because it is the one piece of
 *          context that explains a successful read next to a low HPD, or a
 *          failed one next to a low HPD.
 *
 ****************************************************************************/

static void rk3576_hdmi_edid_dump(FAR const uint8_t *edid, bool hpd)
{
  char text[RK3576_HDMI_DESC_TEXT_LEN + 1u];
  uint8_t input = edid[RK3576_HDMI_EDID_VIDEO_INPUT];
  uint8_t features = edid[RK3576_HDMI_EDID_FEATURES];
  uint8_t gamma = edid[RK3576_HDMI_EDID_GAMMA];
  uint16_t product = (uint16_t)edid[RK3576_HDMI_EDID_PRODUCTCODE] |
                     ((uint16_t)edid[RK3576_HDMI_EDID_PRODUCTCODE + 1u] << 8);
  uint32_t serial = (uint32_t)edid[RK3576_HDMI_EDID_SERIALNO] |
                    ((uint32_t)edid[RK3576_HDMI_EDID_SERIALNO + 1u] << 8) |
                    ((uint32_t)edid[RK3576_HDMI_EDID_SERIALNO + 2u] << 16) |
                    ((uint32_t)edid[RK3576_HDMI_EDID_SERIALNO + 3u] << 24);
  unsigned int i;

  rk3576_hdmi_edid_manufacturer(edid, text);

  _info("EDID: %s product 0x%04x, serial 0x%08" PRIx32
        ", week %u of %u, EDID v%u.%u, %u extension block(s), HPD %s\n",
        text, (unsigned int)product, serial,
        (unsigned int)edid[RK3576_HDMI_EDID_WEEK],
        (unsigned int)edid[RK3576_HDMI_EDID_YEAR] + 1990u,
        (unsigned int)edid[RK3576_HDMI_EDID_VERSION],
        (unsigned int)edid[RK3576_HDMI_EDID_REVISION],
        (unsigned int)edid[RK3576_HDMI_EDID_EXT_COUNT],
        hpd ? "asserted" : "NOT asserted");

  if ((input & RK3576_HDMI_EDID_INPUT_DIGITAL) != 0)
    {
      static FAR const char *const vidif[] = { "undefined", "HDMI-a",
                                               "HDMI-b",    "undefined",
                                               "MDDI",      "DisplayPort" };

      char digitalstr[64];
      char gammastr[20];
      unsigned int revision = edid[RK3576_HDMI_EDID_REVISION];
      unsigned int bits = (input & RK3576_HDMI_EDID_INPUT_BITDEPTH_MASK) >>
                          RK3576_HDMI_EDID_INPUT_BITDEPTH_SHIFT;
      unsigned int iface = input & RK3576_HDMI_EDID_INPUT_VIDIF_MASK;

      /* The video interface and bit depth fields are EDID 1.4 additions.  In a
       * 1.3 block those bits are reserved and read 0, so decoding them anyway
       * would report "undefined" and "depth unspecified" about a monitor that
       * merely predates the fields -- which reads like a defect rather than a
       * version number.  MEASURED, and the reason this check exists: the AOC
       * 22B2WG5 is v1.3 and showed exactly that.
       */

      if (revision < 4u)
        {
          snprintf(digitalstr, sizeof(digitalstr),
                   "interface and depth reserved in EDID v1.%u", revision);
        }
      else if (bits == RK3576_HDMI_EDID_INPUT_BITDEPTH_UNDEFINED)
        {
          snprintf(digitalstr, sizeof(digitalstr), "%s, depth unspecified",
                   iface < nitems(vidif) ? vidif[iface] : "unknown");
        }
      else
        {
          /* The field counts the depth in 2-bit steps starting at 6 bpc:
           * 1 -> 6, 2 -> 8, 3 -> 10, 4 -> 12, 5 -> 14, 6 -> 16.
           */

          snprintf(digitalstr, sizeof(digitalstr), "%s, %u bpc",
                   iface < nitems(vidif) ? vidif[iface] : "unknown",
                   4u + bits * 2u);
        }

      /* Gamma has its own "not stated" encoding: it is stored biased by 100,
       * so 0xff means "undefined" rather than 3.55.
       */

      if (gamma == 0xffu)
        {
          snprintf(gammastr, sizeof(gammastr), "gamma undefined");
        }
      else
        {
          snprintf(gammastr, sizeof(gammastr), "gamma %u.%02u",
                   ((unsigned int)gamma + 100u) / 100u,
                   ((unsigned int)gamma + 100u) % 100u);
        }

      _info("EDID: digital, %s, %s, %ux%u cm, features 0x%02x%s\n", digitalstr,
            gammastr, (unsigned int)edid[RK3576_HDMI_EDID_HSIZE_CM],
            (unsigned int)edid[RK3576_HDMI_EDID_VSIZE_CM],
            (unsigned int)features,
            (features & RK3576_HDMI_EDID_FEATURE_PREFERRED_TIMING) != 0
                ? ", preferred timing promised in descriptor 1"
                : ", no preferred timing promised");
    }
  else
    {
      _info("EDID: analog input (0x%02x); this driver only drives digital "
            "sinks\n",
            (unsigned int)input);
    }

  for (i = 0; i < RK3576_HDMI_EDID_DESC_NUMBER; i++)
    {
      rk3576_hdmi_edid_dump_descriptor(
          i + 1u,
          &edid[RK3576_HDMI_EDID_DESCRIPTOR + i * RK3576_HDMI_EDID_DESC_SIZE]);
    }
}

/****************************************************************************
 * Name: rk3576_hdmi_reset
 *
 * Description:
 *   Assert or release one CRU soft-reset line.
 *
 *   The reset-controller framework has no Rockchip CRU provider, so the line
 *   is pulsed through the raw register, exactly as rk3576_sai.c,
 *   rk3576_saradc.c and rk3576_vop.c do.  Polarity is uniform across the CRU:
 *   writing 1 asserts the reset, 0 releases it.
 *
 * Input Parameters:
 *   priv         - Driver state.
 *   base         - CRU block base (main CRU or PMU1CRU).
 *   con          - SOFTRST_CONn index within that block.
 *   bit          - Bit position of the reset line.
 *   assert_reset - true to assert (hold in reset), false to release.
 *
 ****************************************************************************/

static void rk3576_hdmi_reset(struct rk3576_hdmi_s *priv, uintptr_t base,
                              unsigned int con, unsigned int bit,
                              bool assert_reset)
{
  rk3576_hdmi_grf_write(base, RK3576_CRU_SOFTRST_CON(con), 1u << bit,
                        assert_reset ? (1u << bit) : 0u);
}

/****************************************************************************
 * Name: rk3576_hdmi_pulse_reset
 *
 * Description:
 *   Assert a soft-reset line, wait, then release it.  Used for the HPD
 *   detector, which has no dependencies and therefore needs no ordering
 *   relative to the rest of the init sequence.
 *
 ****************************************************************************/

static void rk3576_hdmi_pulse_reset(struct rk3576_hdmi_s *priv, uintptr_t base,
                                    unsigned int con, unsigned int bit)
{
  rk3576_hdmi_reset(priv, base, con, bit, true);
  up_udelay(RK3576_HDMI_RST_PULSE_US);
  rk3576_hdmi_reset(priv, base, con, bit, false);
}

/****************************************************************************
 * Name: rk3576_hdmi_routing
 *
 * Description:
 *   Program the VO0_GRF fields that connect the VOP to the HDMI controller.
 *
 *   These fields belong to the HDMI output path: they connect the VOP to the
 *   HDMI controller.  Our rk3576_vop.c is deliberately interface-agnostic --
 *   it only selects the SYS_CTRL_*_INFACE_CTRL register for the requested
 *   iface -- so the HDMI-specific routing has to be established here instead.
 *
 *   Fields written (see hardware/rk3576_hdmi.h for the full map):
 *     SOC_CON1  TMDS mode (no FRL), HDCP 1.4 SRAM left disabled
 *     SOC_CON8  IPI colour depth and format, which is how the controller
 *               learns what the VOP is sending, plus the link-symbol clock
 *               gate forced ON (grf_vo0_hdmi_gate = 0)
 *     SOC_CON9  the HDMI channel takes its pixels from the VOP, not from EDP
 *     SOC_CON13 ungate the dclk path into the HDMI transmitter
 *     SOC_CON14 hand the DDC pads to the controller's built-in I2C master and
 *               select the I2S audio source
 *
 *   The GPIO4C sideband pads (HPD/DDC/CEC) are NOT muxed -- see the note below
 *   on why that is a separate concern from the video path.
 *
 * Input Parameters:
 *   priv - Driver state.
 *   bpc  - Bits per colour component of the pixel stream.
 *
 ****************************************************************************/

static void rk3576_hdmi_routing(struct rk3576_hdmi_s *priv, uint8_t bpc)
{
  uint32_t depth;

  /* Colour depth.  Only two values are wired up in the hardware; anything
   * else falls back to 8 bpc, which is also what a DVI sink requires.
   */

  depth = (bpc == 10) ? RK3576_HDMI_GRF_BPC_10 : RK3576_HDMI_GRF_BPC_8;

  rk3576_hdmi_grf_write(
      priv->grf, RK3576_HDMI_GRF_SOC_CON8_OFF,
      RK3576_HDMI_GRF_COLOR_DEPTH_MASK | RK3576_HDMI_GRF_COLOR_FORMAT_MASK |
          RK3576_HDMI_GRF_HDMI_GATE,
      (depth << RK3576_HDMI_GRF_COLOR_DEPTH_SHIFT) |
          (RK3576_HDMI_GRF_FMT_RGB << RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT));

  /* The remaining fields are mode-independent, so they are set once, on the
   * first call.  SOC_CON9 is shared with the MIPI DSI path: whichever
   * consumer runs last wins, and the VOP/HDMI pair is the one that matters
   * here.
   */

  if (!priv->initialized)
    {
      /* TMDS, not FRL.  This is the reset value, so the write only serves to
       * undo the case where an earlier stage left the controller in FRL mode.
       * The rest of SOC_CON1 is HDCP/EMP scratch that the TRM requires to be
       * 0 in normal operation and that nothing here enables, so it is left
       * alone.
       */

      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON1_OFF,
                            RK3576_HDMI_GRF_FRLMOD, 0);

      /* SOC_CON9[9] MUST BE 0.
       *
       * This driver set it to 1 from its first version, on the strength of the
       * register description's "1'b1: Choose VOP path", and that is the wrong
       * reading of the field.  The TRM's own application note (Part 2
       * 12.6.3.4, "VOP_LITE -> HDMI TX Controller") says:
       *
       *     Step1: GRF_VO0_CON13[4] = 1  // grf_sw_hdmi_1to4 with hdmi mode
       *     Step2: GRF_VO0_CON09[9] = 1  // hdmi controller data source
       *                                  //   comes from EBC
       *
       * so bit 9 = 1 selects the EBC/VOP_LITE source specifically, not "the
       * VOP path" in general.  This board drives the controller from the
       * standard VOP2 path, so with bit 9 set the HDMI TX is told to take its
       * pixels from a source that is not running -- which is exactly a
       * permanently zero ipi_clk, and exactly what is measured here.  The
       * reset value of the bit is 0, and 0 is what this driver writes.
       */

      rk3576_hdmi_grf_write(
          priv->grf, RK3576_HDMI_GRF_SOC_CON9_OFF,
          RK3576_HDMI_GRF_CH_SEL_VOP | RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_MASK,
          0u | (depth == RK3576_HDMI_GRF_BPC_10
                    ? RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_10BPC
                    : RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_8BPC));

      /* SOC_CON13: [4] grf_sw_hdmi_1to4_en and [8] grf_ebc_dclk2hdmitx_disable
       * are both driven to 0, which is their reset value.
       *
       * Bit 4 enables the 1to4 module, which TRM Part 2 12.6.3.4 introduces
       * together with CON9[9] as the pair that routes the EBC/VOP_LITE source
       * into the HDMI TX.  With CON9[9] correctly cleared that module is not
       * on the path at all, so enabling it can only misroute the interface.
       * Its reset value is 0.
       */

      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON13_OFF,
                            RK3576_HDMI_GRF_DCLK2HDMITX_DISABLE |
                                RK3576_HDMI_GRF_HDMI_1TO4_EN,
                            0u);

      /* SOC_CON14 hands the DDC pads to the controller's built-in I2C master
       * and selects the I2S pin group.
       */

      rk3576_hdmi_grf_write(
          priv->grf, RK3576_HDMI_GRF_SOC_CON14_OFF,
          RK3576_HDMI_GRF_I2S_SEL | RK3576_HDMI_GRF_SCLIN_MASK |
              RK3576_HDMI_GRF_SDAIN_MASK | RK3576_HDMI_GRF_GRANT_SEL,
          RK3576_HDMI_GRF_I2S_SEL | RK3576_HDMI_GRF_SCLIN_MASK |
              RK3576_HDMI_GRF_SDAIN_MASK | RK3576_HDMI_GRF_GRANT_SEL);

      /* Mask the IOC's HPD interrupt.  The pin is not routed to an interrupt
       * handler yet, and an unmasked level-triggered source would keep the
       * GIC firing.
       *
       * Mind the polarity: the TRM defines MISC_CON0[2] as
       * "hdmitx_hpd_int_mask
       * -- 1'b0: Interrupt enable, 1'b1: Interrupt disable", i.e. inverted
       * with respect to its own name.  The mask is written as 1 (interrupt
       * disabled), the reset value.  It gates the IRQ output only; the HPD
       * status bits in HDMITX_HPD_STATUS are unaffected by it.
       */

      rk3576_hdmi_grf_write(priv->ioc, RK3576_HDMI_IOC_MISC_CON0_OFF,
                            RK3576_HDMI_IOC_HPD_INT_MSK,
                            RK3576_HDMI_IOC_HPD_INT_MSK);

      /* HPD debounce.  Without it the internal HPD level (status bit 3) is the
       * unfiltered pad and a single glitch is enough to look like a hot-plug.
       * It is written for correctness of the HPD input, not because it can
       * light a display.
       */

      putreg32(RK3576_HDMI_IOC_MISC_CON1_DEBOUNCE,
               priv->ioc + RK3576_HDMI_IOC_MISC_CON1_OFF);

      /* Mux the GPIO4C pads to their HDMI functions.
       *
       * The distinction that matters here: the TMDS clock and data lanes are
       * DEDICATED pins and pass through no IOMUX -- that is why a clean
       * 148.5 MHz waveform appears at the connector no matter what this
       * register says.  But the four SIDEBAND signals are on GPIO4C, which
       * resets to 0 = GPIO, so HPD / DDC / CEC are disconnected from the
       * controller until this write is made.
       *
       * MEASURED, and the reason this is here: with the write absent,
       * VCCIO6_IOC_HDMITX_HPD_STATUS reads 0x00000080 -- bit 7 only, with
       * bit 3 clear.  Bit 3 is the debounced HPD level
       * (RK3576_HDMI_LEVEL_INT), so 0x80 decodes as "no sink attached" even
       * with a monitor plugged in and terminating the link.  The HPD input is
       * simply not connected to its pad.  Restoring this write makes that
       * reading meaningful.
       */

      rk3576_hdmi_grf_write(priv->ioc, RK3576_HDMI_IOC_GPIO4C_IOMUX_SEL_L_OFF,
                            RK3576_HDMI_IOC_GPIO4C_SEL_MASK,
                            RK3576_HDMI_IOC_GPIO4C_HDMI_SEL);
    }
}

/****************************************************************************
 * Name: rk3576_hdmi_init_registers
 *
 * Description:
 *   Initialise the controller's register file, its embedded I2C (DDC)
 *   master, and the timer reference.
 *
 *   Notable omissions, all deliberate:
 *     - GLOBAL_SWDISABLE is not touched.  The video datapath is enabled out
 *       of reset; the bits that would have to be set there are the CEC and
 *       audio-packet datapaths, neither of which we use.
 *     - GLOBAL_SWRESET_REQUEST is not touched either.  It resets units *and
 *       their configuration registers*, so using it here would immediately
 *       undo the writes below.  The controller was already reset by the CRU
 *       lines before this function ran.
 *     - VIDEO_INTERFACE_CONFIG0 is not touched: the bus format reaches the
 *       controller as hardware signals from VO0_GRF_SOC_CON8.
 *
 * Returned Value:
 *   Zero (OK) on success; -ETIMEDOUT if the controller never reports its
 *   clock tree running.
 *
 ****************************************************************************/

static int rk3576_hdmi_init_registers(struct rk3576_hdmi_s *priv)
{
  int i;

  /* Mask every interrupt.  The controller has no interrupt consumer yet and
   * an unmasked APB_REGBANK_READY would fire on every register write.
   */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_MAINUNIT_0_INT_MASK_N, 0);
  rk3576_hdmi_putreg(priv, RK3576_HDMI_MAINUNIT_1_INT_MASK_N, 0);

  /* Tell the controller's internal timers how fast the reference clock is.
   * The field is 29 bits wide and rejects anything larger, so clamp rather
   * than silently truncating.
   */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_TIMER_BASE_CONFIG0,
                     priv->ref_hz & RK3576_HDMI_TIMER_REFERENCE_BASE_MASK);

  /* Park the embedded I2C (DDC) master.  It is not used for anything in DVI
   * mode by itself, but left in an undefined state it can drive the DDC pads,
   * and leaving it configured here is what makes the EDID read that follows
   * board bring-up a plain transfer rather than another setup step.
   */

  rk3576_hdmi_ddc_init(priv);

  /* Acknowledge the I2C master's two interrupt sources. */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                     RK3576_HDMI_MAINUNIT_1_INT_CLEAR_VAL);

  /* Wait for the timer base to be accepted, then read the CMU back.
   *
   * TIMER_BASE_STATUS0[0] is the documented "the value was latched" flag and
   * is the cheapest proof that the reference clock is actually toggling -- the
   * field can only be accepted if irefclk is running, because the controller's
   * timers count it.
   *
   * This matters because the controller's register file is NOT uniformly
   * clocked.  Everything below 0x0400 (reset manager, CMU, I2CM, SCDC) sits in
   * the always-on APB domain and answers even with no reference clock at all,
   * while the blocks above it are clocked by the CMU's vidqp/linkqp domains,
   * which are generated FROM the reference clock.  With no reference clock
   * those domains are dead and an access is not decoded as a register access:
   * the slave returns an error and the CPU takes a synchronous external abort
   * rather than reading garbage.
   *
   * MEASURED, and the reason this check is here rather than a comment: the
   * very first access above 0x0400 -- HDCP2LOGIC_CONFIG0, 0x08e0, from
   * rk3576_hdmi_enable() -- aborted exactly this way, while every access below
   * 0x0400 in the same controller had already succeeded.
   *
   * NOTE: that abort has since turned out NOT to be a general property of this
   * region.  With the HDCP2 access removed the same binary reads and writes
   * 0x0968 (LINK_CONFIG0) and 0x3014/0x3024/0x3028 (main-unit interrupts)
   * without complaint -- so the region is live, and 0x08e0 fails for a reason
   * specific to the HDCP block.  See the long note on the HDCP2 access in
   * rk3576_hdmi_enable().  This probe is still worth keeping: it is cheap, it
   * is the documented precondition for the upper block, and it prints
   * CMU_STATUS for free.
   */

  for (i = 0; i < RK3576_HDMI_BANK_READY_TRIES; i++)
    {
      if (rk3576_hdmi_getreg(priv, RK3576_HDMI_TIMER_BASE_STATUS0) &
          RK3576_HDMI_TIMER_BASE_LOCKED_ST)
        {
          /* Deliberately silent on the success path: the timer base latching
           * is an internal precondition that has never failed on this board,
           * and a line saying so every boot is noise.  The failure path below
           * still reports it, which is the case worth reading about.  The
           * controller's identity (CORE_ID, clocks) is logged by the caller
           * instead.
           */

          return OK;
        }

      up_udelay(RK3576_HDMI_BANK_READY_DELAY);
    }

  /* Deliberately NOT fatal.  Two reasons:
   *
   *   - It cannot be relied on as a hard gate: failing here would block a
   *     display that might otherwise work.
   *
   *   - This runs from rk3576_hdmi_initialize(), i.e. BEFORE the PHY is
   *     powered on.  If the vidqp/linkqp domains are fed from the PHY -- which
   *     the required enable ordering implies, see rk3576_hdmi_enable() --
   *     then a "not locked" result here is the expected state rather than a
   *     fault, and CMU_STATUS will look un-locked for the same reason.  Judge
   *     the reference clock by priv->ref_hz (a rate of 0 is the real failure)
   *     and re-check CMU_STATUS after the PHY is up.
   */

  _warn("WARNING: RK3576 HDMI timer base never locked after %d tries "
        "(TIMER_BASE_STATUS0=%08" PRIx32 ", CMU_STATUS=%08" PRIx32
        ", ref %" PRIu32
        " Hz).  The controller's internal vidqp/linkqp clock domains are "
        "derived from the reference clock, so the register block above "
        "0x0400 may not answer and the next access to it can abort on the "
        "bus.\n",
        RK3576_HDMI_BANK_READY_TRIES,
        rk3576_hdmi_getreg(priv, RK3576_HDMI_TIMER_BASE_STATUS0),
        rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_STATUS), priv->ref_hz);

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_clock_get
 *
 * Description:
 *   Resolve one of the controller's clocks and enable it.
 *
 *   hdp is optional: it only samples the hot-plug pin, which nothing consumes
 *   yet, so a board whose clock tree omits it should still be able to
 *   transmit.
 *
 * Input Parameters:
 *   name     - Clock name as registered with the CLK framework.
 *   optional - true if the clock may be absent.
 *
 * Returned Value:
 *   The clock handle on success, NULL if absent and optional; a negated
 *   errno value cast to a pointer otherwise.
 *
 ****************************************************************************/

static FAR struct clk_s *rk3576_hdmi_clock_get(FAR const char *name,
                                               bool optional)
{
  FAR struct clk_s *clk;
  int ret;

  clk = clk_get(name);
  if (clk == NULL)
    {
      if (optional)
        {
          _warn("WARNING: RK3576 HDMI optional clock '%s' is absent\n", name);
          return NULL;
        }

      _err("ERROR: RK3576 HDMI clock '%s' is not registered\n", name);
      return (FAR struct clk_s *)(uintptr_t)-ENOENT;
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      _err("ERROR: RK3576 HDMI failed to enable clock '%s': %d\n", name, ret);
      return (FAR struct clk_s *)(uintptr_t)ret;
    }

  return clk;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdmi_initialize
 *
 * Description:
 *   Bring the HDMI TX controller up to the point where it can accept a mode,
 *   and put the shared HDPTX PHY into its powered-off state.
 *
 *   Sequence:
 *     1. enable pclk / ref / hdp clocks
 *     2. release the CRU APB and reference resets, pulse the HPD reset
 *     3. probe the controller by reading its ID registers
 *     4. program VO0_GRF routing and the controller's register file
 *     5. bring the PHY's APB and resets up (rk3576_hdptxphy_initialize)
 *
 *   Step 5 is last on purpose: the PHY touches PMU1CRU reset lines that are
 *   independent of ours, but it also reads its own SFRs, and doing it after
 *   the routing fields are in place keeps the whole path in a
 *   "clocks on, output off" state if it fails.
 *
 ****************************************************************************/

int rk3576_hdmi_initialize(void)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  FAR struct clk_s *clk;
  uint32_t id;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  /* Step 1: clocks.  pclk_hdmitx0 is the APB clock and must be running
   * before any register access; clk_hdmitx0_ref feeds the internal timers;
   * clk_hdmitxhpd samples the hot-plug pin.
   */

  clk = rk3576_hdmi_clock_get("pclk_hdmitx0", false);
  if (clk == NULL || (uintptr_t)clk > (uintptr_t)-4096)
    {
      ret = (clk == NULL) ? -EIO : (int)(intptr_t)clk;
      goto err_unlock;
    }

  priv->pclk = clk;

  clk = rk3576_hdmi_clock_get("clk_hdmitx0_ref", false);
  if (clk == NULL || (uintptr_t)clk > (uintptr_t)-4096)
    {
      ret = (clk == NULL) ? -EIO : (int)(intptr_t)clk;
      goto err_pclk;
    }

  priv->ref = clk;

  /* Force the VO0 root mux onto clk_gpll before reading the reference rate.
   *
   * aclk_vo0_root_div is what feeds clk_hdmitx0_ref, and the reference clock
   * is what the controller's timers AND its internal CMU domains derive from
   * -- see rk3576_hdmi_init_registers() for why that decides whether the
   * 0x0800+ register block answers at all.
   *
   * The mux resets to 2'b00 = clk_gpll and the divider to /3 = 396 MHz, but an
   * earlier stage is free to have moved either, and the other two inputs are
   * exactly the orphans the VOP driver already has to work around:
   * clk_lpll is reparented by the CPU-frequency helper, and clk_bpll is not
   * registered at all.  A mux left on either leaves the controller with a
   * reference rate that reads back fine and no clock on the wire -- the
   * failure mode this file now reports explicitly.
   *
   * clk_set_parent() is the right call here and cannot be done implicitly:
   * the mux is registered with CLK_MUX_SET_RATE_NO_REPARENT, so only an
   * explicit reparent can move it.
   */

  clk = clk_get("aclk_vo0_root_sel");
  if (clk != NULL)
    {
      FAR struct clk_s *gpll = clk_get("clk_gpll");

      if (gpll != NULL)
        {
          ret = clk_set_parent(clk, gpll);
          if (ret < 0)
            {
              _err("ERROR: RK3576 HDMI failed to put aclk_vo0_root on gpll: "
                   "%d\n",
                   ret);
            }
        }
    }

  /* The VO0 high-speed peripheral clock.
   *
   * the controller's register file is NOT clocked by the APB gate alone: the
   * 0x0000-0x03ff block answers from pclk_hdmitx0, but the block above it --
   * video interface, HDCP, scrambler, packet scheduler and LINK_CONFIG0 --
   * needs this one as well.
   *
   * MEASURED: with only pclk_hdmitx0 and clk_hdmitx0_ref enabled, and after
   * every register below 0x0400 had been read and written successfully,
   * reading HDCP2LOGIC_CONFIG0 (0x08e0) aborted on the bus.
   *
   * That observation is what put this clock here, on the theory that the upper
   * block was unclocked.  The theory is now REFUTED: with the HDCP2 access
   * removed the driver reads and writes 0x0968 and 0x3014/0x3024/0x3028
   * happily, and 0x08e0 still aborts even now that this gate is enabled.  The
   * failure is specific to the HDCP block, not to the region.
   *
   * This clock is kept because it is listed among the HDMI node's required
   * clocks, so enabling it is correct regardless -- but it is not,
   * and never was, the cure for the 0x08e0 abort.  The gate resets enabled, so
   * enabling it only matters if it was gated before this driver ran.
   */

  (void)rk3576_hdmi_clock_get("hclk_vo0_root", true);

  /* The ARC clock.  It is part of the controller's clock set, so it is enabled
   * here and left enabled for the life of the driver rather than tracked and
   * torn down with the others.
   */

  (void)rk3576_hdmi_clock_get("clk_hdmitx0_arc", true);

  /* The audio clock, which the clock tree names "aud" and wires to
   * MCLK_SAI6_8CH.  It had been left off on the reasoning that DVI mode
   * carries no audio, and that reasoning is wrong for a structural reason:
   * the reset manager releases each functional group PER CLOCK DOMAIN, and
   * RESET_MANAGER_STATUS0 carries a dedicated
   * `AVP_DATAPATH_VIDEO_AUDCLK_STATUS` alongside the ipi/vidqp/linkqp/refclk
   * ones.  The VIDEO group therefore cannot come out of reset while audclk is
   * dead -- it is one of the five domains that group is gated on, audio or
   * not.
   *
   * MEASURED, which is what pointed here: RESET_MANAGER_STATUS0 reads
   * 0x00000000 (every group still ASSERTED) while CMU_STATUS reports the audio
   * domain as gated OFF and CMU_AUDQPCLK_FREQ measures 0 Hz.  Forcing the
   * manager's per-domain override to CLK_ON moved RESET_MANAGER_STATUS2 from
   * 0x0c to 0x1f but left STATUS0 at 0 -- consistent with the release having
   * to propagate into each domain on that domain's own clock, which cannot
   * happen if one of those clocks does not exist.
   *
   * The reference clock absent from the set is therefore a hard blocker:
   * enabling every clock in the controller's clock set, audclk included, is
   * required rather than optional.
   */

  (void)rk3576_hdmi_clock_get("mclk_sai6", true);

  priv->ref_hz = clk_get_rate(priv->ref);

  if (priv->ref_hz == 0)
    {
      _err("ERROR: RK3576 HDMI reference clock reports rate 0\n");
      ret = -EIO;
      goto err_ref;
    }

  priv->hdp = rk3576_hdmi_clock_get("clk_hdmitxhpd", true);
  if (priv->hdp != NULL && (uintptr_t)priv->hdp > (uintptr_t)-4096)
    {
      priv->hdp = NULL;
    }

  /* Step 2: CRU resets.  Assert both lines, hold them long enough for the
   * logic to settle, then release.  Presetn (APB) must be released for the
   * controller's SFRs to answer at all; resetn (reference) gates the timers.
   */

  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_APB_BIT, true);
  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_REF_BIT, true);
  rk3576_hdmi_pulse_reset(priv, priv->pmu1, RK3576_HDMI_PMU1RST_CON,
                          RK3576_HDMI_PMU1RST_HPD_BIT);
  up_udelay(RK3576_HDMI_RST_PULSE_US);

  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_APB_BIT, false);
  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_REF_BIT, false);
  up_udelay(RK3576_HDMI_RESET_SETTLE_US);

  /* Step 3: probe.  CORE_ID is read-only with a non-zero reset value, so a
   * zero read means the APB clock or the reset line is still wrong -- worth
   * failing on, because every subsequent write would silently be lost.
   */

  id = rk3576_hdmi_getreg(priv, RK3576_HDMI_CORE_ID);
  if (id == 0)
    {
      _err("ERROR: RK3576 HDMI core id is 0; controller is not "
           "responding\n");
      ret = -ENODEV;
      goto err_hdp;
    }

  _info("RK3576 HDMI controller: core id %08" PRIx32 ", ver %08" PRIx32
        ", config %08" PRIx32 ", ref %" PRIu32 " Hz\n",
        id, rk3576_hdmi_getreg(priv, RK3576_HDMI_VER_NUMBER),
        rk3576_hdmi_getreg(priv, RK3576_HDMI_CONFIG_REG), priv->ref_hz);

  /* Step 4: routing and register file.  The routing depends on bpc, so the
   * one-time fields are programmed inside rk3576_hdmi_routing() guarded by
   * the initialized flag, which is not yet set -- hence the explicit call
   * here with the DVI default of 8 bpc.
   */

  rk3576_hdmi_routing(priv, 8);

  ret = rk3576_hdmi_init_registers(priv);
  if (ret < 0)
    {
      goto err_hdp;
    }

  /* Step 5: the PHY.  It is shared with the eDP controller and owns the
   * pixel-clock PLL, so it must be brought up before the VOP is asked to
   * consume that clock.
   */

  ret = rk3576_hdptxphy_initialize();
  if (ret < 0)
    {
      _err("ERROR: RK3576 HDPTX PHY initialisation failed: %d\n", ret);
      goto err_hdp;
    }

  priv->initialized = true;
  nxmutex_unlock(&priv->lock);

  return OK;

err_hdp:
  if (priv->hdp != NULL)
    {
      clk_disable(priv->hdp);
      priv->hdp = NULL;
    }

err_ref:
  clk_disable(priv->ref);
  priv->ref = NULL;

err_pclk:
  clk_disable(priv->pclk);
  priv->pclk = NULL;

err_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_hdmi_enable
 *
 * Description:
 *   Start outputting a DVI (TMDS) video stream at the requested pixel rate.
 *
 *   Order matters: bring the PHY up first, then select the link mode.
 *   Programming
 *   LINK_CONFIG0 before the PHY is live would have the controller sampling a
 *   dead pixel clock.
 *
 ****************************************************************************/

int rk3576_hdmi_enable(uint32_t pixel_clock_hz, uint8_t bpc)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  int ret;

  /* Only the two colour depths the hardware can encode are accepted.  DVI
   * sinks are always 8 bpc, so 10 bpc is only reachable on an HDMI sink,
   * which this driver does not support yet -- but the plumbing is here so
   * that adding HDMI mode later does not need an API change.
   */

  if (bpc != 8 && bpc != 10)
    {
      _err("ERROR: RK3576 HDMI unsupported colour depth %u bpc\n", bpc);
      return -EINVAL;
    }

  if (pixel_clock_hz == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      _err("ERROR: RK3576 HDMI enable before initialize\n");
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  /* Update the IPI colour depth first: the controller latches it together
   * with the first pixels, so it must be correct before the PHY starts
   * producing the pixel clock.
   */

  rk3576_hdmi_routing(priv, bpc);

  /* Bring the PHY up FIRST -- before touching anything above 0x0400 in the
   * controller.  The reason is that the controller's upper register block does
   * not answer until the PHY is live (see the long note in
   * rk3576_hdmi_init_registers()), and the link mode must not be selected
   * while the controller would be sampling a dead pixel clock.
   *
   * The order here used to be HDCP2 -> PHY -> LINK_CONFIG, and the HDCP2
   * access aborted.  0x08e0 is simply the lowest address above 0x0400, so it
   * was the first one reached while that block was still unclocked -- which
   * makes this a one-line ordering bug, not a fault in the HDCP2 register.
   *
   * power_on() takes the *pixel* clock and the colour depth; the TMDS
   * character rate (pixel_clock * bpc / 8) is derived internally.
   */

  ret = rk3576_hdptxphy_power_on(pixel_clock_hz, bpc);
  if (ret < 0)
    {
      _err("ERROR: RK3576 HDPTX PHY power on failed at %" PRIu32
           " Hz / %u bpc: %d\n",
           pixel_clock_hz, bpc, ret);
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  /* HDCP2_BYPASS is NOT written here, and the reason is timing rather than
   * permissions -- an earlier version of this comment got that wrong and
   * concluded the register was unreachable, so the correction is recorded.
   *
   * HDCP2LOGIC_CONFIG0 (0x08e0) sits in the 0x0800-0x0aff block, which is
   * clock-gated until the CMU's video clock domains come up, i.e. until the
   * VOP is actually delivering an interface clock.  At this point in the
   * sequence the PHY is live but the VOP is not scanning yet, so CLK_LOCKED is
   * still false and the block does not answer.  MEASURED, twice:
   *
   *   1. modifyreg (read-modify-write) before power_on(): abort on the read.
   *   2. A plain store after power_on(): the store was accepted, and the NEXT
   *      controller access then aborted -- the panic dumped x4 = 0x0968 and
   *      x5 = 0x27da0000, i.e. the LINK_CONFIG0 write immediately below,
   *      already computing its address.
   *
   * Attempt 2 is what made the register look permanently dead, because the two
   * PKTSCHED writes in that same region had just succeeded.  They succeeded
   * because rk3576_hdmi_start_video_path() issues them after clk_locked turns
   * true; every
   * 0x08e0 attempt was made here, before it.  Same block, same access width,
   * opposite outcome -- the variable was never the register.
   *
   * So HDCP2_BYPASS is written from rk3576_hdmi_start_video_path(), in the
   * clk_locked branch, immediately ahead of the packet scheduler.  The order
   * there is bypass first, then packets.
   */

  /* Select DVI mode: no infoframes, no audio, no HDCP, no scrambling.  FRL
   * must be explicitly cleared -- it is a separate field, not the complement
   * of OPMODE_DVI.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0,
                        RK3576_HDMI_OPMODE_FRL | RK3576_HDMI_OPMODE_FRL_4LANES,
                        RK3576_HDMI_OPMODE_DVI);

  /* Take the video datapath out of software disable.  0x0044 is in the
   * always-on block, so a read-modify-write is safe here.
   *
   * The packet scheduler is deliberately NOT programmed here.  It lives in
   * the 0x0800-0x0aff block, which is clock-gated until the VOP delivers an
   * interface clock -- and MEASURED, touching that block while it is gated
   * is what produces the synchronous external aborts: writing 0x0aac and
   * 0x0aa8 is accepted, but the very next controller access then aborts,
   * and reading 0x0aa8 aborts outright.  rk3576_hdmi_start_video_path()
   * programs the scheduler once CMU_STATUS proves the clocks are up.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_GLOBAL_SWDISABLE,
                        RK3576_HDMI_AVP_DATAPATH_VIDEO_SWDISABLE, 0);

  priv->pixel_clock = pixel_clock_hz;
  priv->bpc = bpc;
  priv->streaming = true;

  /* CMU_STATUS is read here because it is the only status register that may be
   * read at this point, and it is reported rather than checked.  MEASURED:
   * 0x0000024a here, which fails the (CMU_STATUS & 0x15) == 0x15 readiness
   * test
   * -- and that is the EXPECTED reading, not a fault: ipi_clk and vidqpclk are
   * derived from the interface clock the VOP supplies, and the VOP has not
   * been started yet.  It is logged for comparison with the post-VOP reading
   * in rk3576_hdmi_start_video_path(), which is the one that says whether the
   * video path actually came up.
   *
   * Nothing in the 0x0800-0x0aff block is touched here: MEASURED, programming
   * it while it is still clock-gated accepts the write and then breaks the
   * NEXT controller access.  HDCP2_BYPASS and the packet scheduler both wait
   * for clk_locked, in rk3576_hdmi_start_video_path().
   */

  _info("RK3576 HDMI DVI mode enabled: %" PRIu32 " Hz, %u bpc\n",
        pixel_clock_hz, bpc);

  nxmutex_unlock(&priv->lock);

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_start_video_path
 *
 * Description:
 *   Program the parts of the controller that only become reachable once the
 *   interface clock is running, and start the packet scheduler.
 *
 *   WHY THIS IS A SEPARATE CALL, AND WHY IT COMES AFTER THE VOP.  Everything
 *   in the 0x0800-0x0aff block is clock-gated until the CMU's video clock
 *   domains come up, and those are derived from the interface clock the VOP
 *   delivers -- so this cannot be folded into rk3576_hdmi_enable(), which
 *   runs while the PHY is the only thing alive.  MEASURED, touching that
 *   block while it is gated is not merely useless: the write is accepted and
 *   the NEXT controller access then takes a synchronous external abort.
 *   Writing 0x0aac and 0x08e0 did exactly that, and reading 0x0aa8 or 0x0814
 *   aborts outright.  The block reads are consequently absent by design, and
 *   rk3576_hdmi_enable() documents the same restriction.
 *
 *   Call this once the VOP is scanning.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) on success; -EIO if no stream has been enabled.
 *
 ****************************************************************************/

int rk3576_hdmi_start_video_path(void)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  uint32_t cmu;
  bool clk_locked;

  if (!priv->streaming)
    {
      _err("ERROR: RK3576 HDMI start before enable\n");
      return -EIO;
    }

  /* The documented readiness test, (CMU_STATUS & 0x15) == 0x15, is true once
   * ipi_clk, vidqpclk and linkqpclk are all up.  While it
   * fails, the block programmed below must not be touched.
   */

  cmu = rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_STATUS);
  clk_locked =
      (cmu & RK3576_HDMI_CMU_CTRL_CLK_EN) == RK3576_HDMI_CMU_CTRL_CLK_EN;

  /* Re-apply the mode programming now that the interface clock is live.
   *
   * TRM Part 2 24.6.1 orders the bring-up as: enable all clocks -> configure
   * the PHY -> reset the controller -> configure link/packets/video.  This
   * driver cannot follow that literally, because the pixel clock is produced
   * by the PHY and only reaches the controller once the VOP is scanning, so
   * rk3576_hdmi_initialize() necessarily runs before any pixel clock exists.
   * Re-issuing the two writes here costs nothing -- VO0_GRF and the link
   * register are both in blocks that are already up -- and removes that
   * ordering as a variable.
   */

  rk3576_hdmi_routing(priv, priv->bpc);

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0,
                        RK3576_HDMI_OPMODE_FRL | RK3576_HDMI_OPMODE_FRL_4LANES,
                        RK3576_HDMI_OPMODE_DVI);

  /* Everything in 0x0800-0x0aff is clock-gated until the CMU's video clock
   * domains are up, and MEASURED, programming it while gated is accepted by
   * the bus but breaks the NEXT controller access -- writing 0x0aac and 0x08e0
   * and reading 0x0aa8 have each done it.  So this whole block is programmed
   * here, and only once the clocks have provably come up.
   */

  if (clk_locked)
    {
      /* HDCP2_BYPASS first: of everything in this block it is the one write
       * that changes what leaves the connector rather than how it is framed,
       * and it is the current best explanation for the black picture.
       *
       * TRM Part 2, HDCP2LOGIC_CONFIG0 (0x08e0) bit0:
       *
       *   0x0 (BYPASS_OFF): The video datapath goes through the External
       *                     HDCP2 Module.        <-- THIS IS THE RESET VALUE
       *   0x1 (BYPASS_ON):  The video datapath bypasses the External HDCP2
       *                     Module.
       *
       * So out of reset the pixel stream is routed THROUGH a cipher block that
       * nothing on this board brings up.  Timing is generated before that
       * point, so the sink still locks, counts a full frame and shows black --
       * exactly the symptom.  The bit is therefore set here, in DVI mode as
       * well, ahead of LINK_CONFIG0 and the packet scheduler.
       *
       * Plain store, never a read-modify-write: reads of this register have
       * taken a synchronous external abort twice on this board.  Writing the
       * whole word also clears the four override fields at bits [4:1], and 0
       * is what they must be -- an override ENABLE of 0 leaves that indication
       * "defined internally", which is the normal state.
       */

      putreg32(RK3576_HDMI_HDCP2_BYPASS,
               priv->base + RK3576_HDMI_HDCP2LOGIC_CONFIG0);

      putreg32(RK3576_HDMI_PKTSCHED_PKT_CONTROL0_START,
               priv->base + RK3576_HDMI_PKTSCHED_PKT_CONTROL0);
      putreg32(RK3576_HDMI_PKTSCHED_GCP_TX_EN,
               priv->base + RK3576_HDMI_PKTSCHED_PKT_EN);

      _info("RK3576 HDMI video path started: HDCP2 bypassed, packet scheduler "
            "running (CMU_STATUS %08" PRIx32 ", all display clock domains "
            "up)\n",
            cmu);
    }
  else
    {
      _warn(
          "WARNING: RK3576 HDMI: the video clock domains are still not up "
          "(CMU_STATUS %08" PRIx32 ", expected 0x15 bits): HDCP2 bypass and "
          "packet scheduler left unprogrammed, because that register block is "
          "clock-gated and touching it has been measured to break the next "
          "controller access\n",
          cmu);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_disable
 *
 * Description:
 *   Stop the video stream and park the PHY.  The controller's register file
 *   is left configured so that a later enable() is enough to restart.
 *
 ****************************************************************************/

int rk3576_hdmi_disable(void)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      /* Back to DVI idle.  Nothing else needs undoing: the infoframe
       * scheduler was never enabled, so there is no packet state to clear.
       */

      rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0, 0,
                            RK3576_HDMI_OPMODE_DVI);
      rk3576_hdptxphy_power_off();
    }

  priv->streaming = false;
  priv->pixel_clock = 0;
  priv->bpc = 0;

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_is_ready
 *
 * Description:
 *   Report whether the HDMI transmit path is producing a stream.
 *
 *   The condition is "a mode was enabled and the PHY reports locked + ready".
 *   That is deliberately the strictest statement the driver can honestly
 *   make: whether a picture appears also depends on the VOP actually driving
 *   the IPI and on the sink accepting the signal, neither of which the
 *   controller can report.
 *
 *   The PHY's lock and ready flags are also mirrored into VO0_GRF_SOC_ST3,
 *   so this can be cross-checked from outside the driver.
 *
 ****************************************************************************/

bool rk3576_hdmi_is_ready(void)
{
  return g_rk3576_hdmi.streaming && rk3576_hdptxphy_is_ready();
}

/****************************************************************************
 * Name: rk3576_hdmi_pixel_clock_hz
 *
 * Description:
 *   Return the pixel clock of the enabled mode, or 0.
 *
 ****************************************************************************/

uint32_t rk3576_hdmi_pixel_clock_hz(void) { return g_rk3576_hdmi.pixel_clock; }

/****************************************************************************
 * Name: rk3576_hdmi_ref_clock_hz
 *
 * Description:
 *   Return the controller's reference clock frequency, or 0.
 *
 ****************************************************************************/

uint32_t rk3576_hdmi_ref_clock_hz(void) { return g_rk3576_hdmi.ref_hz; }

/****************************************************************************
 * Name: rk3576_hdmi_hpd_connected
 *
 * Description:
 *   Report whether the sink is asserting hot-plug detect.  See the header for
 *   why this is advisory rather than a gate on the EDID read.
 *
 ****************************************************************************/

bool rk3576_hdmi_hpd_connected(void)
{
  return (getreg32(g_rk3576_hdmi.ioc + RK3576_HDMI_IOC_HDMI_HPD_STATUS_OFF) &
          RK3576_HDMI_IOC_HPD_LEVEL) != 0;
}

/****************************************************************************
 * Name: rk3576_hdmi_read_sink_timing
 *
 * Description:
 *   Read the sink's EDID base block over DDC, log it in full, and decode the
 *   operating timing from it.  See the header for the contract and the
 *   deliberate refusal to gate on HPD.
 *
 ****************************************************************************/

int rk3576_hdmi_read_sink_timing(FAR struct rk3576_hdmi_timing_s *timing)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  FAR const uint8_t *desc;
  struct rk3576_hdmi_timing_s candidate;
  enum rk3576_hdmi_dtd_status_e status;
  uint8_t edid[RK3576_HDMI_EDID_LENGTH];
  bool hpd;
  int unusable = -ENOENT;
  unsigned int i;
  int ret;

  if (timing == NULL)
    {
      return -EINVAL;
    }

  if (!priv->initialized)
    {
      _err("ERROR: RK3576 HDMI EDID read before initialize\n");
      return -EIO;
    }

  hpd = rk3576_hdmi_hpd_connected();

  ret = rk3576_hdmi_ddc_read(priv, edid, sizeof(edid));
  if (ret < 0)
    {
      _warn("WARNING: RK3576 HDMI EDID read failed: %d (HPD %s)\n", ret,
            hpd ? "asserted" : "NOT asserted");
      return ret;
    }

  if (!rk3576_hdmi_edid_valid(edid))
    {
      _warn("WARNING: RK3576 HDMI EDID header or checksum is invalid\n");
      return -EINVAL;
    }

  rk3576_hdmi_edid_dump(edid, hpd);

  /* Take the FIRST usable detailed timing, scanning all four descriptor slots.
   *
   * Two reasons for the scan rather than reading slot 1 alone:
   *
   *   - EDID 1.3+ requires the preferred timing to be in descriptor 1, and
   * byte 24 bit 1 is the sink's promise that it is.  Cheap EDID 1.0/1.2 sinks
   *     predate that rule and put the monitor name there instead, leaving the
   *     real timing in a later slot.  Reading only slot 1 makes those sinks
   * look like they have no timing at all.
   *
   *   - "First" is what "preferred" means: the descriptors are ordered and the
   *     first timing is the sink's native mode.  Choosing a later one would
   * need a preference of our own, and falling back to the board's known-good
   *     default is a more predictable outcome than driving a secondary mode.
   *
   * A slot that holds a timing this driver cannot use (-ENOTSUP) or a
   * malformed one (-EINVAL) does not stop the scan -- a later slot may still
   * be usable -- but the reason is remembered so the caller can say why
   * nothing was taken.
   */

  for (i = 0; i < RK3576_HDMI_EDID_DESC_NUMBER; i++)
    {
      desc =
          &edid[RK3576_HDMI_EDID_DESCRIPTOR + i * RK3576_HDMI_EDID_DESC_SIZE];

      status = rk3576_hdmi_edid_dtd(desc, &candidate);
      if (status == RK3576_HDMI_DTD_STATUS_OK)
        {
          rk3576_hdmi_edid_manufacturer(edid, candidate.manufacturer);
          *timing = candidate;
          return OK;
        }

      if (status != RK3576_HDMI_DTD_STATUS_NOT_TIMING && unusable == -ENOENT)
        {
          unusable = (status == RK3576_HDMI_DTD_STATUS_MALFORMED) ? -EINVAL
                                                                  : -ENOTSUP;
        }
    }

  return unusable;
}

/****************************************************************************
 * Name: rk3576_hdmi_uninitialize
 *
 * Description:
 *   Stop the stream, park the PHY, hold the controller in reset and turn its
 *   clocks off.  Present for symmetry with initialize() and for a future
 *   power-management path; board bring-up never calls it.
 *
 ****************************************************************************/

int rk3576_hdmi_uninitialize(void)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  /* Inline what rk3576_hdmi_disable() does rather than calling it: the mutex
   * is not recursive, so calling it here would deadlock against ourselves.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0, 0,
                        RK3576_HDMI_OPMODE_DVI);
  rk3576_hdptxphy_power_off();

  priv->streaming = false;
  priv->pixel_clock = 0;
  priv->bpc = 0;

  /* Hold the controller in reset before stopping its clocks, so that it
   * cannot issue partial AXI transactions while pclk disappears.
   */

  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_REF_BIT, true);
  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_APB_BIT, true);

  if (priv->hdp != NULL)
    {
      clk_disable(priv->hdp);
      priv->hdp = NULL;
    }

  clk_disable(priv->ref);
  priv->ref = NULL;

  clk_disable(priv->pclk);
  priv->pclk = NULL;

  priv->ref_hz = 0;
  priv->initialized = false;

  nxmutex_unlock(&priv->lock);
  return OK;
}

#endif /* CONFIG_RK3576_HDMI */
