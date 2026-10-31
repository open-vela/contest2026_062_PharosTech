/****************************************************************************
 * apps/graphics/nyabula_display/nyabula_te.h
 *
 * TE (vertical-sync) edge source abstraction for the dual-panel display.
 *
 * The TE signal is the only hardware synchronization the BlankGated
 * scheduler needs.  Two edge events are defined (polarity is FIXED and
 * identical for every source):
 *
 *   scan_start(sid)  : TE falling edge (HIGH -> LOW): blanking ended, a
 *                      new frame scan begins -> clear in_blanking, ask for
 *                      a new frame.
 *   blank_start(sid) : TE rising edge (LOW -> HIGH): scanning finished,
 *                      blanking begins -> the ONLY gate at which a
 *                      whole-frame QSPI write may start.
 *
 * The framework (nyabula_dual_lcd.c) registers these callbacks; the TE
 * source (software frame clock today, a panel TE GPIO interrupt later)
 * calls them on the actual edges.  Exactly one source implementation is
 * compiled, selected by Kconfig:
 *
 *   CONFIG_NYABULA_DISPLAY_TE_SW       - software frame clock (no TE pin
 *                                        wired).  A high-priority thread
 *                                        synthesizes the two edges per
 *                                        frame.
 *   CONFIG_NYABULA_DISPLAY_TE_BOARDCTL - one panel TE GPIO per screen; a
 *                                        BOARD-LEVEL ISR classifies the edge
 *                                        direction and posts a per-direction
 *                                        semaphore (registered via
 *                                        boardctl), which a high-priority
 *                                        thread consumes to drive the
 *                                        algorithm entry points.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 ****************************************************************************/

#ifndef __NYABULA_TE_H
#define __NYABULA_TE_H

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NYABULA_TE_MAX_SCREENS 2

/* TE edge handling priority (NuttX: lower number = higher priority).  The
 * TE source runs at a high priority so its edges fire on time and are not
 * delayed by the render or transfer threads. */

#ifndef NYABULA_TE_PRIORITY
#define NYABULA_TE_PRIORITY 95
#endif

/* Software TE timing (used only by the SW source): refresh rate in Hz and
 * active (scanning) time per frame in us.  The SW clock fires scan-start at
 * the frame boundary and blank-start ACTIVE_US later, so the synthesized TE
 * stays HIGH (blanking) for (frame_period - ACTIVE_US) and LOW (scanning)
 * for ACTIVE_US, matching the panel.  ACTIVE_US must be larger than the
 * whole-frame QSPI write time (7ms) for the catch-up scan to hold. */

#ifndef NYABULA_TE_REFRESH_HZ
#define NYABULA_TE_REFRESH_HZ 60
#endif

#ifndef NYABULA_TE_ACTIVE_US
#define NYABULA_TE_ACTIVE_US 7500
#endif

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/* Create a thread with an explicit SCHED_FIFO priority (NuttX: lower
 * number = higher priority).  Shared by all TE sources so their edge
 * threads run on time and are not delayed by the render or transfer
 * threads.  Returns 0 on success, or a POSIX error number. */

static inline int nyabula_te_create_thread_prio(pthread_t *thr,
                                                void *(*fn)(void *), void *arg,
                                                int prio)
{
  pthread_attr_t attr;
  struct sched_param param;
  int ret;

  pthread_attr_init(&attr);
  param.sched_priority = prio;
  ret = pthread_attr_setschedparam(&attr, &param);
  if (ret != 0)
    {
      pthread_attr_destroy(&attr);
      return ret;
    }

  ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  if (ret != 0)
    {
      pthread_attr_destroy(&attr);
      return ret;
    }

  ret = pthread_create(thr, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  return ret;
}

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct nyabula_dual_lcd_s;

/* Edge callbacks implemented by the framework.  Called by the TE source on
 * the actual edges (see the polarity notes above). */

typedef struct
{
  /* TE falling edge: blanking ended, a new frame scan begins. */
  void (*scan_start)(struct nyabula_dual_lcd_s *dual, int sid);

  /* TE rising edge: scanning finished, blanking begins. */
  void (*blank_start)(struct nyabula_dual_lcd_s *dual, int sid);
} nyabula_te_callbacks_t;

/* Opaque TE source handle (one instance; allocated by init). */

typedef struct nyabula_te_s nyabula_te_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Create and start the TE source selected by Kconfig.  `cb` must remain
 * valid until deinit.  Returns an opaque handle, or NULL on failure. */

nyabula_te_t *nyabula_te_init(struct nyabula_dual_lcd_s *dual,
                              const nyabula_te_callbacks_t *cb);

/* Stop the TE source and release all resources. */

void nyabula_te_deinit(nyabula_te_t *te);

#ifdef __cplusplus
}
#endif

#endif /* __NYABULA_TE_H */
