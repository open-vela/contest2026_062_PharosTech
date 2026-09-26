/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_video.c
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
 * Board-level VOP / framebuffer ownership for the kickpi-k7 (RK3576).
 *
 * See kickpi_k7_video.h for why this is a helper the output drivers call,
 * rather than a boot step, and for the single-output constraint.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include "kickpi_k7_video.h"
#include "rk3576_vop.h"

#ifdef CONFIG_KICKPI_K7_VIDEO

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The VOP instance currently owned, or NULL.  rk3576_vop.c has no way to
 * hand back a handle (it publishes one internally), so the board keeps its
 * own record purely to enforce the single-output rule and to answer
 * kickpi_k7_video_is_initialized().
 */

static FAR const struct kickpi_k7_video_mode_s *g_kickpi_k7_video_mode;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_video_initialize
 *
 * Description:
 *   Bring up the VOP for one display mode and register the framebuffer.
 *
 ****************************************************************************/

int kickpi_k7_video_initialize(FAR const struct kickpi_k7_video_mode_s *mode)
{
  struct rk3576_vop_config vop_cfg;
  int ret;

  if (mode == NULL || mode->pixel_clock == 0)
    {
      syslog(LOG_ERR, "ERROR: kickpi-k7 video invalid mode\n");
      return -EINVAL;
    }

  if (g_kickpi_k7_video_mode != NULL)
    {
      syslog(LOG_ERR,
             "ERROR: kickpi-k7 video already owned by '%s'; refusing to bring "
             "up '%s' (only one display output is supported)\n",
             g_kickpi_k7_video_mode->name, mode->name);
      return -EBUSY;
    }

  /* rk3576_vop_initialize() allocates the framebuffer, powers the CLUSTER and
   * ESMART domains, releases the resets, programs the layer / timing / port
   * and registers /dev/fbN.  All of it derives from this one struct.
   */

  memset(&vop_cfg, 0, sizeof(vop_cfg));
  vop_cfg.xres = mode->xres;
  vop_cfg.yres = mode->yres;
  vop_cfg.iface = mode->iface;
  vop_cfg.port = mode->port;
  vop_cfg.display = RK3576_VOP_DISPLAY_DEFAULT;
  vop_cfg.plane = 0;
  vop_cfg.hsync_len = mode->hsync_len;
  vop_cfg.hfront_porch = mode->hfront_porch;
  vop_cfg.hback_porch = mode->hback_porch;
  vop_cfg.vsync_len = mode->vsync_len;
  vop_cfg.vfront_porch = mode->vfront_porch;
  vop_cfg.vback_porch = mode->vback_porch;
  vop_cfg.hsync_positive = mode->hsync_positive;
  vop_cfg.vsync_positive = mode->vsync_positive;
  vop_cfg.pixel_clock = mode->pixel_clock;

  ret = rk3576_vop_initialize(&vop_cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: rk3576_vop_initialize failed for '%s' (%ux%u @ %lu Hz, "
             "iface=%d port=%d): %d\n",
             mode->name, (unsigned int)mode->xres, (unsigned int)mode->yres,
             (unsigned long)mode->pixel_clock, (int)mode->iface,
             (int)mode->port, ret);
      return ret;
    }

  g_kickpi_k7_video_mode = mode;

  /* Paint a solid colour before the stream starts.  This has to happen after
   * rk3576_vop_initialize() (the framebuffer does not exist before it) and
   * before the interface begins transmitting, so that the first frames the
   * sink receives are a known colour rather than whatever the allocator
   * handed back.  The application paints the real content later.
   */

  rk3576_vop_fill(mode->fill_rgb);

  syslog(LOG_INFO,
         "kickpi-k7: VOP up: %s, %ux%u @ %lu Hz, iface=%d port=%d, "
         "/dev/fb%d registered\n",
         mode->name, (unsigned int)mode->xres, (unsigned int)mode->yres,
         (unsigned long)mode->pixel_clock, (int)mode->iface, (int)mode->port,
         RK3576_VOP_DISPLAY_DEFAULT);

  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_video_is_initialized
 *
 * Description:
 *   Report whether the VOP has been claimed.
 *
 ****************************************************************************/

bool kickpi_k7_video_is_initialized(void)
{
  return g_kickpi_k7_video_mode != NULL;
}

#endif /* CONFIG_KICKPI_K7_VIDEO */
