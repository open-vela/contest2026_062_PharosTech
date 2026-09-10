/****************************************************************************
 * app/nyabula/src/nyabula_eye_main.c
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

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <sys/boardctl.h>

#include "nyabula_display.h"
#include "nyabula_eye_service.h"

/****************************************************************************
 * Name: main or nyabula_eye_main
 *
 * Description:
 *   Own the product's Eye/LVGL thread. Display remains an independent service;
 *   its demo entry point and demo objects are not used here.
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int ret;

  if (lv_is_initialized())
    {
      fprintf(stderr, "nyabula_eye: LVGL is already owned\n");
      return EBUSY;
    }

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
  boardctl(BOARDIOC_INIT, 0);
#endif

  ret = nyabula_display_init(CONFIG_NYABULA_DISPLAY_LCD_DEVPATH0,
                             CONFIG_NYABULA_DISPLAY_LCD_DEVPATH1,
                             CONFIG_NYABULA_DISPLAY_SCREEN_WIDTH,
                             CONFIG_NYABULA_DISPLAY_SCREEN_HEIGHT);
  if (ret < 0)
    {
      fprintf(stderr, "nyabula_eye: display initialization failed: %d\n", ret);
      return 1;
    }

  ret = nyabula_eye_service_attach(
      lv_display_get_screen_active(nyabula_display_get_screen(0)),
      lv_display_get_screen_active(nyabula_display_get_screen(1)));
  if (ret < 0)
    {
      fprintf(stderr, "nyabula_eye: eye attach failed: %d\n", ret);
      nyabula_display_deinit();
      return 1;
    }

  printf("nyabula_eye: Eye Engine attached to LCD0 and LCD1\n");

  /* Keep all Eye and LVGL calls on this thread. Core workers only enqueue. */

  for (;;)
    {
      nyabula_eye_service_tick();
      nyabula_display_task();
    }

  nyabula_eye_service_detach();
  nyabula_display_deinit();
  return 0;
}
