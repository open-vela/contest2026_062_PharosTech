/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_boardctl.c
 *
 * SPDX-License-Identifier: Apache-2.0
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

#include <errno.h>
#include <semaphore.h>
#include <stdint.h>

#include <nuttx/config.h>
#include <sys/boardctl.h>

#include "kickpi_k7.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of per-edge TE semaphores (bs/ss x 2 screens). */

#define KICKPI_K7_TE_SEM_COUNT 4

/* camenc's request to point the capture path at another of the sensor's
 * modes.  The argument is the mode's index in the sensor driver's mode table;
 * see ov5647.h.
 *
 * One past BOARDIOC_USER, which is the last command the framework itself
 * defines; everything above it belongs to the board, and boardctl() forwards
 * that whole range to board_ioctl() when CONFIG_BOARDCTL_IOCTL is set.
 *
 * There is no shared header for this pair, so the two halves name each other:
 * the sending half is CAMENC_BOARDIOC_SET_MODE in app/camenc/camenc_main.c.
 * That is the convention this file already follows for BOARDIOC_USER (fixed
 * ABI contract, no shared header), and it is the reason both halves say what
 * they are the other half of. */

#define BOARDIOC_CAMERA_SET_MODE (BOARDIOC_USER + 1)

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Per-edge TE semaphores registered by the nyabula_display application via
 * boardctl(BOARDIOC_USER).  Layout (fixed ABI contract, no shared header):
 *   g_te_sem[0] = screen 0 blank-start (bs, rising edge)
 *   g_te_sem[1] = screen 0 scan-start  (ss, falling edge)
 *   g_te_sem[2] = screen 1 blank-start
 *   g_te_sem[3] = screen 1 scan-start
 *
 * The TE GPIO ISR (kickpi_k7_lcd.c) classifies each edge by reading the pin
 * level at interrupt time (HIGH=blanking -> bs, LOW=scanning -> ss) and posts
 * the corresponding sem.  This locks the edge direction in the ISR, so the
 * consumer thread need not re-read the pin level later (which was the source
 * of the stale-read / unpaired-edge bug under heavy CPU load).  The pointers
 * are NULL until the app registers them; a NULL slot simply drops the edge. */

#ifdef CONFIG_BOARDCTL_IOCTL
FAR sem_t *g_te_sem[KICKPI_K7_TE_SEM_COUNT];
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_ioctl
 *
 * Description:
 *   Board-specific boardctl command handler.  The nyabula_display app uses
 *   BOARDIOC_USER to register the four per-edge semaphores so the TE GPIO
 *   ISR can post them directly (bypassing the POSIX signal path).  ABI:
 *   arg is the base address of a contiguous array of four sem_t (order
 *   bs0, ss0, bs1, ss1).  Passing arg == 0 clears the registration.
 *
 *   CONFIG_BOARDCTL_IOCTL must be enabled for boardctl() to route the
 *   BOARDIOC_USER command here (boardctl.c forwards unrecognized commands to
 *   board_ioctl() when CONFIG_BOARDCTL_IOCTL=y).
 *
 *   camenc uses BOARDIOC_CAMERA_SET_MODE, with the new mode's index as the
 *   argument, to change which sensor mode the capture path is configured for.
 *   It is handled here rather than by the application driving the drivers
 *   itself because the mode is not one driver's business: a mode brings its
 *   geometry, its link rate and its Bayer order with it, and those belong to
 *   the D-PHY, the CSI HOST and the capture engine respectively.  The board
 *   is the only place that can move all of them together and in the right
 *   order; see kickpi_k7_camera_set_mode().
 *
 ****************************************************************************/

#ifdef CONFIG_BOARDCTL_IOCTL
int board_ioctl(unsigned int cmd, uintptr_t arg)
{
  FAR sem_t *base;
  int i;

  switch (cmd)
    {
      case BOARDIOC_USER:
        /* Register (or clear, if arg == 0) the four edge semaphores.  The
         * application passes the base address of a contiguous array of four
         * sem_t (order bs0, ss0, bs1, ss1); we store the address of each
         * element so the ISR can post them individually. */

        if (arg == 0)
          {
            for (i = 0; i < KICKPI_K7_TE_SEM_COUNT; i++)
              {
                g_te_sem[i] = NULL;
              }

            return OK;
          }

        base = (FAR sem_t *)arg;
        for (i = 0; i < KICKPI_K7_TE_SEM_COUNT; i++)
          {
            g_te_sem[i] = &base[i];
          }

        return OK;

      case BOARDIOC_CAMERA_SET_MODE:
        /* Point the camera's receive chain at another sensor mode.  The
         * argument is the mode's index, and an index that names no mode is
         * refused before anything is taken down, so a bad request leaves the
         * stream that is configured alone. */

#ifdef CONFIG_KICKPI_K7_CAMERA
        return kickpi_k7_camera_set_mode((unsigned int)arg);
#else
        return -ENOTTY;
#endif

      default:
        return -ENOTTY;
    }
}
#endif /* CONFIG_BOARDCTL_IOCTL */
