/****************************************************************************
 * apps/graphics/nyabula_display/main.c
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
#include <sys/boardctl.h>
#include <unistd.h>

#include <lvgl/demos/lv_demos.h>
#include <lvgl/lvgl.h>

#include "nyabula_display.h"
#include "nyabula_dual_demo.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Should we perform board-specific driver initialization? There are two
 * ways that board initialization can occur:  1) automatically via
 * board_late_initialize() during bootup if CONFIG_BOARD_LATE_INITIALIZE
 * or 2). via a call to boardctl() if the interface is enabled
 * (CONFIG_BOARDCTL=y).
 * If this task is running as an NSH built-in application, then that
 * initialization has probably already been performed otherwise we do it
 * here.
 */

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#define NEED_BOARDINIT 1
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Run one independent, animated UI on each of the two screens to exercise
 * the dual-screen v8 scheduler (both screens animate and contend for the
 * single QSPI write bus):
 *   - Screen 0: a spinning fan blade + a horizontally bouncing ball
 *   - Screen 1: a counter-spinning fan blade + a vertically bouncing square
 *
 * Each UI is rooted on its own display's active screen, obtained via
 * lv_display_get_screen_active(disp) -- NOT the global lv_screen_active().
 * This keeps the two screens fully isolated regardless of which display is
 * the "default", so the two demos never stomp on each other's objects (the
 * way the stock single-screen stress/benchmark demos do via
 * lv_screen_active()).
 */

static void lv_demo_dual_apps(void)
{
  lv_display_t *d0 = nyabula_display_get_screen(0);
  lv_display_t *d1 = nyabula_display_get_screen(1);

  nyabula_dual_demo_create(d0, d1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main or nyabula_display_main
 *
 * Description:
 *   Nyabula Display main entry point.
 *
 * Input Parameters:
 *   Standard argc and argv
 *
 * Returned Value:
 *   Zero on success; a positive, non-zero value on failure.
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int ret;

#ifdef NEED_BOARDINIT
  /* Perform board-specific driver initialization */
  boardctl(BOARDIOC_INIT, 0);
#endif

  /* Initialize dual display */
  LV_LOG_USER("Initializing dual display: %s and %s",
              CONFIG_NYABULA_DISPLAY_LCD_DEVPATH0,
              CONFIG_NYABULA_DISPLAY_LCD_DEVPATH1);
  ret = nyabula_display_init(CONFIG_NYABULA_DISPLAY_LCD_DEVPATH0,
                             CONFIG_NYABULA_DISPLAY_LCD_DEVPATH1,
                             CONFIG_NYABULA_DISPLAY_SCREEN_WIDTH,
                             CONFIG_NYABULA_DISPLAY_SCREEN_HEIGHT);
  if (ret < 0)
    {
      LV_LOG_ERROR("Nyabula dual display initialization failed!");
      return 1;
    }

  /* Create demo UI for both screens.  LVGL is single-threaded, so the UI
   * must be built before entering the render loop. */
  lv_demo_dual_apps();

  /* Drive the render loop from this (main) thread.  The display pipeline
   * (TE + transfer threads) was started by init(); main simply hosts the
   * lv_timer_handler() call and consumes TE-driven render requests. */
  for (;;)
    {
      nyabula_display_task();

      /* main is free to do other work here between render steps. */
    }

  /* Cleanup (unreachable in this demo) */
  nyabula_display_deinit();

  return 0;
}
