/****************************************************************************
 * apps/graphics/nyabula_display/nyabula_dual_lcd.c
 *
 * Dual-screen dual-buffer LVGL interface layered on the "BlankGated" frame
 * scheduler (an in-house dual-panel scheduling algorithm; its C port lives
 * in nyabula_scheduler_blankgated.c).
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
 * Design overview
 *
 * Two panels (ST77916, 360x360 RGB565) share ONE QSPI write bus, so at any
 * instant only one screen can be written.  This layer is the FRAMEWORK:
 * it owns the threads, the LVGL integration and the DMA (PUTAREA) device
 * access, and passes all *scheduling decisions* to the BlankGated algorithm
 * (nyabula_scheduler_blankgated.c).  Framework and algorithm are fully
 * decoupled: the algorithm only consumes the four edge callbacks/requests
 * declared in nyabula_scheduler_blankgated.h and knows nothing about LVGL
 * or DMA.
 *
 * Thread model
 *   - Render loop    : logically a render thread, hosted by the caller
 *                   (main) via nyabula_dual_lcd_task(), which runs
 *                   lv_timer_handler() (LVGL is single-threaded).
 *   - TE source       : produces the scan-start / blank-start edges (see
 *                     nyabula_te.h).  Today a software frame clock thread
 *                     synthesizes them at the frame rate (screens staggered
 *                     90-deg); when the panel TE pins are wired it becomes
 *                     a GPIO interrupt source.  High priority for correct
 *                     timing.
 *   - Transfer thread: receives whole-frame write jobs from a single-slot
 *                     mailbox and issues a BLOCKING LCDDEVIO_PUTAREA ioctl
 *                     (QSPI DMA).  It does not consume CPU while the DMA is
 *                     in flight.
 *
 * TE signal / polarity / state machine
 *   The panel controller cannot produce a falling edge exactly at line 180
 *   (what the previous v8 scheduler required): it only outputs a TE level,
 *   HIGH during blanking and LOW during scanning.  The BlankGated algorithm
 *   therefore gates a whole-frame QSPI write on *entering blanking* (TE
 *   rising edge) and lets it cross into the active scan (catch-up scan),
 *   since the write (7ms) is faster than the scan (7.5ms).
 *
 *   - scan-start edge (TE falling, blanking ends) -> on_scan_start
 *   - blank-start edge (TE rising, blanking begins)-> on_blank_start
 *
 *   The edge callbacks (registered with the TE source) forward to the
 *   algorithm (which takes its own lock).  A GPIO TE ISR must NOT call them
 *   from interrupt context; the GPIO TE source defers to a thread (see
 *   nyabula_te_gpio.c).
 *
 * Framework <-> algorithm interface (see nyabula_scheduler_blankgated.h):
 *   framework -> algorithm : on_scan_start / on_blank_start / on_render_done
 *                            / on_xfer_done
 *   algorithm -> framework : request_render / request_write / on_buf_free
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "nyabula_dual_lcd.h"

#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <nuttx/lcd/lcd_dev.h>
#include <sched.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/sched_note.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/draw/sw/lv_draw_sw.h>

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Full-frame RGB565 offscreen double buffers for every screen, statically
 * allocated so their base addresses are 64B cache-line aligned AND fall in
 * the low 32-bit physical address space.  This lets the QSPI FSPI
 * lower-half run a direct DMA (zero bounce) on the whole-frame writes; a
 * heap allocation (lv_malloc) would only guarantee 8B alignment and so
 * would take the lower-half's bounce path on every frame.
 *
 * The arrays are dimensioned to the compile-time default resolution
 * (NYABULA_DUAL_LCD_FRAME_BYTES) because static storage has a fixed size;
 * screen_init() rejects any larger runtime geometry.  Layout is
 * [screen_id][buf_idx], matching scr->buf[buf_idx].data.
 *
 * Correctness precondition for the DMA-direct benefit (see the comment on
 * NYABULA_DUAL_LCD_FRAME_BYTES in nyabula_dual_lcd.h):
 *   - NuttX must be flat-mapped (VA == PA), so the aligned C array is also
 *     physically aligned;
 *   - the .bss section (this array) must link below 4 GiB, inside the DMA's
 *     32-bit range.
 * On this target (RK3576, CONFIG_RAM_START 0x40200000, flat mapping) both
 * hold.  If they do not, correctness is preserved (the FSPI bounce/polling
 * path kicks in) but the direct-DMA throughput gain is lost.
 */

static uint8_t g_nyabula_fb[NYABULA_DUAL_LCD_MAX_SCREENS][2]
                           [NYABULA_DUAL_LCD_FRAME_BYTES]
    __attribute__((aligned(64)));

/* Singleton guard: the statically allocated framebuffers (g_nyabula_fb) can
 * only back ONE live instance at a time.  Non-NULL while an instance
 * exists; nyabula_dual_lcd_create() refuses to build a second one and
 * nyabula_dual_lcd_destroy() clears it so the buffers can be reused. */
static nyabula_dual_lcd_t *g_dual_active = NULL;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int32_t align_round_up(int32_t v, uint16_t align);
static void rounder_cb(lv_event_t *e);

/* TE edge callbacks (implemented by this framework, registered with the TE
 * source via nyabula_te_init). */
static void te_scan_start_cb(nyabula_dual_lcd_t *dual, int sid);
static void te_blank_start_cb(nyabula_dual_lcd_t *dual, int sid);

/* BlankGated request callbacks (algorithm -> framework). */
static void sch_request_render(struct nyabula_screen *scr, int slot);
static bool sch_request_write(struct nyabula_screen *scr, int slot);
static void sch_on_buf_free(struct nyabula_screen *scr, int slot);
static void sch_render_skip(struct nyabula_screen *scr, int slot);

/* Single-slot xfer write-job mailbox (producer -> transfer thread). */
static int xfer_job_post(nyabula_dual_lcd_t *dual,
                         const nyabula_write_job_t *j);
static int xfer_job_take(nyabula_dual_lcd_t *dual, nyabula_write_job_t *out);

/* Threads */
static void *transfer_thread_func(void *arg);

/* Buffer / display helpers */
static void rounder_cb(lv_event_t *e);
static void te_refr_req_cb(lv_event_t *e);
static void flush_cb(lv_display_t *disp, const lv_area_t *area,
                     uint8_t *color_p);
static void flush_wait_cb(lv_display_t *disp);
static void display_release_cb(lv_event_t *e);
static int screen_display_on(nyabula_screen_t *scr);
static int screen_init(nyabula_screen_t *scr, int sid, const char *dev_path,
                       int width, int height);
static void screen_destroy(nyabula_screen_t *scr);

/****************************************************************************
 * Name: align_round_up
 *
 * Description:
 *   Round v up to the next multiple of align.
 *
 ****************************************************************************/

static int32_t align_round_up(int32_t v, uint16_t align)
{
  return (v + align - 1) & ~(align - 1);
}

/****************************************************************************
 * Name: rounder_cb
 *
 * Description:
 *   LVGL invalidation-area alignment, requested by the LCD upper half via
 *   LCDDEVIO_GETAREAALIGN.  Aligns dirty-area coordinates to the panel's
 *   transfer constraints.
 *
 ****************************************************************************/

static void rounder_cb(lv_event_t *e)
{
  nyabula_screen_t *scr = lv_event_get_user_data(e);
  lv_area_t *area;
  struct lcddev_area_align_s *align_info;
  int32_t w;
  int32_t h;

  /* Only align whole-screen flushes for the full-refresh pipeline. */
  if (lv_event_get_code(e) != LV_EVENT_INVALIDATE_AREA)
    {
      return;
    }

  area = lv_event_get_param(e);
  align_info = &scr->align;

  area->x1 &= ~(align_info->col_start_align - 1);
  area->y1 &= ~(align_info->row_start_align - 1);

  w = align_round_up(lv_area_get_width(area), align_info->width_align);
  h = align_round_up(lv_area_get_height(area), align_info->height_align);

  area->x2 = area->x1 + w - 1;
  area->y2 = area->y1 + h - 1;
}

/****************************************************************************
 * Name: te_refr_req_cb
 *
 * Description:
 *   LVGL event callback for LV_EVENT_REFR_REQUEST.  LVGL's built-in
 *   disp_event_cb resumes the default refresh timer on this event, which
 *   would make it auto-render outside the TE cadence.  That built-in
 *   handler is registered first, so our callback runs AFTER it; here we
 *   pause the refresh timer again to cancel that auto-resume.  Rendering is
 *   thereby driven exclusively by the TE scan-start edge via explicit
 *   lv_refr_now() (the timer handle is kept non-NULL so lv_refr_now works).
 *
 *   Note: lv_refr_now() -> _lv_display_refr_timer() re-pauses the timer
 *   itself after running one frame, so the timer never auto-runs.
 *
 ****************************************************************************/

static void te_refr_req_cb(lv_event_t *e)
{
  lv_display_t *disp;
  nyabula_screen_t *scr;
  lv_timer_t *tmr;

  if (lv_event_get_code(e) != LV_EVENT_REFR_REQUEST)
    {
      return;
    }

  disp = lv_event_get_current_target(e);
  if (disp == NULL)
    {
      return;
    }

  scr = lv_display_get_driver_data(disp);
  if (!scr || !scr->initialized)
    {
      return;
    }

  /* Cancel LVGL's auto-resume of the refresh timer (TE-exclusive render). */
  tmr = lv_display_get_refr_timer(disp);
  if (tmr != NULL)
    {
      lv_timer_pause(tmr);
    }
}

/****************************************************************************
 * request callbacks (algorithm -> framework)
 ****************************************************************************/

/* algorithm -> framework: request render of a new frame into offscreen
 * buffer `slot` (0/1, chosen by the algorithm).  Runs under the algorithm
 * lock (TE/transfer/render thread context): it records the selected slot and
 * posts a render request for the render loop.  The render loop redirects
 * LVGL to draw into `slot` (via lv_display_set_draw_buffers) before the
 * actual lv_refr_now() on the caller's thread in nyabula_dual_lcd_task(). */

static void sch_request_render(struct nyabula_screen *scr, int slot)
{
  if (!scr->initialized)
    {
      return;
    }

  /* Record which slot the algorithm wants LVGL to render into.  When
   * flush_cb fires, the framework reports on_render_done with this slot. */
  sem_wait(&scr->st_mutex);
  scr->pending_slot = slot;
  scr->render_request = true;
  sem_post(&scr->st_mutex);
}

/* algorithm -> framework: request a whole-frame write of buffer `slot`.
 * Runs under the algorithm lock; posts the write job to the single-slot
 * mailbox for the transfer thread (one blocking QSPI DMA for the entire
 * frame).  Always succeeds under single-flight (the slot cannot be full);
 * returns false only when the pipeline is shutting down, so the algorithm's
 * roll-back path stays reachable during teardown. */

static bool sch_request_write(struct nyabula_screen *scr, int slot)
{
  nyabula_dual_lcd_t *dual;
  nyabula_write_job_t job;

  dual = (nyabula_dual_lcd_t *)scr->dual;
  if (!dual || dual->quitting)
    {
      return false;
    }

  memset(&job, 0, sizeof(job));
  job.screen_id = scr->screen_id;
  job.buf_idx = slot;

  xfer_job_post(dual, &job);
  return true;
}

/* algorithm -> framework: a slot's whole-frame write is fully done in GRAM;
 * its offscreen buffer is free for LVGL to reuse.  Post buf_free so a blocked
 * flush_wait_cb (or the render defer wait) can proceed. */

static void sch_on_buf_free(struct nyabula_screen *scr, int slot)
{
  if (!scr->initialized)
    {
      return;
    }

  sem_wait(&scr->st_mutex);
  if (slot >= 0 && slot < 2)
    {
      scr->buf[slot].busy = false; /* buffer is now free */
    }
  sem_post(&scr->st_mutex);

  sem_post(&scr->buf_free);
}

/* algorithm -> framework: a requested render of `slot` was skipped by LVGL
 * (no invalid area, static frame).  The content did not change and GRAM still
 * shows the latest frame, so the slot is simply released -- the algorithm
 * clears rendering_slot without marking the slot readied for a write. */

static void sch_render_skip(struct nyabula_screen *scr, int slot)
{
  if (!scr->initialized)
    {
      return;
    }

  nyabula_sch_bg_on_render_skip(&scr->dual->sch, scr->screen_id, slot);
}

/* LVGL event handler: this display actually redrew a frame (it had at least
 * one invalid area) during the render loop's lv_refr_now().  This event is
 * only fired when inv_p != 0, so its absence right after lv_refr_now() on the
 * same thread means LVGL skipped the draw (static frame).  Same thread as the
 * render loop, so no locking is needed for the flag. */

static void render_ready_cb(lv_event_t *e)
{
  nyabula_screen_t *scr;

  if (lv_event_get_code(e) != LV_EVENT_RENDER_READY)
    {
      return;
    }

  scr = lv_display_get_driver_data(lv_event_get_current_target(e));
  if (scr != NULL)
    {
      scr->lvgl_rendered = true;
    }
}

/****************************************************************************
 * Name: te_scan_start_cb
 *
 * Description:
 *   TE edge callback (from the TE source): falling edge (HIGH -> LOW),
 *   blanking ended, a new frame scan begins.  Forwarded to the algorithm,
 *   which leaves the blanking window and asks for a new frame.  Also wakes
 *   the render loop so the render request is consumed promptly at the frame
 *   boundary.
 *
 ****************************************************************************/

static void te_scan_start_cb(nyabula_dual_lcd_t *dual, int sid)
{
  nyabula_screen_t *scr;

  if (dual->quitting)
    {
      return;
    }

  scr = &dual->screen[sid];
  if (!scr->initialized)
    {
      return;
    }

  nyabula_sch_bg_on_scan_start(&dual->sch, sid);

  /* Wake the render loop once per frame so the pending render request
   * (posted by sch_request_render via the algorithm) is consumed promptly at
   * the frame boundary. */
  sem_post(&dual->render_kick);
}

/****************************************************************************
 * Name: te_blank_start_cb
 *
 * Description:
 *   TE edge callback (from the TE source): rising edge (LOW -> HIGH),
 *   scanning finished, blanking begins.  This is the ONLY gate at which the
 *   algorithm may start a whole-frame QSPI write (the write may then cross
 *   into the active scan).
 *
 ****************************************************************************/

static void te_blank_start_cb(nyabula_dual_lcd_t *dual, int sid)
{
  nyabula_screen_t *scr;

  if (dual->quitting)
    {
      return;
    }

  scr = &dual->screen[sid];
  if (!scr->initialized)
    {
      return;
    }

  nyabula_sch_bg_on_blank_start(&dual->sch, sid);
}

/****************************************************************************
 * Single-slot write-job mailbox (bounded by single-flight, not a FIFO).
 *
 * The BlankGated algorithm is strictly single-flight (xfer_busy), so at any
 * instant at most ONE whole-frame write is waiting for the transfer thread
 * plus the one in flight on the shared bus.  A multi-slot FIFO (as once
 * existed here) could never hold more than one element -- it mirrored the
 * simulator's discrete-event queue, which is not part of the reference
 * scheduler (vsyncalg_plusplus/schedulers.py BlankGatedScheduler).  Per that
 * reference the framework needs only ONE slot and a counting semaphore to
 * hand the write job off to the transfer thread and park it when idle.
 *
 * Crossing threads:
 *   - producer  : TE/scheduler thread -> request_write -> xfer_job_post
 *   - consumer  : transfer thread     -> xfer_job_take -> blocking PUTAREA
 *
 * The slot crossover is safe under single-flight: the producer only ever
 * posts a NEW job after the previous whole-frame write's xfer_done, by which
 * time the consumer has already copied the prior job into its local variable
 * and is DMA-ing it.  job_mutex (binary) still guards the single slot so the
 * handoff stays correct even if single-flight is ever relaxed; job_avail
 * (counting 0/1) parks/unparks the consumer without burning CPU.
 ****************************************************************************/

static int xfer_job_post(nyabula_dual_lcd_t *dual,
                         const nyabula_write_job_t *j)
{
  sem_wait(&dual->job_mutex);
  dual->job_slot = *j;
  sem_post(&dual->job_mutex);

  /* Wake the transfer thread: one pending xfer job.  Under single-flight
   * this never overflows (the producer can't post a second job while the
   * first is still in the slot), so job_avail stays 0/1. */
  sem_post(&dual->job_avail);

  return 0;
}

static int xfer_job_take(nyabula_dual_lcd_t *dual, nyabula_write_job_t *out)
{
  /* Block (CPU-free) until a xfer job is posted; consuming it clears the
   * 0/1 count so the slot may be safely overwritten by the next post. */
  sem_wait(&dual->job_avail);
  sem_wait(&dual->job_mutex);
  *out = dual->job_slot;
  sem_post(&dual->job_mutex);

  return 0;
}

/****************************************************************************
 * Name: nyabula_dual_lcd_task
 *
 * Description:
 *   Run one step of the render loop from the CALLER's own thread (e.g. the
 *   main thread).  Each call:
 *     1) advances LVGL via lv_timer_handler() (timers, animation, refresh),
 *     2) consumes any pending TE-driven render requests: invalidates the
 *        screen's active object once so LVGL renders exactly one full frame
 *        (FULL mode) to feed the BlankGated pipeline.
 *
 *   The display layer starts the TE/transfer threads in create(); the
 *   caller hosts the lv_timer_handler() call (LVGL is single-threaded, and
 *   running it in main avoids an extra thread).  The UI must be built
 *   before the first call.
 *
 *   Animation is unaffected: LVGL's animation timer and the default
 *   refresh re-arm path are untouched; we only add one extra full-screen
 *   invalidate per TE scan-start edge.
 *
 *   Blocks up to the LVGL idle period when idle (wakes early on a render
 *   kick), then returns so the caller can drive the loop.
 *
 ****************************************************************************/

void nyabula_dual_lcd_task(nyabula_dual_lcd_t *dual)
{
  struct timespec ts;
  uint32_t idle;
  int sid;

  if (!dual)
    {
      return;
    }

  /* Run one cycle of the LVGL state machine (timers, animation).  This
   * advances animations (time from the NuttX clock via lv_tick_set_cb) and
   * accumulates their invalidations; the default refresh timer is kept
   * paused (see te_refr_req_cb), so it does NOT itself render.  The render
   * happens below via lv_refr_now() driven by the algorithm's render
   * request, keeping the pipeline TE-aligned. */
  idle = lv_timer_handler();

  /* Consume TE-driven render requests: for each screen whose algorithm
   * requested a frame, redirect LVGL to the algorithm-chosen buffer slot and
   * issue lv_refr_now().  lv_refr_now() advances animations too and skips
   * drawing when there is no invalid area.  The double-buffer gating and slot
   * selection are entirely the algorithm's job (can_render / _free_slot), so
   * the render loop never decides deferral or picks a buffer here. */
  for (sid = 0; sid < NYABULA_DUAL_LCD_MAX_SCREENS; sid++)
    {
      nyabula_screen_t *scr = &dual->screen[sid];
      bool request;
      int slot;

      if (!scr->initialized || !scr->disp)
        {
          continue;
        }

      sem_wait(&scr->st_mutex);
      request = scr->render_request;
      slot = scr->pending_slot;
      if (request)
        {
          scr->render_request = false;
          scr->pending_slot = -1;
        }

      sem_post(&scr->st_mutex);

      if (request && slot >= 0 && slot < 2)
        {
          /* Point LVGL's active draw buffer at the algorithm-selected slot:
           * the first argument of set_draw_buffers becomes buf_act. */
          if (slot == 0)
            {
              lv_display_set_draw_buffers(scr->disp, &scr->draw_buf[0],
                                          &scr->draw_buf[1]);
            }
          else
            {
              lv_display_set_draw_buffers(scr->disp, &scr->draw_buf[1],
                                          &scr->draw_buf[0]);
            }

          /* Clear the "rendered" flag, then run one synchronous LVGL frame.
           * LV_EVENT_RENDER_READY only fires when the display actually
           * redrew (inv_p != 0); if the flag is still false afterwards LVGL
           * had no invalid area and skipped the draw (static frame).  In
           * that case there is no flush_cb -> on_render_done, so the
           * requested rendering_slot must be released explicitly via
           * on_render_skip (which does NOT mark the slot ready -- GRAM
           * already holds the latest frame, so no write is issued).  This
           * both prevents the rendering_slot leak that froze one screen and
           * lets static frames avoid needless redraws/writes. */
          scr->lvgl_rendered = false;
          lv_refr_now(scr->disp);
          if (!scr->lvgl_rendered)
            {
              sch_render_skip(scr, slot);
            }
        }
    }

  /* Wait for either a TE edge kick (posted by the TE scan-start callback so
   * we service lv_timer_handler() promptly at the frame boundary) or until
   * the LVGL idle period elapses -- whichever comes first.  The timeout is
   * computed relative to NOW so the wait never extends beyond `idle` ms from
   * this moment, keeping the LVGL clock true. */
  if (idle == 0)
    {
      idle = 1;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += (long)idle * 1000000L;
  if (ts.tv_nsec >= 1000000000L)
    {
      ts.tv_sec += ts.tv_nsec / 1000000000L;
      ts.tv_nsec %= 1000000000L;
    }

  sem_timedwait(&dual->render_kick, &ts);
}

/****************************************************************************
 * Name: transfer_thread_func
 *
 * Description:
 *   Transfer thread.  Waits on the single-slot write mailbox (job_avail) and
 *   issues a BLOCKING LCDDEVIO_PUTAREA ioctl (QSPI DMA) for each whole-frame
 *   job -- the call blocks until the DMA completes but does not consume CPU
 *   meanwhile.  Fairness between screens is the ALGORITHM's job at submission
 *   time (it picks which screen's ready slot to write when the bus is free);
 *   the framework's mailbox is a strict handoff, not a reorder point.
 *
 *   After each whole-frame write completes, the edge is reported to the
 *   BlankGated algorithm (nyabula_sch_bg_on_xfer_done), which releases the
 *   single-flight bus and frees the slot's double buffer.
 *
 ****************************************************************************/

static void *transfer_thread_func(void *arg)
{
  nyabula_dual_lcd_t *dual = (nyabula_dual_lcd_t *)arg;
  nyabula_screen_t *scr;
  struct lcddev_area_s lcd_area;
  nyabula_write_job_t job;
  int ret;

  memset(&lcd_area, 0, sizeof(lcd_area));

  while (dual->running)
    {
      xfer_job_take(dual, &job);

      if (!dual->running)
        {
          break;
        }

      scr = &dual->screen[job.screen_id];

      /* Whole frame: rows 0..total_lines-1, write pointer runs in the same
       * direction as the panel scan (top of GRAM first), which is what the
       * catch-up-scan guarantee requires. */
      lcd_area.row_start = 0;
      lcd_area.row_end = scr->total_lines - 1;
      lcd_area.col_start = 0;
      lcd_area.col_end = scr->width - 1;
      lcd_area.stride = scr->stride;
      lcd_area.data = scr->buf[job.buf_idx].data;

      /* Mark begin/end of the blocking PUTAREA ioctl so the ~ms transfer
       * interval is visible in the trace dump.  ioctl()'s body lives in the
       * non-instrumented kernel fs_ioctl.c, so the automatic function
       * tracing (-finstrument-functions) cannot see it; these explicit
       * sched_note markers give us a named B/E span for the DMA wait. */
      sched_note_beginex(NOTE_TAG_ALWAYS, "nyabula:putarea");

      ret = ioctl(scr->fd, LCDDEVIO_PUTAREA, (unsigned long)&lcd_area);

      sched_note_endex(NOTE_TAG_ALWAYS, "nyabula:putarea");

      if (ret < 0)
        {
          /* Log only on failure (does not spam the log). */
          lcdinfo("screen %d PUTAREA rows %u-%u failed: %d\n", job.screen_id,
                  lcd_area.row_start, lcd_area.row_end, -ret);
        }

      /* Whole-frame transfer complete: report the edge to the BlankGated
       * algorithm, which releases the single-flight bus and (because a slot
       * is complete exactly when its single whole-frame write ends) frees the
       * double buffer.  The DMA hardware gives us exactly one interrupt per
       * submitted write, which is the physical signal the algorithm keys its
       * bookkeeping on. */
      nyabula_sch_bg_on_xfer_done(&dual->sch, job.screen_id, job.buf_idx);
    }

  return NULL;
}

/****************************************************************************
 * Name: flush_wait_cb
 *
 * Description:
 *   LVGL flush-wait callback, replacing LVGL's default busy `while(flushing)`
 *   spin with a blocking-but-CPU-free wait.
 *
 *   A buffer is "free" once its slot has been fully written to GRAM (busy
 *   set back to false by on_buf_free, which posts buf_free).  Wait here while
 *   BOTH offscreen buffers are still occupied (busy), i.e. a new draw would
 *   overwrite content whose DMA transfer has not finished.  Once at least one
 *   buffer is free, return immediately.
 *
 *   The BlankGated algorithm already guarantees it only requests a render
 *   when a working buffer is available (can_render), so under a healthy bus
 *   this rarely blocks; it is a defensive net for LVGL's internal buffer
 *   alternation racing a slow whole-frame DMA.
 *
 ****************************************************************************/

static void flush_wait_cb(lv_display_t *disp)
{
  nyabula_screen_t *scr = lv_display_get_driver_data(disp);
  bool both_busy;

  if (!scr || !scr->initialized)
    {
      return;
    }

  sem_wait(&scr->st_mutex);
  both_busy = (scr->buf[0].busy && scr->buf[1].busy);
  while (both_busy && scr->initialized)
    {
      /* Both offscreen buffers hold rendered-but-unwritten content: a new
       * draw would need a third buffer.  Wait (signalled) for the transfer
       * thread to finish a slot and post buf_free. */
      sem_post(&scr->st_mutex);
      sem_wait(&scr->buf_free);
      sem_wait(&scr->st_mutex);
      both_busy = (scr->buf[0].busy && scr->buf[1].busy);
    }

  sem_post(&scr->st_mutex);
}

/****************************************************************************
 * Name: flush_cb
 *
 * Description:
 *   LVGL flush callback, called from the render thread when LVGL finishes
 *   rendering a full frame into the buffer the algorithm directed it to via
 *   request_render (pending_slot).  Reports on_render_done(slot) so the
 *   algorithm moves that slot into its ready (awaiting-write) set.
 *
 *   A full double-buffer wait is handled by flush_wait_cb() above in LVGL's
 *   own wait slot, so this callback never blocks the render thread itself.
 *
 ****************************************************************************/

static void flush_cb(lv_display_t *disp, const lv_area_t *area,
                     uint8_t *color_p)
{
  nyabula_screen_t *scr = lv_display_get_driver_data(disp);
  int buf_idx;

  (void)area; /* Full-refresh: LVGL flushes the whole screen each frame. */

  buf_idx = (color_p == scr->buf[0].data) ? 0 : 1;

  /* Byte-swap the rendered RGB565 frame in place to the big-endian byte
   * order the panel GRAM expects (the st77916 driver no longer does this).
   * color_p is scr->buf[buf_idx].data, the same buffer the transfer thread
   * later DMAs, and LVGL has just finished rendering it synchronously, so
   * it is safe to touch here.  This runs on the render thread, overlapping
   * with the transfer thread's DMA of the other screen -- swapping at flush
   * time (rather than in the transfer path) keeps the CPU work off the
   * shared-bus critical path.  The buffer is not reused by LVGL until its
   * slot is fully written to GRAM (busy reset to false in on_buf_free), so
   * the swapped byte order survives until the DMA reads it.
   */
  lv_draw_sw_rgb565_swap(scr->buf[buf_idx].data, scr->width * scr->height);

  sem_wait(&scr->st_mutex);

  /* Mark the slot occupied (rendered content awaiting write).  The algorithm
   * is told this slot is now ready. */
  if (scr->initialized)
    {
      scr->buf[buf_idx].busy = true;
    }

  sem_post(&scr->st_mutex);

  /* Content is ready: the algorithm moves buf_idx from "rendering" to
   * "ready".  Called outside st_mutex (the algorithm takes its own lock).
   */
  nyabula_sch_bg_on_render_done(&scr->dual->sch, scr->screen_id, buf_idx);

  /* Release this buffer so LVGL resumes rendering into the other one. */
  lv_display_flush_ready(disp);
}

/****************************************************************************
 * Name: display_release_cb
 *
 * Description:
 *   Called when the LVGL display is deleted; closes the LCD fd and detaches
 *   the flush callback.
 *
 ****************************************************************************/

static void display_release_cb(lv_event_t *e)
{
  lv_display_t *disp;
  nyabula_screen_t *scr;

  disp = (lv_display_t *)lv_event_get_user_data(e);
  scr = lv_display_get_driver_data(disp);

  if (scr)
    {
      lv_display_set_driver_data(disp, NULL);
      lv_display_set_flush_cb(disp, NULL);

      /* Symmetric power-down: we powered the panel on via SETPOWER in
       * screen_display_on(), so turn it off before the fd is closed. */
      if (scr->fd >= 0)
        {
          ioctl(scr->fd, LCDDEVIO_SETPOWER, (unsigned long)0);
        }

      if (scr->fd >= 0)
        {
          close(scr->fd);
          scr->fd = -1;
        }

      LV_LOG_USER("Screen released");
    }
}

/****************************************************************************
 * Name: screen_display_on
 *
 * Description:
 *   Before the LVGL pipeline has rendered anything, the panel GRAM still
 *   holds whatever garbage it was powered up with.  Powering the panel on
 *   at that point would flash uninitialized content (snow) until the first
 *   frame is written.  To avoid that, this helper writes one full black
 *   frame to GRAM with a blocking LCDDEVIO_PUTAREA, then turns the panel on
 *   with LCDDEVIO_SETPOWER so the panel comes up to stable black instead of
 *   garbage.  This deliberately does not depend on the TE/vsync scheduler:
 *   a plain direct GRAM fill is always valid before any live rendering.
 *
 * Input Parameters:
 *   scr - The screen to pre-fill and power on.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.  On failure the
 *   panel is left unpowered (or partially filled), so it may show startup
 *   noise until a real frame is rendered.
 *
 ****************************************************************************/

static int screen_display_on(nyabula_screen_t *scr)
{
  struct lcddev_area_s area;
  uint8_t *black;
  int ret;

  /* Allocate a temporary full-frame black buffer.  It is plain malloc'd
   * memory: st77916_putarea() stages it into its own DMA-safe buffer, so
   * no special alignment is required here. */
  black = lv_malloc(scr->buf_size);
  if (black == NULL)
    {
      LV_LOG_ERROR("Screen %d: failed to allocate black fill buffer",
                   scr->screen_id);
      return -ENOMEM;
    }

  memset(black, 0, scr->buf_size);

  /* Fill the whole GRAM with black. */
  memset(&area, 0, sizeof(area));
  area.row_start = 0;
  area.row_end = scr->height - 1;
  area.col_start = 0;
  area.col_end = scr->width - 1;
  area.stride = scr->stride;
  area.data = black;

  ret = ioctl(scr->fd, LCDDEVIO_PUTAREA, (unsigned long)&area);
  if (ret < 0)
    {
      LV_LOG_ERROR("Screen %d: black fill PUTAREA failed: %d", scr->screen_id,
                   -ret);
      lv_free(black);
      return ret;
    }

  lv_free(black);

  /* Turn the panel on now that its GRAM holds stable black. */
  ret = ioctl(scr->fd, LCDDEVIO_SETPOWER, LCD_FULL_ON);
  if (ret < 0)
    {
      LV_LOG_ERROR("Screen %d: SETPOWER failed: %d", scr->screen_id, -ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: screen_init
 *
 * Description:
 *   Initialize a single screen: open the LCD device, allocate two full-frame
 *   RGB565 buffers, set up the LVGL display.  The BlankGated algorithm
 *   context is initialized (with callbacks) by the caller after the dual
 *   context is built, since it needs the dual back-pointer.
 *
 ****************************************************************************/

static int screen_init(nyabula_screen_t *scr, int sid, const char *dev_path,
                       int width, int height)
{
  int ret;
  int xerr;

  scr->screen_id = sid;
  scr->width = width;
  scr->height = height;
  scr->stride = width * 2; /* RGB565: 2 bytes per pixel */
  scr->total_lines = height;
  scr->buf_size = (uint32_t)width * height * 2;
  scr->pending_slot = -1;
  scr->render_request = false;
  scr->dual = NULL; /* set by the caller (nyabula_dual_lcd_create) */
  scr->initialized = false;

  /* Open LCD device */
  LV_LOG_USER("Opening LCD device: %s", dev_path);
  scr->fd = open(dev_path, O_RDWR);
  if (scr->fd < 0)
    {
      xerr = -errno;
      LV_LOG_ERROR("Failed to open %s: %d", dev_path, -xerr);
      return xerr;
    }

  /* Get alignment info */
  ret = ioctl(scr->fd, LCDDEVIO_GETAREAALIGN, &scr->align);
  if (ret < 0)
    {
      LV_LOG_WARN("ioctl(GETAREAALIGN) failed, using defaults");
      scr->align.col_start_align = 1;
      scr->align.row_start_align = 1;
      scr->align.width_align = 1;
      scr->align.height_align = 1;
    }

  /* The offscreen double buffers are statically allocated (g_nyabula_fb)
   * so their addresses are 64B-aligned and in low memory for DMA-direct
   * whole-frame writes.  The static array is sized to the compile-time
   * default resolution, so reject any larger runtime geometry here. */
  if (scr->buf_size > NYABULA_DUAL_LCD_FRAME_BYTES)
    {
      LV_LOG_ERROR("Screen %d: %ux%u (%u B) exceeds static framebuffer "
                   "%u B; use the default resolution or bump "
                   "NYABULA_DUAL_LCD_DEF_WIDTH/HEIGHT",
                   sid, width, height, scr->buf_size,
                   (unsigned)NYABULA_DUAL_LCD_FRAME_BYTES);
      xerr = -ENOMEM;
      goto err_fd;
    }

  scr->buf[0].data = g_nyabula_fb[sid][0];
  scr->buf[0].busy = false;
  memset(scr->buf[0].data, 0, scr->buf_size);

  scr->buf[1].data = g_nyabula_fb[sid][1];
  scr->buf[1].busy = false;
  memset(scr->buf[1].data, 0, scr->buf_size);

  /* Initialize sync primitives */
  sem_init(&scr->buf_free, 0, 0);
  sem_init(&scr->st_mutex, 0, 1);

  /* Create LVGL display with full-refresh double buffering. */
  scr->disp = lv_display_create(width, height);
  if (!scr->disp)
    {
      LV_LOG_ERROR("Failed to create LVGL display");
      xerr = -ENOMEM;
      goto err_disp;
    }

  /* LVGL renders one full frame per buffer and calls flush_cb once for the
   * whole screen; this matches the BlankGated whole-frame write model. */
  lv_display_set_buffers(scr->disp, scr->buf[0].data, scr->buf[1].data,
                         scr->buf_size, LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(scr->disp, flush_cb);
  lv_display_set_flush_wait_cb(scr->disp, flush_wait_cb);
  lv_display_set_driver_data(scr->disp, scr);

  /* Build two LVGL draw-buffer descriptors bound to the same offscreen data
   * buffers.  The algorithm owns slot selection: request_render redirects
   * LVGL to the chosen slot by calling lv_display_set_draw_buffers with that
   * slot's descriptor first (the active draw buffer becomes buf_1).  Use the
   * display's own color format and our stride so the descriptors match what
   * set_buffers set up. */
  {
    lv_color_format_t cf = lv_display_get_color_format(scr->disp);
    lv_draw_buf_init(&scr->draw_buf[0], width, height, cf, scr->stride,
                     scr->buf[0].data, scr->buf_size);
    lv_draw_buf_init(&scr->draw_buf[1], width, height, cf, scr->stride,
                     scr->buf[1].data, scr->buf_size);
  }

  /* Retain the default refresh timer: lv_refr_now() needs a non-NULL
   * disp->refr_timer handle to drive a frame (it guards on
   * `if(disp->refr_timer)`).  But we neutralize its auto-running via
   * te_refr_req_cb(), which pauses it on every REFR_REQUEST (after LVGL's
   * built-in handler has resumed it), so rendering is driven exclusively
   * by the TE scan-start edge through explicit lv_refr_now(). */
  lv_display_add_event_cb(scr->disp, rounder_cb, LV_EVENT_INVALIDATE_AREA,
                          scr);
  lv_display_add_event_cb(scr->disp, te_refr_req_cb, LV_EVENT_REFR_REQUEST,
                          scr);
  lv_display_add_event_cb(scr->disp, render_ready_cb, LV_EVENT_RENDER_READY,
                          scr);
  lv_display_add_event_cb(scr->disp, display_release_cb, LV_EVENT_DELETE,
                          scr->disp);

  scr->initialized = true;
  LV_LOG_USER("Screen initialized: %dx%d, stride=%d, buf_size=%u", width,
              height, scr->stride, scr->buf_size);

  /* Pre-fill GRAM with black and power the panel on.  This happens before
   * any live rendering so the panel never shows uninitialized GRAM garbage;
   * the TEM/vsync pipeline then refreshes real frames on top. */
  ret = screen_display_on(scr);
  if (ret < 0)
    {
      LV_LOG_WARN("Screen %d display-on incomplete: %d", sid, -ret);
      /* Non-fatal: rendering still works, the panel just may have shown
       * startup noise before the first real frame. */
    }

  return 0;

err_disp:
  sem_destroy(&scr->buf_free);
  sem_destroy(&scr->st_mutex);
err_fd:
  close(scr->fd);
  scr->fd = -1;
  return xerr;
}

/****************************************************************************
 * Name: screen_destroy
 *
 * Description:
 *   Tear down a single screen and release all resources.
 *
 ****************************************************************************/

static void screen_destroy(nyabula_screen_t *scr)
{
  if (!scr->initialized)
    {
      return;
    }

  if (scr->disp)
    {
      lv_display_delete(scr->disp);
      scr->disp = NULL;
    }

  /* Symmetric power-down (idempotent: display_release_cb may already have
   * turned the panel off and closed the fd on lv_display_delete above). */
  if (scr->fd >= 0)
    {
      ioctl(scr->fd, LCDDEVIO_SETPOWER, (unsigned long)0);
    }

  /* The double buffers are statically allocated (g_nyabula_fb), so there
   * is nothing to free; just mark them invalid and reset the pointers. */
  scr->buf[0].data = NULL;
  scr->buf[0].busy = false;
  scr->buf[1].data = NULL;
  scr->buf[1].busy = false;

  if (scr->fd >= 0)
    {
      close(scr->fd);
      scr->fd = -1;
    }

  sem_destroy(&scr->buf_free);
  sem_destroy(&scr->st_mutex);

  scr->initialized = false;
  LV_LOG_USER("Screen destroyed");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nyabula_dual_lcd_create
 *
 * Description:
 *   Create the dual-LCD context with the two threads (software TE frame
 *   clock + transfer) and wire the BlankGated scheduling algorithm by
 *   registering its request callbacks.
 *
 ****************************************************************************/

nyabula_dual_lcd_t *nyabula_dual_lcd_create(const char *dev_path0,
                                            const char *dev_path1, int width,
                                            int height)
{
  nyabula_dual_lcd_t *dual;
  nyabula_sch_bg_callbacks_t sch_cb;
  int sid;
  int ret;

  LV_ASSERT_NULL(dev_path0);
  LV_ASSERT_NULL(dev_path1);

  /* Singleton guard: the static framebuffers can only back one live
   * instance.  Refuse to build a second one; the caller must destroy the
   * current instance first (see the module contract in nyabula_dual_lcd.h).
   */
  if (g_dual_active != NULL)
    {
      LV_LOG_ERROR("nyabula_dual_lcd is a singleton: destroy the current "
                   "instance before creating another");
      return NULL;
    }

  if (width <= 0)
    {
      width = NYABULA_DUAL_LCD_DEF_WIDTH;
    }

  if (height <= 0)
    {
      height = NYABULA_DUAL_LCD_DEF_HEIGHT;
    }

  dual = lv_malloc_zeroed(sizeof(nyabula_dual_lcd_t));
  if (!dual)
    {
      LV_LOG_ERROR("Failed to allocate dual LCD context");
      return NULL;
    }

  /* Claim the singleton before any further (fallible) setup: on failure
   * below the error paths release it again. */
  g_dual_active = dual;

  dual->running = false;
  dual->te = NULL;

  sem_init(&dual->job_mutex, 0, 1);
  sem_init(&dual->job_avail, 0, 0);
  sem_init(&dual->render_kick, 0, 0);

  /* Initialize screen 0 */
  ret = screen_init(&dual->screen[0], 0, dev_path0, width, height);
  if (ret < 0)
    {
      LV_LOG_ERROR("Failed to initialize screen 0: %d", -ret);
      goto err_screen0;
    }

  /* Initialize screen 1 */
  ret = screen_init(&dual->screen[1], 1, dev_path1, width, height);
  if (ret < 0)
    {
      LV_LOG_ERROR("Failed to initialize screen 1: %d", -ret);
      goto err_screen1;
    }

  /* Wire the BlankGated algorithm: register its request callbacks and set
   * the screen back-pointers so its write request can reach the transfer
   * mailbox.  The algorithm only ever sees the opaque screen handles and these
   * callbacks -- no LVGL/DMA detail. */
  sch_cb.request_render = sch_request_render;
  sch_cb.request_write = sch_request_write;
  sch_cb.on_buf_free = sch_on_buf_free;
  sch_cb.on_render_skip = sch_render_skip;

  nyabula_sch_bg_init(&dual->sch, &sch_cb);

  for (sid = 0; sid < NYABULA_DUAL_LCD_MAX_SCREENS; sid++)
    {
      dual->screen[sid].dual = dual;
      dual->sch.screen[sid] = &dual->screen[sid];
    }

  dual->running = true;

  /* Start the TE edge source (software frame clock now, panel TE GPIO
   * interrupt later; selected by CONFIG_NYABULA_DISPLAY_TE_SOURCE).  It
   * calls te_scan_start_cb / te_blank_start_cb on the edges. */
  {
    nyabula_te_callbacks_t te_cb;

    te_cb.scan_start = te_scan_start_cb;
    te_cb.blank_start = te_blank_start_cb;
    dual->te = nyabula_te_init(dual, &te_cb);
  }

  if (dual->te == NULL)
    {
      LV_LOG_ERROR("Failed to start TE source");
      dual->running = false;
      goto err_te_thread;
    }

  /* Start the transfer (QSPI DMA) thread.  It blocks on the single-slot
   * mailbox while idle, so it needs no special priority (the TE source
   * still runs its own higher-priority thread -- see nyabula_te_*.c). */
  ret =
      pthread_create(&dual->transfer_thread, NULL, transfer_thread_func, dual);
  if (ret != 0)
    {
      LV_LOG_ERROR("Failed to create transfer thread: %d", ret);
      dual->running = false;
      nyabula_te_deinit(dual->te);
      dual->te = NULL;
      goto err_te_thread;
    }

  LV_LOG_USER("Dual LCD created successfully (BlankGated scheduler, %d fps); "
              "run nyabula_dual_lcd_task() in a loop to drive rendering",
              NYABULA_DUAL_LCD_REFRESH_HZ);
  return dual;

err_te_thread:
  screen_destroy(&dual->screen[0]);
  screen_destroy(&dual->screen[1]);
  /* fall through */

err_screen1:
  screen_destroy(&dual->screen[0]);
  /* fall through */

err_screen0:
  sem_destroy(&dual->job_mutex);
  sem_destroy(&dual->job_avail);
  sem_destroy(&dual->render_kick);
  lv_free(dual);
  g_dual_active = NULL; /* release the singleton on failure so it can retry */
  return NULL;
}

/****************************************************************************
 * Name: nyabula_dual_lcd_destroy
 *
 * Description:
 *   Stop all threads and release resources.
 *
 ****************************************************************************/

void nyabula_dual_lcd_destroy(nyabula_dual_lcd_t *dual)
{
  if (!dual)
    {
      return;
    }

  /* Stop the threads. */
  dual->running = false;

  /* Wake the transfer thread (blocked on job_avail).  The render loop is
   * hosted by the caller, not joined here. */
  sem_post(&dual->render_kick);
  sem_post(&dual->job_avail);

  /* Stop the TE edge source (its thread exits on running=false). */
  nyabula_te_deinit(dual->te);
  dual->te = NULL;

  pthread_join(dual->transfer_thread, NULL);

  screen_destroy(&dual->screen[0]);
  screen_destroy(&dual->screen[1]);

  sem_destroy(&dual->job_mutex);
  sem_destroy(&dual->job_avail);
  sem_destroy(&dual->render_kick);

  lv_free(dual);

  /* Release the singleton claim so a new instance can be created again. */
  if (g_dual_active == dual)
    {
      g_dual_active = NULL;
    }

  LV_LOG_USER("Dual LCD destroyed");
}

/****************************************************************************
 * Name: nyabula_dual_lcd_get_display
 *
 * Description:
 *   Get the LVGL display instance for a specific screen.
 *
 ****************************************************************************/

lv_display_t *nyabula_dual_lcd_get_display(nyabula_dual_lcd_t *dual,
                                           int screen_id)
{
  if (!dual || screen_id < 0 || screen_id >= NYABULA_DUAL_LCD_MAX_SCREENS)
    {
      LV_LOG_ERROR("Invalid dual LCD context or screen ID: %d", screen_id);
      return NULL;
    }

  if (!dual->screen[screen_id].initialized)
    {
      LV_LOG_ERROR("Screen %d not initialized", screen_id);
      return NULL;
    }

  return dual->screen[screen_id].disp;
}
