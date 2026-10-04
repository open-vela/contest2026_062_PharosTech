/****************************************************************************
 * chips/rk3576/vepu/rk3576_vepu.h
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
 * RK3576 VEPU510 video encoder -- public interface.
 *
 * Current scope: power domain, resets, clocks and a version probe.  The
 * encoder proper (VEPU510 register programming, H.264 bitstream syntax and
 * registration as a V4L2 M2M codec device) is layered on top of this.
 *
 * Bring-up order, mirroring rk3576_vicap.c:
 *
 *   1. release PD_VEPU0's memory-repair initial reset and keep the domain
 *      powered.  Until that initial reset is released, every register in
 *      the domain reads back as zero -- which looks exactly like a driver
 *      that never programmed anything.
 *   2. release the CRU software resets
 *   3. enable the clocks (including the bus-interface gates)
 *   4. read the VEPU510 version register and check h264_cap
 *
 * No clock rate is programmed here.  Register access only needs aclk/hclk,
 * and the vendor device tree's targets (ACLK 400 MHz, core 702 MHz, see
 * hardware/rk3576_vepu.h) should be set deliberately by the encoder driver
 * once it can measure the candidate parents -- rather than guessed at
 * bring-up time.
 ****************************************************************************/

#ifndef __CHIPS_RK3576_VEPU_RK3576_VEPU_H
#define __CHIPS_RK3576_VEPU_RK3576_VEPU_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/* The encoder's job description is built from these three: the picture and
 * its buffers, the H.264 syntax parameters, and the slice values the picture
 * needs.  They are the same structs the pure register layer consumes, so
 * there is one description of a job rather than one per layer.
 */

#include "h264_syntax.h"
#include "vepu510_regs.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* What the encoder reported about the job it just finished.
 *
 * intsta is kept raw.  The two useful numbers are latched by the hardware
 * and are only meaningful if no error bit is set, so the caller is given the
 * status register itself rather than a filtered view of it.
 */

struct rk3576_vepu_result_s
{
  uint32_t bs_length; /* bytes the encoder wrote to the destination */
  uint32_t sse;       /* distortion figure, as the IP reports it   */
  uint32_t intsta;    /* interrupt status the job ended on         */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vepu_power_on
 *
 * Description:
 *   Bring VEPU0 out of reset: release the power domain's initial reset,
 *   keep the domain powered, let the bus interface run, release the CRU
 *   resets and enable the clocks.  Idempotent.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_vepu_power_on(void);

/****************************************************************************
 * Name: rk3576_vepu_power_off
 *
 * Description:
 *   Undo rk3576_vepu_power_on(): disable the clocks, assert the resets and
 *   request the power domain down.  Idempotent.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_vepu_power_off(void);

/****************************************************************************
 * Name: rk3576_vepu_read_version
 *
 * Description:
 *   Read the VEPU510 version register (CTL block offset 0x0000).  Valid
 *   only after rk3576_vepu_power_on() has succeeded.
 *
 * Returned Value:
 *   The raw register value.  0 means the domain is still held in its
 *   initial reset; 0xffffffff means the register bus is not responding.
 *
 ****************************************************************************/

uint32_t rk3576_vepu_read_version(void);

/****************************************************************************
 * Name: rk3576_vepu_probe
 *
 * Description:
 *   Bring-up acceptance test.  Powers VEPU0 on, reads the version register
 *   and checks that the IP identifies itself as a functioning VEPU510 with
 *   H.264 encode capability.  Logs what it found.
 *
 *   This is deliberately the first thing to run against the hardware: it
 *   validates power, clocks, resets and register access in one step,
 *   without needing any of the encoder logic to work.
 *
 * Returned Value:
 *   OK if the IP answered with a plausible, H.264-capable version;
 *   a negated errno otherwise.
 *
 ****************************************************************************/

int rk3576_vepu_probe(void);

/****************************************************************************
 * Name: rk3576_vepu_initialize
 *
 * Description:
 *   Prepare VEPU0 for encoding: power the domain on, leave its MMU in
 *   pass-through, and attach and enable the completion interrupt.
 *
 *   Idempotent, and intended to be called once from board bring-up.  It is
 *   separate from the encode path so that the interrupt can be attached from
 *   a context where that is allowed, rather than on the first job.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_vepu_initialize(void);

/****************************************************************************
 * Name: rk3576_vepu_uninitialize
 *
 * Description:
 *   Detach the interrupt and power the domain down.  Idempotent.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_vepu_uninitialize(void);

/****************************************************************************
 * Name: rk3576_vepu_reset
 *
 * Description:
 *   Ask the block to clear itself, with the same soft reset the vendor kernel
 *   uses, so that a stream start begins where a power-on begins.
 *
 *   The block keeps internal state that no register write reaches, and a
 *   stream whose registers are all correct can still be encoded with the
 *   wrong state underneath them.  A stream start is therefore a cold start:
 *   the codec calls this when a stream begins, so that the second stream of a
 *   run begins where the first one did.
 *
 *   It is the soft reset (enc_clr) rather than a power cycle or a reset of
 *   the block's clock and reset lines, which the vendor kernel treats as the
 *   fallback for when this fails, and which were both tried on hardware here
 *   and both leave the encoder unable to complete a job.  See the note in the
 *   driver for what each of them does to it.
 *
 *   Safe to call whenever the block is up, and does nothing if it is not.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.  A block that does not report
 *   the safe clear is logged, and the reset completes with a force clear.
 *
 ****************************************************************************/

int rk3576_vepu_reset(void);

/****************************************************************************
 * Name: rk3576_vepu_encode
 *
 * Description:
 *   Encode one picture and wait for the hardware to finish.
 *
 *   This is the whole of the hardware's job: build the register image, write
 *   it, start the encoder, wait, and report what it said.  Nothing above it
 *   needs to know how the registers are laid out, and nothing below it needs
 *   to know what a V4L2 buffer is.
 *
 *   frm describes the source picture and both buffers, cfg the H.264 syntax,
 *   slice the slice values.  The destination buffer is written from its
 *   start; dst_offset is the offset the encoder's write pointer begins at,
 *   which is zero for a fresh buffer.
 *
 *   One job at a time.  The hardware has a single encoder, so concurrent
 *   callers are serialised on a mutex rather than left to interleave their
 *   register writes.
 *
 * Input Parameters:
 *   frm    - source picture, source buffer and destination buffer
 *   cfg    - H.264 syntax parameters
 *   slice  - slice-level values for this picture
 *   result - receives what the encoder reported; may be NULL
 *
 * Returned Value:
 *   OK on success.  A negated errno on failure: -ETIMEDOUT if the encoder
 *   never signalled completion, -EIO if it completed with an error bit set
 *   or produced no bitstream, -EINVAL for an unusable description.
 *
 ****************************************************************************/

int rk3576_vepu_encode(FAR const struct rk3576_vepu510_frame_s *frm,
                       FAR const struct rk3576_h264_cfg_s *cfg,
                       FAR const struct rk3576_vepu510_slice_s *slice,
                       FAR struct rk3576_vepu_result_s *result);

/****************************************************************************
 * Name: rk3576_vepu_dump
 *
 * Description:
 *   Log the encoder's state and the registers that decide whether it started
 *   at all.  Intended to be called when a job times out or reports an error,
 *   from task context.
 *
 ****************************************************************************/

void rk3576_vepu_dump(void);

#ifdef CONFIG_RK3576_VEPU_SELFTEST

/****************************************************************************
 * Name: rk3576_vepu_selftest
 *
 * Description:
 *   Bring-up acceptance test for the encode path: build a small picture in
 *   ordinary memory, hand it to the encoder and report what came back.
 *
 *   This is to the encode path what rk3576_vepu_probe() is to the clock and
 *   reset path.  The picture is synthesised rather than captured so that a
 *   failure cannot be a camera's, and the configuration is the plainest one
 *   the register layer supports, so that a failure cannot be a syntax bug
 *   either -- what is left is the hardware path itself.
 *
 * Returned Value:
 *   OK if the encoder produced a bitstream that starts like an H.264 IDR;
 *   a negated errno otherwise.  What it found is logged either way.
 *
 ****************************************************************************/

int rk3576_vepu_selftest(void);

#endif /* CONFIG_RK3576_VEPU_SELFTEST */

#endif /* __CHIPS_RK3576_VEPU_RK3576_VEPU_H */
