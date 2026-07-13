/****************************************************************************
 * chips/rk3576/rk3576_dma.c
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
 * RK3576 ARM PL330 (DMA-330) driver.  The controller has no direct
 * source/destination/count registers; each of its 8 channels executes a
 * small micro-code program that the CPU assembles in DMA-visible memory and
 * launches through the debug interface (DBGINST0/1 + DBGCMD DMAGO).  A
 * channel raises an event/interrupt on completion via the DMASEV micro-code
 * instruction; the ISR reads INTMIS, clears via INTCLR and dispatches to the
 * per-channel callback.  Channel faults are reported through FSRC/FTRn.
 *
 * Scope: single-shot (one buffer) transfers, memory-to-memory and
 * peripheral (M2P/P2M via the peripheral-request interface, DMAWFP).
 * Cyclic and scatter/gather transfers are deferred and noted below.
 *
 * The platform (RK3576, Cortex-A) is cache coherent to peripherals only via
 * explicit maintenance; this is a UP (single core) configuration.  The
 * per-channel micro-code buffer is written by the CPU and read by the DMAC,
 * so it is cache-line aligned and cleaned before launch, mirroring the
 * mmcsd/eMMC descriptor handling in this chip directory.  The caller's data
 * buffers are cleaned (source) / invalidated (destination) by the SAI/other
 * consumer that owns them.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "arm64_internal.h"
#include "hardware/rk3576_dma.h"
#include "rk3576_dma.h"

#ifdef CONFIG_RK3576_DMA

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base address.  Only dmac0 is instantiated by default; dmac1
 * (0x2abb0000) and dmac2 (0x2abd0000) can be added by giving each its own
 * instance of struct rk3576_dma_ctrl_s.
 */

#ifndef CONFIG_RK3576_DMAC0_BASE
#  define CONFIG_RK3576_DMAC0_BASE   0x2ab90000
#endif

/* GIC INTIDs for dmac0 (from arch/arm64/include/rk3576/irq.h).  The "event"
 * line signals DMASEV completion and normal channel interrupts; the "abort"
 * line signals a manager/channel fault.  NOTE: the TRM numbers the SPIs as
 * event=SPI0 (INTID 64) and abort=SPI1 (INTID 65); verify against the TRM
 * DMA request table if a controller other than dmac0 is added.
 */

#define RK3576_DMA_IRQ_EVENT       RK3576_IRQ_DMAC0        /* INTID 64      */
#define RK3576_DMA_IRQ_ABORT       RK3576_IRQ_DMAC0_ABORT  /* INTID 65      */

/* DMAGO launch state.  openvela runs non-secure at EL1, so the DMA manager
 * is expected to have booted non-secure and DMAGO must carry the NS bit.
 * Verify with CR0.mgr_ns_at_rst / the platform boot state on hardware.
 */

#define RK3576_DMA_LAUNCH_NS       1

/* PL330 hardware limits. */

#define RK3576_DMA_MAX_BURSTLEN    16     /* Beats per burst (CCR 4-bit)   */
#define RK3576_DMA_LOOP_MAX        256    /* 8-bit loop counter (0..255+1) */

/* Micro-code program buffer: one cache-line-aligned buffer per channel.
 * The largest program (nested loops + peripheral wait) is well under 128
 * bytes; round the buffer up to a cache line.
 */

#define RK3576_DMA_PROG_SIZE       128

/* ARMv8-A L1 D-cache line size; the micro-code buffer is aligned to this so
 * a clean of the program never touches neighbouring data.
 */

#define RK3576_DMA_CACHELINE       64

/* Poll bound for the debug interface / channel-stop handshake. */

#define RK3576_DMA_SPIN            100000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One DMA channel. */

struct rk3576_dmach_s
{
  uint8_t   chan;       /* Channel number 0..7                            */
  uint8_t   periph;     /* Peripheral request line, or PERIPH_NONE        */
  bool      inuse;      /* Channel reserved                               */
  uint8_t   direction;  /* Cached from setup (M2M/M2P/P2M)                */
  size_t    proglen;    /* Assembled micro-code length in bytes           */
  rk3576_dma_callback_t callback;
  void     *arg;

  /* Micro-code program buffer.  DMA-visible, cache-line aligned so it can
   * be cleaned independently of neighbouring data.
   */

  uint8_t   prog[RK3576_DMA_PROG_SIZE]
            aligned_data(RK3576_DMA_CACHELINE);
};

/* One controller instance. */

struct rk3576_dma_ctrl_s
{
  uintptr_t base;       /* Controller register base                       */
  int       irq_event;  /* Completion/event interrupt                     */
  int       irq_abort;  /* Fault/abort interrupt                          */
  spinlock_t lock;      /* Guards channel allocation + register access    */
  struct rk3576_dmach_s chan[RK3576_DMA_NCHANNELS];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_dma_getreg(struct rk3576_dma_ctrl_s *ctrl,
                                  unsigned int off);
static void rk3576_dma_putreg(struct rk3576_dma_ctrl_s *ctrl,
                              unsigned int off, uint32_t val);

static struct rk3576_dma_ctrl_s *rk3576_dma_ctrl_of(struct rk3576_dmach_s *ch);

static int rk3576_dma_ilog2(unsigned int v);
static uint32_t rk3576_dma_build_ccr(
                              const struct rk3576_dma_config_s *config);

static size_t rk3576_dma_emit_mov(uint8_t *p, uint8_t rd, uint32_t imm);
static size_t rk3576_dma_emit_body(uint8_t *p, uint8_t direction,
                                   uint8_t periph);
static size_t rk3576_dma_emit_loop(uint8_t *p, unsigned int nbursts,
                                   uint8_t direction, uint8_t periph);

static int rk3576_dma_debug_exec(struct rk3576_dma_ctrl_s *ctrl,
                                 bool channel, uint8_t chan,
                                 const uint8_t *insn, size_t len);

static void rk3576_dma_dispatch(struct rk3576_dma_ctrl_s *ctrl,
                                unsigned int chan, int result);
static int rk3576_dma_event_isr(int irq, void *context, void *arg);
static int rk3576_dma_abort_isr(int irq, void *context, void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* dmac0.  Additional controllers get their own instance and irq attach. */

static struct rk3576_dma_ctrl_s g_rk3576_dmac0 =
{
  .base      = CONFIG_RK3576_DMAC0_BASE,
  .irq_event = RK3576_DMA_IRQ_EVENT,
  .irq_abort = RK3576_DMA_IRQ_ABORT,
  .lock      = SP_UNLOCKED,
};

static bool g_rk3576_dma_initialized;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rk3576_dma_getreg(struct rk3576_dma_ctrl_s *ctrl,
                                  unsigned int off)
{
  return getreg32(ctrl->base + off);
}

static void rk3576_dma_putreg(struct rk3576_dma_ctrl_s *ctrl,
                              unsigned int off, uint32_t val)
{
  putreg32(val, ctrl->base + off);
}

/****************************************************************************
 * Name: rk3576_dma_ctrl_of
 *
 * Description:
 *   Return the controller owning a channel.  Only dmac0 exists today, so
 *   this is a direct mapping; kept as a helper so more controllers can be
 *   added without touching the call sites.
 ****************************************************************************/

static struct rk3576_dma_ctrl_s *rk3576_dma_ctrl_of(struct rk3576_dmach_s *ch)
{
  UNUSED(ch);
  return &g_rk3576_dmac0;
}

/****************************************************************************
 * Name: rk3576_dma_ilog2
 *
 * Description:
 *   Return log2(v) for a power-of-two v in 1..8, or a negative errno if v is
 *   not a supported PL330 transfer width.
 ****************************************************************************/

static int rk3576_dma_ilog2(unsigned int v)
{
  switch (v)
    {
      case 1:
        return 0;
      case 2:
        return 1;
      case 4:
        return 2;
      case 8:
        return 3;
      default:
        return -EINVAL;
    }
}

/****************************************************************************
 * Name: rk3576_dma_build_ccr
 *
 * Description:
 *   Build the 32-bit CCR value that the DMAMOV CCR micro-code instruction
 *   loads into the channel.  Encodes source/destination increment, burst
 *   size (transfer width) and burst length.  Protection defaults to
 *   privileged + (non-)secure to match the launch state; cache control is
 *   left at 0 (non-cacheable / device) because coherency is handled by
 *   explicit cache maintenance in the consumer.  No endian swap.
 *
 *   For a peripheral transfer the peripheral side address does not increment
 *   (fixed FIFO register) and the memory side does.
 ****************************************************************************/

static uint32_t rk3576_dma_build_ccr(
                              const struct rk3576_dma_config_s *config)
{
  uint32_t ccr = 0;
  int      ssize;
  int      dsize;
  bool     src_inc;
  bool     dst_inc;
  uint32_t prot_ns = RK3576_DMA_LAUNCH_NS ? DMA_CCR_SRC_PROT_NS : 0;

  ssize = rk3576_dma_ilog2(config->src_width);
  dsize = rk3576_dma_ilog2(config->dst_width);

  /* Increment rules per direction. */

  switch (config->direction)
    {
      case RK3576_DMA_M2P:
        src_inc = true;   /* memory increments */
        dst_inc = false;  /* peripheral FIFO fixed */
        break;

      case RK3576_DMA_P2M:
        src_inc = false;  /* peripheral FIFO fixed */
        dst_inc = true;   /* memory increments */
        break;

      case RK3576_DMA_M2M:
      default:
        src_inc = true;
        dst_inc = true;
        break;
    }

  if (src_inc)
    {
      ccr |= DMA_CCR_SRC_INC;
    }

  if (dst_inc)
    {
      ccr |= DMA_CCR_DST_INC;
    }

  ccr |= (uint32_t)ssize << DMA_CCR_SRC_BURSTSIZE_SHIFT;
  ccr |= (uint32_t)dsize << DMA_CCR_DST_BURSTSIZE_SHIFT;
  ccr |= (uint32_t)(config->burst_len - 1) << DMA_CCR_SRC_BURSTLEN_SHIFT;
  ccr |= (uint32_t)(config->burst_len - 1) << DMA_CCR_DST_BURSTLEN_SHIFT;

  ccr |= DMA_CCR_SRC_PROT_PRIV | DMA_CCR_DST_PROT_PRIV;
  ccr |= prot_ns;
  ccr |= RK3576_DMA_LAUNCH_NS ? DMA_CCR_DST_PROT_NS : 0;

  return ccr;
}

/****************************************************************************
 * Name: rk3576_dma_emit_mov
 *
 * Description:
 *   Emit a DMAMOV <rd>, imm32 instruction (6 bytes: opcode, register
 *   selector, little-endian 32-bit immediate).  Returns the byte count.
 ****************************************************************************/

static size_t rk3576_dma_emit_mov(uint8_t *p, uint8_t rd, uint32_t imm)
{
  p[0] = DMA_OP_DMAMOV;
  p[1] = rd;
  p[2] = (uint8_t)(imm & 0xff);
  p[3] = (uint8_t)((imm >> 8) & 0xff);
  p[4] = (uint8_t)((imm >> 16) & 0xff);
  p[5] = (uint8_t)((imm >> 24) & 0xff);
  return 6;
}

/****************************************************************************
 * Name: rk3576_dma_emit_body
 *
 * Description:
 *   Emit the per-burst body of the transfer loop.  For a peripheral
 *   transfer this waits for the peripheral's burst request (DMAWFP burst)
 *   before the load/store so the FIFO paces the DMA; for memory-to-memory it
 *   is an unconditional load/store pair.  Returns the byte count.
 ****************************************************************************/

static size_t rk3576_dma_emit_body(uint8_t *p, uint8_t direction,
                                   uint8_t periph)
{
  size_t n = 0;

  if (direction == RK3576_DMA_M2P)
    {
      /* Memory -> peripheral (e.g. SAI TX FIFO).  The SAI issues single (per
       * sample) DMA requests, so wait for a single request, load one word
       * from memory and store it to the peripheral with DMASTP (single) so
       * the DMAC acknowledges the request.  A burst transfer overruns the
       * FIFO write port and the samples are lost (audio plays fast + broken).
       */

      p[n++] = DMA_OP_DMAWFP | DMA_WFP_BURST;
      p[n++] = (uint8_t)((periph & 0x1f) << 3);
      p[n++] = DMA_OP_DMALDB;
      p[n++] = DMA_OP_DMASTP | DMA_WFP_BURST;
      p[n++] = (uint8_t)((periph & 0x1f) << 3);
    }
  else if (direction == RK3576_DMA_P2M)
    {
      p[n++] = DMA_OP_DMAWFP | DMA_WFP_BURST;
      p[n++] = (uint8_t)((periph & 0x1f) << 3);
      p[n++] = DMA_OP_DMALDP | DMA_WFP_BURST;
      p[n++] = (uint8_t)((periph & 0x1f) << 3);
      p[n++] = DMA_OP_DMASTB;
    }
  else
    {
      /* Unconditional burst load/store for memory-to-memory. */

      p[n++] = DMA_OP_DMALD;
      p[n++] = DMA_OP_DMAST;
    }

  return n;
}

/****************************************************************************
 * Name: rk3576_dma_emit_loop
 *
 * Description:
 *   Emit the transfer loop covering nbursts bursts.  The PL330 loop counter
 *   is 8-bit (max 256 iterations), so counts up to 256 use a single DMALP;
 *   larger counts use a nested DMALP (LC0 outer, LC1 inner) plus a trailing
 *   single-loop remainder.  Returns the byte count, or 0 if nbursts exceeds
 *   the nested-loop capacity (256*256).
 ****************************************************************************/

static size_t rk3576_dma_emit_loop(uint8_t *p, unsigned int nbursts,
                                   uint8_t direction, uint8_t periph)
{
  size_t n = 0;

  if (nbursts == 0)
    {
      return 0;
    }

  if (nbursts <= RK3576_DMA_LOOP_MAX)
    {
      /* Single loop: DMALP lc0, (nbursts-1) ... DMALPEND. */

      uint8_t *lpstart;
      uint8_t *lpend;
      size_t   body;

      p[n++] = DMA_OP_DMALP0;
      p[n++] = (uint8_t)(nbursts - 1);
      lpstart = &p[n];

      body = rk3576_dma_emit_body(&p[n], direction, periph);
      n += body;

      /* DMALPEND: finite loop, unconditional, backward jump = distance from
       * the byte after DMALPEND's opcode back to the first looped opcode.
       * Encoded as (bytes from loop start to the LPEND opcode).
       */

      lpend = &p[n];
      p[n++] = DMA_OP_DMALPEND | DMA_LPEND_NF;
      p[n++] = (uint8_t)(lpend - lpstart);
    }
  else
    {
      /* Nested loop.  outer * inner full bursts + remainder. */

      unsigned int outer = nbursts / RK3576_DMA_LOOP_MAX;
      unsigned int inner = RK3576_DMA_LOOP_MAX;
      unsigned int rem   = nbursts % RK3576_DMA_LOOP_MAX;
      uint8_t     *lpout;
      uint8_t     *lpin;
      uint8_t     *lpend;
      size_t       body;

      if (outer > RK3576_DMA_LOOP_MAX)
        {
          return 0;   /* Beyond 256*256 bursts: caller must split */
        }

      /* Outer loop (LC0). */

      p[n++] = DMA_OP_DMALP0;
      p[n++] = (uint8_t)(outer - 1);
      lpout  = &p[n];

      /* Inner loop (LC1). */

      p[n++] = DMA_OP_DMALP1;
      p[n++] = (uint8_t)(inner - 1);
      lpin   = &p[n];

      body = rk3576_dma_emit_body(&p[n], direction, periph);
      n += body;

      lpend = &p[n];
      p[n++] = DMA_OP_DMALPEND | DMA_LPEND_NF | DMA_LPEND_LC1;
      p[n++] = (uint8_t)(lpend - lpin);

      lpend = &p[n];
      p[n++] = DMA_OP_DMALPEND | DMA_LPEND_NF;
      p[n++] = (uint8_t)(lpend - lpout);

      /* Remainder bursts in a trailing single loop. */

      if (rem > 0)
        {
          uint8_t *rstart;
          uint8_t *rend;

          p[n++] = DMA_OP_DMALP0;
          p[n++] = (uint8_t)(rem - 1);
          rstart = &p[n];

          body = rk3576_dma_emit_body(&p[n], direction, periph);
          n += body;

          rend = &p[n];
          p[n++] = DMA_OP_DMALPEND | DMA_LPEND_NF;
          p[n++] = (uint8_t)(rend - rstart);
        }
    }

  return n;
}

/****************************************************************************
 * Name: rk3576_dma_debug_exec
 *
 * Description:
 *   Execute a 1..6 byte micro-code instruction through the debug interface.
 *   Used to issue DMAGO (launch, channel == false / DMA manager thread) and
 *   DMAKILL (channel == true / DMA channel thread).  Poll DBGSTATUS until the
 *   debug engine is idle, write the instruction into DBGINST0/DBGINST1, then
 *   trigger it with DBGCMD.
 *
 *   DBGINST0: [31:24]=instr byte1, [23:16]=instr byte0, [10:8]=channel,
 *             [0]=thread (0 manager, 1 channel).
 *   DBGINST1: instr bytes 2..5 (the 32-bit immediate), if present.
 ****************************************************************************/

static int rk3576_dma_debug_exec(struct rk3576_dma_ctrl_s *ctrl,
                                 bool channel, uint8_t chan,
                                 const uint8_t *insn, size_t len)
{
  uint32_t inst0;
  uint32_t inst1 = 0;
  int      i;

  /* Wait for the debug engine to be idle. */

  for (i = 0; i < RK3576_DMA_SPIN; i++)
    {
      if ((rk3576_dma_getreg(ctrl, RK3576_DMA_DBGSTATUS) &
           DMA_DBGSTATUS_BUSY) == 0)
        {
          break;
        }
    }

  if (i >= RK3576_DMA_SPIN)
    {
      dmaerr("ERROR: debug engine busy\n");
      return -EBUSY;
    }

  inst0  = (uint32_t)insn[0] << DMA_DBGINST0_INSB0_SHIFT;
  inst0 |= (uint32_t)insn[1] << DMA_DBGINST0_INSB1_SHIFT;
  inst0 |= (uint32_t)(chan & 0x7) << DMA_DBGINST0_CHAN_SHIFT;
  inst0 |= channel ? DMA_DBGINST0_THREAD_CHAN : DMA_DBGINST0_THREAD_MGR;

  if (len > 2)
    {
      inst1 = (uint32_t)insn[2]        |
              ((uint32_t)insn[3] << 8) |
              ((uint32_t)insn[4] << 16) |
              ((uint32_t)insn[5] << 24);
    }

  rk3576_dma_putreg(ctrl, RK3576_DMA_DBGINST0, inst0);
  rk3576_dma_putreg(ctrl, RK3576_DMA_DBGINST1, inst1);

  /* Kick it off. */

  rk3576_dma_putreg(ctrl, RK3576_DMA_DBGCMD, DMA_DBGCMD_EXECUTE);
  return OK;
}

/****************************************************************************
 * Name: rk3576_dma_dispatch
 *
 * Description:
 *   Deliver the completion/fault result for a channel to its callback and
 *   clear its bookkeeping.  Called from interrupt context.
 ****************************************************************************/

static void rk3576_dma_dispatch(struct rk3576_dma_ctrl_s *ctrl,
                                unsigned int chan, int result)
{
  struct rk3576_dmach_s *ch = &ctrl->chan[chan];
  rk3576_dma_callback_t  cb = ch->callback;
  void                  *arg = ch->arg;

  if (cb != NULL)
    {
      cb(ch, result, arg);
    }
}

/****************************************************************************
 * Name: rk3576_dma_event_isr
 *
 * Description:
 *   Completion interrupt.  A channel that finished raised its event via
 *   DMASEV, setting the matching INTMIS bit.  Event number equals channel
 *   number in this driver.  Clear the interrupt and dispatch success.
 ****************************************************************************/

static int rk3576_dma_event_isr(int irq, void *context, void *arg)
{
  struct rk3576_dma_ctrl_s *ctrl = arg;
  uint32_t mis;
  unsigned int chan;

  UNUSED(irq);
  UNUSED(context);

  mis = rk3576_dma_getreg(ctrl, RK3576_DMA_INTMIS);

  for (chan = 0; chan < RK3576_DMA_NCHANNELS; chan++)
    {
      if ((mis & (1u << chan)) != 0)
        {
          /* Clear this event before invoking the callback. */

          rk3576_dma_putreg(ctrl, RK3576_DMA_INTCLR, 1u << chan);
          rk3576_dma_dispatch(ctrl, chan, OK);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_dma_abort_isr
 *
 * Description:
 *   Fault interrupt.  FSRD flags a DMA-manager fault; FSRC flags per-channel
 *   faults.  Log the fault type (FTRD / FTRn), issue DMAKILL to stop the
 *   faulting channel, and dispatch a failure to its callback.
 ****************************************************************************/

static int rk3576_dma_abort_isr(int irq, void *context, void *arg)
{
  struct rk3576_dma_ctrl_s *ctrl = arg;
  uint32_t fsrc;
  uint32_t fsrd;
  unsigned int chan;

  UNUSED(irq);
  UNUSED(context);

  fsrd = rk3576_dma_getreg(ctrl, RK3576_DMA_FSRD);
  if (fsrd != 0)
    {
      dmaerr("ERROR: DMA manager fault FTRD=%08" PRIx32 "\n",
             rk3576_dma_getreg(ctrl, RK3576_DMA_FTRD));
    }

  fsrc = rk3576_dma_getreg(ctrl, RK3576_DMA_FSRC);

  for (chan = 0; chan < RK3576_DMA_NCHANNELS; chan++)
    {
      if ((fsrc & (1u << chan)) != 0)
        {
          uint8_t kill = DMA_OP_DMAKILL;

          dmaerr("ERROR: DMA channel %u fault FTR=%08" PRIx32 "\n",
                 chan, rk3576_dma_getreg(ctrl, RK3576_DMA_FTR(chan)));

          /* Move the faulting channel back to Stopped. */

          rk3576_dma_debug_exec(ctrl, true, (uint8_t)chan, &kill, 1);
          rk3576_dma_dispatch(ctrl, chan, -EIO);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_dma_initialize
 *
 * Description:
 *   One-time controller bring-up: confirm the controller is clocked, mask
 *   and clear all events, route every event to the interrupt line, and
 *   attach the event/abort ISRs.  Called lazily from the first
 *   rk3576_dma_get_channel().
 ****************************************************************************/

static int rk3576_dma_initialize(struct rk3576_dma_ctrl_s *ctrl)
{
  uint32_t cr0;
  int      ret;
  int      i;

  /* Confirm the controller is clocked by reading CR0 (a gated block reads
   * back 0 / all-ones).  The bootloader normally leaves the dmac aclk
   * (CRU clk id 0xC9) ungated; if CR0 does not report the expected 8
   * channels, a CRU ungate (mirror rk3576_cru_*_enable) is required here.
   * Left as a probe concern rather than programming CRU bits blindly.
   */

  cr0 = rk3576_dma_getreg(ctrl, RK3576_DMA_CR0);
  if (cr0 == 0 || cr0 == 0xffffffff)
    {
      dmaerr("ERROR: dmac@%" PRIxPTR " CR0=%08" PRIx32
             " (clock gated?)\n", ctrl->base, cr0);
      return -ENODEV;
    }

  dmainfo("dmac@%" PRIxPTR " CR0=%08" PRIx32 " channels=%u\n",
          ctrl->base, cr0,
          ((cr0 & DMA_CR0_NUM_CHNLS_MASK) >> DMA_CR0_NUM_CHNLS_SHIFT) + 1);

  /* Initialise channel bookkeeping. */

  for (i = 0; i < RK3576_DMA_NCHANNELS; i++)
    {
      ctrl->chan[i].chan  = (uint8_t)i;
      ctrl->chan[i].inuse = false;
    }

  /* Mask and clear all events, then route every event to the interrupt
   * line (INTEN bit set = event N raises an interrupt rather than only
   * being observable via INT_EVENT_RIS).
   */

  rk3576_dma_putreg(ctrl, RK3576_DMA_INTEN, 0);
  rk3576_dma_putreg(ctrl, RK3576_DMA_INTCLR, 0xff);
  rk3576_dma_putreg(ctrl, RK3576_DMA_INTEN, 0xff);

  /* Attach and enable both interrupt lines. */

  ret = irq_attach(ctrl->irq_event, rk3576_dma_event_isr, ctrl);
  if (ret < 0)
    {
      dmaerr("ERROR: irq_attach event irq=%d ret=%d\n",
             ctrl->irq_event, ret);
      return ret;
    }

  ret = irq_attach(ctrl->irq_abort, rk3576_dma_abort_isr, ctrl);
  if (ret < 0)
    {
      dmaerr("ERROR: irq_attach abort irq=%d ret=%d\n",
             ctrl->irq_abort, ret);
      return ret;
    }

  up_enable_irq(ctrl->irq_event);
  up_enable_irq(ctrl->irq_abort);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dma_get_channel
 ****************************************************************************/

struct rk3576_dmach_s *rk3576_dma_get_channel(unsigned int periph_req)
{
  struct rk3576_dma_ctrl_s *ctrl = &g_rk3576_dmac0;
  struct rk3576_dmach_s    *ch = NULL;
  irqstate_t                flags;
  int                       i;

  flags = spin_lock_irqsave(&ctrl->lock);

  if (!g_rk3576_dma_initialized)
    {
      if (rk3576_dma_initialize(ctrl) < 0)
        {
          spin_unlock_irqrestore(&ctrl->lock, flags);
          return NULL;
        }

      g_rk3576_dma_initialized = true;
    }

  for (i = 0; i < RK3576_DMA_NCHANNELS; i++)
    {
      if (!ctrl->chan[i].inuse)
        {
          ch           = &ctrl->chan[i];
          ch->inuse    = true;
          ch->periph   = (uint8_t)periph_req;
          ch->callback = NULL;
          ch->arg      = NULL;
          ch->proglen  = 0;
          break;
        }
    }

  spin_unlock_irqrestore(&ctrl->lock, flags);

  if (ch == NULL)
    {
      dmaerr("ERROR: no free DMA channel\n");
    }

  return ch;
}

/****************************************************************************
 * Name: rk3576_dma_free_channel
 ****************************************************************************/

void rk3576_dma_free_channel(struct rk3576_dmach_s *ch)
{
  struct rk3576_dma_ctrl_s *ctrl;
  irqstate_t flags;

  if (ch == NULL)
    {
      return;
    }

  ctrl = rk3576_dma_ctrl_of(ch);

  rk3576_dma_stop(ch);

  flags = spin_lock_irqsave(&ctrl->lock);
  ch->inuse    = false;
  ch->callback = NULL;
  ch->arg      = NULL;
  ch->proglen  = 0;
  spin_unlock_irqrestore(&ctrl->lock, flags);
}

/****************************************************************************
 * Name: rk3576_dma_setup
 *
 * Description:
 *   Validate the request and assemble the channel micro-code program:
 *
 *     DMAMOV SAR, src
 *     DMAMOV DAR, dst
 *     DMAMOV CCR, ccr
 *     [DMAFLUSHP periph]        (peripheral transfers only)
 *     DMALP ... body ... DMALPEND
 *     DMASEV <channel>
 *     DMAEND
 *
 *   The program buffer is cleaned from the D-cache so the DMAC reads the
 *   bytes the CPU just wrote.  The caller is responsible for cache
 *   maintenance on the data buffers referenced by src/dst.
 ****************************************************************************/

int rk3576_dma_setup(struct rk3576_dmach_s *ch,
                     const struct rk3576_dma_config_s *config)
{
  uint32_t     ccr;
  unsigned int bytes_per_burst;
  unsigned int nbursts;
  size_t       n = 0;
  size_t       loop;
  uint8_t     *p;

  if (ch == NULL || config == NULL)
    {
      return -EINVAL;
    }

  /* Only equal src/dst widths are supported for now (matched LD/ST). */

  if (config->src_width != config->dst_width)
    {
      dmaerr("ERROR: asymmetric widths not supported\n");
      return -ENOSYS;
    }

  if (rk3576_dma_ilog2(config->src_width) < 0 ||
      config->burst_len < 1 ||
      config->burst_len > RK3576_DMA_MAX_BURSTLEN)
    {
      return -EINVAL;
    }

  /* PL330 SAR/DAR are 32-bit: addresses must be in the low 4GB. */

  if ((uint64_t)config->src + config->nbytes > 0x100000000ull ||
      (uint64_t)config->dst + config->nbytes > 0x100000000ull)
    {
      dmaerr("ERROR: address above 4GB not addressable by PL330\n");
      return -EFAULT;
    }

  /* The transfer must be a whole number of bursts.  Partial-burst residues
   * (byte tails) are not yet handled; the SAI consumer uses burst-aligned
   * buffers.
   */

  bytes_per_burst = (unsigned int)config->src_width * config->burst_len;
  if (config->nbytes == 0 || (config->nbytes % bytes_per_burst) != 0)
    {
      dmaerr("ERROR: nbytes %zu not a multiple of burst %u\n",
             config->nbytes, bytes_per_burst);
      return -EINVAL;
    }

  nbursts = (unsigned int)(config->nbytes / bytes_per_burst);

  ch->direction = config->direction;
  ch->callback  = config->callback;
  ch->arg       = config->arg;

  ccr = rk3576_dma_build_ccr(config);

  /* Assemble the program. */

  p = ch->prog;
  n += rk3576_dma_emit_mov(&p[n], DMA_MOV_SAR, (uint32_t)config->src);
  n += rk3576_dma_emit_mov(&p[n], DMA_MOV_DAR, (uint32_t)config->dst);
  n += rk3576_dma_emit_mov(&p[n], DMA_MOV_CCR, ccr);

  if (config->direction != RK3576_DMA_M2M)
    {
      /* Discard any stale request state on the peripheral request line. */

      p[n++] = DMA_OP_DMAFLUSHP;
      p[n++] = (uint8_t)((ch->periph & 0x1f) << 3);
    }

  loop = rk3576_dma_emit_loop(&p[n], nbursts, config->direction, ch->periph);
  if (loop == 0)
    {
      dmaerr("ERROR: transfer too large (%u bursts)\n", nbursts);
      return -E2BIG;
    }

  n += loop;

  /* Signal completion event == channel number, then end. */

  p[n++] = DMA_OP_DMASEV;
  p[n++] = (uint8_t)((ch->chan & 0x1f) << 3);
  p[n++] = DMA_OP_DMAEND;

  if (n > RK3576_DMA_PROG_SIZE)
    {
      /* Overran the program buffer.  Should not happen for supported sizes;
       * caught here so a hardware launch never reads past the buffer.
       */

      dmaerr("ERROR: program overflow %zu > %u\n", n,
             (unsigned int)RK3576_DMA_PROG_SIZE);
      return -ENOSPC;
    }

  ch->proglen = n;

  /* Clean the program buffer so the DMAC observes it in memory. */

  up_clean_dcache((uintptr_t)ch->prog, (uintptr_t)ch->prog + n);

  dmainfo("ch%u setup dir=%u bursts=%u proglen=%zu ccr=%08" PRIx32 "\n",
          ch->chan, config->direction, nbursts, n, ccr);
  return OK;
}

/****************************************************************************
 * Name: rk3576_dma_start
 *
 * Description:
 *   Launch the channel with DMAGO through the debug interface.  DMAGO
 *   selects the channel and points it at the program buffer; the DMA manager
 *   thread executes it, which starts the channel thread.
 ****************************************************************************/

int rk3576_dma_start(struct rk3576_dmach_s *ch)
{
  struct rk3576_dma_ctrl_s *ctrl;
  uint8_t  go[6];
  uint32_t prog;

  if (ch == NULL || ch->proglen == 0)
    {
      return -EINVAL;
    }

  ctrl = rk3576_dma_ctrl_of(ch);
  prog = (uint32_t)(uintptr_t)ch->prog;

  /* DMAGO: byte0 = opcode | NS, byte1 = channel, bytes2..5 = program addr. */

  go[0] = DMA_OP_DMAGO | (RK3576_DMA_LAUNCH_NS ? DMA_GO_NS : 0);
  go[1] = ch->chan;
  go[2] = (uint8_t)(prog & 0xff);
  go[3] = (uint8_t)((prog >> 8) & 0xff);
  go[4] = (uint8_t)((prog >> 16) & 0xff);
  go[5] = (uint8_t)((prog >> 24) & 0xff);

  /* DMAGO runs on the DMA manager thread (channel == false). */

  return rk3576_dma_debug_exec(ctrl, false, ch->chan, go, sizeof(go));
}

/****************************************************************************
 * Name: rk3576_dma_stop
 *
 * Description:
 *   Abort the channel with DMAKILL (issued on the channel thread) and clear
 *   any pending event for it.  No completion callback is delivered.
 ****************************************************************************/

int rk3576_dma_stop(struct rk3576_dmach_s *ch)
{
  struct rk3576_dma_ctrl_s *ctrl;
  uint8_t kill = DMA_OP_DMAKILL;
  int     ret;

  if (ch == NULL)
    {
      return -EINVAL;
    }

  ctrl = rk3576_dma_ctrl_of(ch);

  ret = rk3576_dma_debug_exec(ctrl, true, ch->chan, &kill, 1);

  /* Drop any completion event the channel may have already latched. */

  rk3576_dma_putreg(ctrl, RK3576_DMA_INTCLR, 1u << ch->chan);
  return ret;
}

#endif /* CONFIG_RK3576_DMA */
