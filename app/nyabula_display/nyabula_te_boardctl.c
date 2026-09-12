/****************************************************************************
 * apps/graphics/nyabula_display/nyabula_te_boardctl.c
 *
 * Panel TE GPIO interrupt source via boardctl-registered semaphores
 * (CONFIG_NYABULA_DISPLAY_TE_BOARDCTL).
 *
 * Unlike the former POSIX-signal /dev/gpioN approach, this source relies
 * on the BOARD registering a driver-level GPIO interrupt for each TE pin
 * and posting, from interrupt context, one semaphore per edge direction.
 * The semaphores are registered with the board through
 * boardctl(BOARDIOC_USER, ...), so the ISR never goes through the
 * sigqueue/sigwaitinfo signal stack.
 *
 * Why this exists
 *   In the signal-based path, the consumer thread awoke on a signal and
 *   then re-read the pin level to CLASSIFY the edge (rising=blank-start vs
 *   falling=scan-start).  Under heavy CPU load (nyabula_eye) the delay from
 *   the hardware edge to that read could exceed one TE phase, so the thread
 *   read a STALE level and mis-classified the direction, producing unpaired
 *   begin/end edges in the trace.  Here the board classifies the direction
 *   AT INTERRUPT TIME (while the edge is fresh) and posts the matching sem,
 *   so direction is locked in the ISR and can never be read stale.
 *
 * ABI contract with the board (no shared header, by convention):
 *   - boardctl command : BOARDIOC_USER
 *   - arg              : base address of a contiguous array of four sem_t
 *                        (order: [0]=bs0, [1]=ss0, [2]=bs1, [3]=ss1).  The
 *                        board stores the address of each element and posts
 *                        the matching one per classified edge.
 *   - Passing arg == 0 to the same boardctl command clears the board's
 *     registered semaphores (used on teardown).
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "nyabula_te.h"

#include <nuttx/sched_note.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <string.h>
#include <sys/boardctl.h>

#include <lvgl/lvgl.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of edge semaphores across both screens (bs/ss x 2 screens). */

#define NYABULA_TE_SEM_COUNT 4

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Per-thread context: the thread body needs both the source instance and
 * which of the four edge slots it serves. */

struct nyabula_te_thread_ctx_s
{
  struct nyabula_te_s *te;
  int slot; /* 0=bs0, 1=ss0, 2=bs1, 3=ss1 */
};

/* One consumer thread per (screen, edge direction).  Each thread blocks on
 * its own semaphore and calls the framework callback when it fires.  A
 * one-thread-per-direction model avoids both multi-wait multiplexing and
 * direction inference: direction is fixed by which sem was posted. */

struct nyabula_te_s
{
  pthread_t thread[NYABULA_TE_SEM_COUNT];
  bool thread_started[NYABULA_TE_SEM_COUNT];
  struct nyabula_te_thread_ctx_s ctx[NYABULA_TE_SEM_COUNT];
  sem_t sem[NYABULA_TE_SEM_COUNT];
  bool sem_init_done[NYABULA_TE_SEM_COUNT];
  bool running;

  struct nyabula_dual_lcd_s *dual;
  nyabula_te_callbacks_t cb;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Trace mark name for each TE edge: bs = blank-start (rising), ss =
 * scan-start (falling).  Uses a per-edge mark (not a begin/end span) so the
 * visualizer never has to pair unmatched events. */

static const char *te_edge_mark_name(int sid, bool bs)
{
  static const char *const names[2][2] = {
    { "te:bs0", "te:ss0" }, /* sid 0: blank-start, scan-start */
    { "te:bs1", "te:ss1" }  /* sid 1 */
  };

  return names[sid][bs ? 0 : 1];
}

/* Consumer thread: block on its edge semaphore, then drive the framework
 * callback (and trace the edge) when it fires. */

static void *te_thread_func(void *arg)
{
  struct nyabula_te_thread_ctx_s *ctx = (struct nyabula_te_thread_ctx_s *)arg;
  struct nyabula_te_s *te = ctx->te;
  int slot = ctx->slot;
  int sid = (slot < 2) ? 0 : 1;
  bool bs = ((slot & 1) == 0); /* even slot -> blank-start */
  sem_t *sem = &te->sem[slot];

  while (te->running)
    {
      sem_wait(sem);

      if (!te->running)
        {
          break;
        }

      sched_note_mark(NOTE_TAG_ALWAYS, te_edge_mark_name(sid, bs));

      if (bs)
        {
          if (te->cb.blank_start != NULL)
            {
              te->cb.blank_start(te->dual, sid);
            }
        }
      else
        {
          if (te->cb.scan_start != NULL)
            {
              te->cb.scan_start(te->dual, sid);
            }
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

nyabula_te_t *nyabula_te_init(struct nyabula_dual_lcd_s *dual,
                              const nyabula_te_callbacks_t *cb)
{
  struct nyabula_te_s *t;
  int i;
  int ret;

  t = (struct nyabula_te_s *)lv_malloc(sizeof(*t));
  if (t == NULL)
    {
      return NULL;
    }

  memset(t, 0, sizeof(*t));
  t->running = false;
  t->dual = dual;
  if (cb != NULL)
    {
      t->cb = *cb;
    }

  for (i = 0; i < NYABULA_TE_SEM_COUNT; i++)
    {
      t->ctx[i].te = t;
      t->ctx[i].slot = i;

      sem_init(&t->sem[i], 0, 0);
      t->sem_init_done[i] = true;
    }

  /* Register the four semaphores with the board ISR.  arg points to a
   * contiguous array of four sem_t* (bs0, ss0, bs1, ss1), matching the
   * board's fixed ABI. */

  ret = boardctl(BOARDIOC_USER, (uintptr_t)t->sem);
  if (ret < 0)
    {
      LV_LOG_ERROR("boardctl(BOARDIOC_USER) TE sem register failed: %d", -ret);
      goto err_sems;
    }

  t->running = true;

  for (i = 0; i < NYABULA_TE_SEM_COUNT; i++)
    {
      ret = nyabula_te_create_thread_prio(&t->thread[i], te_thread_func,
                                          &t->ctx[i], NYABULA_TE_PRIORITY);
      if (ret != 0)
        {
          LV_LOG_ERROR("TE thread %d create failed: %d", i, ret);
          t->running = false;
          goto err_threads;
        }

      t->thread_started[i] = true;
    }

  return (nyabula_te_t *)t;

err_threads:
  /* Stop any started threads: post each sem so a blocked thread wakes and
   * observes running==false, then join.  Any unstarted thread slot stays
   * zero-initialized and is skipped. */

  for (i = 0; i < NYABULA_TE_SEM_COUNT; i++)
    {
      sem_post(&t->sem[i]);
    }

  for (i = 0; i < NYABULA_TE_SEM_COUNT; i++)
    {
      if (t->thread_started[i])
        {
          pthread_join(t->thread[i], NULL);
        }
    }

  boardctl(BOARDIOC_USER, 0); /* clear the board's sem registration */

err_sems:
  for (i = 0; i < NYABULA_TE_SEM_COUNT; i++)
    {
      if (t->sem_init_done[i])
        {
          sem_destroy(&t->sem[i]);
        }
    }

  lv_free(t);
  return NULL;
}

void nyabula_te_deinit(nyabula_te_t *te)
{
  struct nyabula_te_s *t = (struct nyabula_te_s *)te;
  int i;

  if (t == NULL)
    {
      return;
    }

  t->running = false;

  /* Clear the board's registration so the ISR stops posting (its sem
   * pointers are set to NULL; a NULL slot drops the edge). */

  boardctl(BOARDIOC_USER, 0);

  /* Wake every consumer thread so it observes running==false and exits. */

  for (i = 0; i < NYABULA_TE_SEM_COUNT; i++)
    {
      sem_post(&t->sem[i]);
    }

  for (i = 0; i < NYABULA_TE_SEM_COUNT; i++)
    {
      if (t->thread_started[i])
        {
          pthread_join(t->thread[i], NULL);
        }
    }

  for (i = 0; i < NYABULA_TE_SEM_COUNT; i++)
    {
      if (t->sem_init_done[i])
        {
          sem_destroy(&t->sem[i]);
        }
    }

  lv_free(t);
}
