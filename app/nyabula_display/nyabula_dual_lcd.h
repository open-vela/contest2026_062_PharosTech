/****************************************************************************
 * apps/graphics/nyabula_display/nyabula_dual_lcd.h
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
 * SINGLE-INSTANCE (singleton) contract
 *
 * This module is explicitly a single-instance design.  The per-screen
 * full-frame double buffers are statically allocated (g_nyabula_fb in
 * nyabula_dual_lcd.c) so their base addresses are 64B-aligned and in low
 * 32-bit memory, enabling DMA-direct whole-frame writes to the QSPI FSPI
 * lower-half without a bounce buffer.  A static buffer is inherently
 * unique process-wide, so only ONE nyabula_dual_lcd_create() may be
 * outstanding at a time.
 *
 * Consequence:
 *   - Calling nyabula_dual_lcd_create() while an instance already exists
 *     (and has not been destroyed) FAILS and returns NULL.
 *   - After nyabula_dual_lcd_destroy(), a new instance may be created
 *     again (the static buffers are reused by the new instance).
 *
 * If a true multi-instance design were ever required, the static buffers
 * would have to be moved to per-instance dynamic allocation, which would
 * sacrifice the DMA-direct property (dynamic heap only guarantees 8B
 * alignment / arbitrary placement) -- a deliberate trade-off this module
 * does not make.
 ****************************************************************************/

#ifndef __NYABULA_DUAL_LCD_H
#define __NYABULA_DUAL_LCD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <nuttx/lcd/lcd_dev.h>
#include <pthread.h>
#include <semaphore.h>

/* BlankGated algorithm layer: framework and scheduler are decoupled; the
 * algorithm issues render/write requests via the registered callbacks. */
#include "nyabula_scheduler_blankgated.h"

/* TE edge source abstraction (software frame clock now, panel TE GPIO
 * interrupt later; selected by CONFIG_NYABULA_DISPLAY_TE_SOURCE). */
#include "nyabula_te.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Maximum number of screens sharing the single QSPI write bus. */

#define NYABULA_DUAL_LCD_MAX_SCREENS 2

/* Default screen geometry (ST77916, 360x360 RGB565). */

#ifndef NYABULA_DUAL_LCD_DEF_WIDTH
#define NYABULA_DUAL_LCD_DEF_WIDTH 360
#endif

#ifndef NYABULA_DUAL_LCD_DEF_HEIGHT
#define NYABULA_DUAL_LCD_DEF_HEIGHT 360
#endif

/* Compile-time size of one full-frame RGB565 buffer.  The two offscreen
 * double buffers per screen are statically allocated (see
 * nyabula_dual_lcd.c) so their base addresses can be 64B cache-line aligned
 * and physically contiguous, letting the QSPI FSPI lower-half use a direct
 * DMA transfer with zero bounce.
 *
 * NOTE: this DMA-friendly static allocation is only correct when BOTH of
 * these hold on the target (checked/commented at the point of use):
 *   1) Flat-mapping: the kernel runs in a single address space where the
 *      virtual address equals the physical address, so a 64B-aligned C
 *      array is also physically 64B-aligned for the DMA engine.
 *   2) The .bss (and hence this array) links below 4 GiB, inside the DMA
 *      engine's 32-bit SAR/DAR addressable range.
 * If either is violated the buffers fall back to the lower-half's bounce /
 * polling path and the DMA-direct benefit is lost (correctness is NOT
 * affected, only throughput).
 */

#define NYABULA_DUAL_LCD_FRAME_BYTES \
  (NYABULA_DUAL_LCD_DEF_WIDTH * NYABULA_DUAL_LCD_DEF_HEIGHT * 2)

/* Size of one LVGL draw buffer in RGB888 (3 bytes per pixel).  LVGL renders
 * into the RGB888 draw buffer, then flush_cb converts it to RGB565 (with
 * ordered dithering + byte swap) into the RGB565 DMA buffer above.  Keeping
 * the two formats in separate buffers decouples the 8-bit render path from
 * the 16-bit DMA path so the existing 565 double-buffer machinery is
 * unchanged. */
#define NYABULA_DUAL_LCD_DRAW_BYTES \
  (NYABULA_DUAL_LCD_DEF_WIDTH * NYABULA_DUAL_LCD_DEF_HEIGHT * 3)

/* Default refresh rate in Hz.  The TE source (see nyabula_te.h) derives the
 * software frame clock from this when the SW source is selected. */

#ifndef NYABULA_DUAL_LCD_REFRESH_HZ
#define NYABULA_DUAL_LCD_REFRESH_HZ 60
#endif

/* The transfer request is a single-slot mailbox, not a queue.  The BlankGated
 * algorithm is strictly single-flight (xfer_busy gates it), so at any instant
 * there is at most ONE whole-frame write pending (a ready slot) plus the one
 * in flight on the bus.  A bounded multi-slot queue would never hold more than
 * one element, so per the vsyncalg_plusplus reference (which has no such
 * queue) we use exactly one slot.  The only cross-thread sync needed is a
 * counting semaphore (job_avail) that wakes the transfer thread when a job is
 * posted and lets it block (CPU-free) when idle.
 */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A full-frame RGB565 offscreen buffer.  `busy` is true while the buffer
 * holds rendered-but-not-yet-freed content (i.e. it must not be reused by
 * LVGL until its whole-frame write completes and the slot is released). */

typedef struct
{
  uint8_t *data; /* Buffer base address (buf_size bytes) */
  bool busy; /* true = occupied (rendered / awaiting write); false = free */
} nyabula_buf_t;

/* Forward declaration: each screen holds a back-pointer to its owning dual
 * context so the algorithm's request callbacks can reach the job queue /
 * threads without a global lookup. */

struct nyabula_dual_lcd_s;

/* Per-screen state and LVGL handle.  The scheduling state lives in the
 * shared nyabula_sch_bg_t in the dual context (see
 * nyabula_scheduler_blankgated.h); this struct holds only the framework-side
 * resources (device, LVGL display, buffers, sync).
 *
 * The tag "nyabula_screen" matches the opaque forward-declaration used by
 * the algorithm header, so the algorithm can reference screens purely by
 * pointer without knowing their layout. */

struct nyabula_screen
{
  /* LCD device (/dev/lcd0 or /dev/lcd1) */
  int fd;
  lv_display_t *disp;

  /* Geometry */
  int width;
  int height;
  int stride;      /* Bytes per scan line of the RGB565 DMA buffer (width*2) */
  int draw_stride; /* Bytes per scan line of the RGB888 draw buffer (width*3)
                    */
  uint32_t buf_size;  /* Bytes per full frame RGB565 DMA buffer */
  uint32_t draw_size; /* Bytes per full-frame RGB888 draw buffer */
  int total_lines;    /* height */

  struct lcddev_area_align_s align;

  /* Double buffers (full screen each, RGB565, DMA-direct to the panel) */
  nyabula_buf_t buf[2];

  /* LVGL draw buffers (full screen each, RGB888).  LVGL renders here, then
   * flush_cb converts to RGB565 into buf[].  Dynamically allocated (need not
   * be DMA-aligned: only buf[] is DMA'd). */
  uint8_t *draw_data[2];

  /* LVGL draw-buffer descriptors bound to buf[].data.  The algorithm picks
   * the offscreen slot to render into; request_render redirects LVGL to that
   * slot by reordering these two via lv_display_set_draw_buffers (whose
   * first argument becomes the active draw buffer). */
  lv_draw_buf_t draw_buf[2];

  /* The slot the algorithm has selected for the next render (0/1, or -1 if
   * none pending).  Written by sch_request_render under st_mutex; consumed by
   * the render loop to point LVGL at the right buffer before lv_refr_now(). */
  int pending_slot;

  /* Posted by the transfer thread whenever a buffer slot becomes fully
   * written (whole-frame DMA done) and is therefore free for LVGL to reuse.
   * The render thread's flush_wait_cb waits here while a target buffer is
   * still locked. */
  sem_t buf_free;

  /* TE-driven render request.  The TE scan-start edge (via the algorithm)
   * requests a render; the render loop consumes this flag to issue exactly
   * one lv_refr_now() for this screen at the frame boundary.  Guarded by
   * st_mutex. */
  bool render_request;

  /* Set by the LV_EVENT_RENDER_READY handler when this display actually
   * redrew (i.e. it had at least one invalid area).  The render loop clears
   * it before calling lv_refr_now() and reads it afterwards: if it is still
   * false, LVGL skipped the draw entirely (static frame) and the requested
   * slot must be released via on_render_skip instead of on_render_done.
   * Same thread as lv_refr_now() (the render loop), so no lock needed. */
  bool lvgl_rendered;

  /* Serializes access to this screen's scheduler state shared between the
   * TE thread, the transfer thread and the render thread (flush_cb).
   * The edge entry points are intended to be usable from a real TE ISR,
   * so this is a mutex-like binary semaphore safe to post from both thread
   * and (upcoming) interrupt context. */
  sem_t st_mutex;

  /* Screen index (0 or 1), set at create time. */
  int screen_id;

  /* Back-pointer to the owning dual context (set at create time), used by
   * the request callbacks to reach the job queue / threads. */
  struct nyabula_dual_lcd_s *dual;

  bool initialized;
};

typedef struct nyabula_screen nyabula_screen_t;

/* A pending whole-frame write queued for the transfer thread.  The BlankGated
 * scheduler writes the ENTIRE frame (all total_lines rows, from row 0) in one
 * blocking QSPI DMA per slot, gated on the target screen's blanking. */
typedef struct
{
  int screen_id; /* 0 or 1 */
  int buf_idx;   /* Offscreen buffer slot holding the content */
} nyabula_write_job_t;

/* Dual screen context.  The tag is exposed so the audit interface (in
 * nyabula_display_audit.h) can forward-declare it without pulling in the
 * full definition. */
typedef struct nyabula_dual_lcd_s
{
  nyabula_screen_t screen[NYABULA_DUAL_LCD_MAX_SCREENS];

  /* Scheduling algorithm context (shared by both screens so the single
   * shared-bus write is arbitrated here).  Prefixed `sch` rather than the
   * concrete algorithm name so swapping in a different scheduler does not
   * force a rename of the framework's integration point. */
  nyabula_sch_bg_t sch;

  /* Transfer thread: blocking QSPI DMA, one whole-frame at a time. */
  pthread_t transfer_thread;

  /* TE edge source (software frame clock or panel TE GPIO interrupt, see
   * nyabula_te.h).  It calls the edge callbacks on scan-start/blank-start;
   * the framework only forwards them to the algorithm.  Allocated by
   * nyabula_te_init(), released by nyabula_te_deinit(). */
  nyabula_te_t *te;
  bool running;

  /* Single-slot write-job mailbox consumed by the transfer thread.  The
   * BlankGated algorithm is single-flight (xfer_busy gates it), so at most
   * one whole-frame write is pending at a time -- exactly the reference
   * scheduler's "at most one ready" model, hence just ONE slot (no FIFO).
   * job_mutex guards the single slot against the producer (TE/scheduler
   * thread via request_write) and the consumer (transfer thread); job_avail
   * is the counting semaphore that parks the transfer thread when idle and
   * wakes it when a job arrives. */
  nyabula_write_job_t job_slot;
  sem_t job_mutex; /* Guards the single job slot (binary mutex) */
  sem_t job_avail; /* Count of pending jobs (0/1 under single-flight) */

  /* Render-kick semaphore: wakes the render loop (run by the caller's
   * thread via nyabula_dual_lcd_task()) to consume TE-driven render
   * requests sooner than the default idle sleep.  The render loop and the
   * TE/transfer threads are decoupled from main by this layer; only the
   * lv_timer_handler() call is hosted by the caller. */
  sem_t render_kick;

  bool quitting;
} nyabula_dual_lcd_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: nyabula_dual_lcd_create
 *
 * Description:
 *   Create dual LCD display drivers with double buffering support.
 *   Both screens share a single QSPI bus, so DMA transfers are serialized.
 *
 * Input Parameters:
 *   dev_path0 - Path to first LCD device (e.g., "/dev/lcd0")
 *   dev_path1 - Path to second LCD device (e.g., "/dev/lcd1")
 *   width     - Screen width in pixels
 *   height    - Screen height in pixels
 *
 * Returned Value:
 *   Pointer to the created dual LCD context on success, NULL on failure.
 *
 ****************************************************************************/

nyabula_dual_lcd_t *nyabula_dual_lcd_create(const char *dev_path0,
                                            const char *dev_path1, int width,
                                            int height);

/****************************************************************************
 * Name: nyabula_dual_lcd_destroy
 *
 * Description:
 *   Destroy dual LCD display drivers and release resources.
 *
 * Input Parameters:
 *   dual - Pointer to the dual LCD context to destroy.
 *
 ****************************************************************************/

void nyabula_dual_lcd_destroy(nyabula_dual_lcd_t *dual);

/****************************************************************************
 * Name: nyabula_dual_lcd_task
 *
 * Description:
 *   Run one step of the render loop from the caller's own thread.  Each
 *   call advances LVGL (lv_timer_handler()) and consumes any pending
 *   TE-driven render requests.  The caller (e.g. main) should invoke this
 *   in a loop; the display layer starts the TE/transfer threads in
 *   create().  The caller must have finished building the UI before the
 *   first call (LVGL is single-threaded).
 *
 *   Returns after processing one render step (blocking up to the LVGL idle
 *   period when there is nothing to do, or waking early on a render kick).
 *
 * Input Parameters:
 *   dual - Pointer to the dual LCD context.
 *
 ****************************************************************************/

void nyabula_dual_lcd_task(nyabula_dual_lcd_t *dual);

/****************************************************************************
 * Name: nyabula_dual_lcd_get_display
 *
 * Description:
 *   Get LVGL display instance for a specific screen.
 *
 * Input Parameters:
 *   dual      - Pointer to the dual LCD context.
 *   screen_id - Screen index (0 or 1).
 *
 * Returned Value:
 *   Pointer to lv_display_t on success, NULL on failure.
 *
 ****************************************************************************/

lv_display_t *nyabula_dual_lcd_get_display(nyabula_dual_lcd_t *dual,
                                           int screen_id);

#ifdef __cplusplus
}
#endif

#endif /* __NYABULA_DUAL_LCD_H */
