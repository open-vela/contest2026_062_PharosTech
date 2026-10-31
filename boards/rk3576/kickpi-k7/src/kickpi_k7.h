/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/src/kickpi_k7.h
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

#ifndef __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H
#define __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include <semaphore.h>

#ifndef __ASSEMBLY__

struct sdio_dev_s;

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Per-edge TE semaphores registered by the nyabula_display application via
 * boardctl(BOARDIOC_USER).  Layout (fixed ABI contract):
 *   g_te_sem[0] = screen 0 blank-start (bs, rising)
 *   g_te_sem[1] = screen 0 scan-start  (ss, falling)
 *   g_te_sem[2] = screen 1 blank-start
 *   g_te_sem[3] = screen 1 scan-start
 *
 * Defined in kickpi_k7_boardctl.c (together with board_ioctl()); consumed by
 * the TE GPIO ISR in kickpi_k7_lcd.c.  NULL until the app registers them. */

#ifdef CONFIG_BOARDCTL_IOCTL
extern FAR sem_t *g_te_sem[4];
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_KICKPI_K7_STORAGE_AUTOMOUNT
int kickpi_k7_storage_initialize(FAR struct sdio_dev_s *sdmmc,
                                 FAR struct sdio_dev_s *emmc);
#endif

#ifdef CONFIG_KICKPI_K7_WIFI
int kickpi_k7_wifi_initialize(void);
#ifdef CONFIG_SV6621_PM
int kickpi_k7_wifi_prepare_sleep(void);
int kickpi_k7_wifi_abort_sleep(void);
#endif
#endif

#ifdef CONFIG_KICKPI_K7_RTC
int kickpi_k7_rtc_initialize(void);
#endif

#ifdef CONFIG_KICKPI_K7_TOUCH
/****************************************************************************
 * Name: kickpi_k7_touch_initialize
 *
 * Description:
 *   Board-level wiring for the on-board Goodix GT911 capacitive touch
 *   controller that sits on the MIPI DSI panel module: mux the I2C0 M1 pins
 *   (SCL GPIO0_C1 / SDA GPIO0_C2, AF9), bring up I2C0, claim the reset
 *   (GPIO0_D0) and interrupt (GPIO0_C5) pins and register the controller as
 *   /dev/input0 through the platform GT911 driver.
 *
 *   The module supply is shared with the display, so this must run after the
 *   display bring-up has powered it; kickpi_k7_mipi_dsi_initialize() calls it
 *   at the end of its own sequence.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_touch_initialize(void);
#endif

#ifdef CONFIG_KICKPI_K7_LCD

#ifdef CONFIG_RK3576_SDMMC
#error \
    "CONFIG_KICKPI_K7_LCD can not be enabled with CONFIG_RK3576_SDMMC=y simultaneously"
#endif

int kickpi_k7_lcd_initialize(void);
#endif

#ifdef CONFIG_KICKPI_K7_USBHOST
int kickpi_k7_usbhost_initialize(void);
#endif

#ifdef CONFIG_KICKPI_K7_MIPI_DSI
/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_initialize
 *
 * Description:
 *   Board-level wiring for the on-board MIPI DSI LCD panel: configure the
 *   panel control pins (reset / power-enable) and backlight PWM, bring up
 *   the RK3576 DCPHY + DSI-2 host, register the DSI host/panel device, send
 *   the panel DCS init sequence, then enable video and register the VOP
 *   framebuffer (/dev/fbN).
 *
 *   Caller must have brought up the RK3576 clock tree
 *   (rk3576_clk_tree_initialize() in board_late_initialize()).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_mipi_dsi_initialize(void);
#endif

#ifdef CONFIG_KICKPI_K7_HDMI
/****************************************************************************
 * Name: kickpi_k7_hdmi_initialize
 *
 * Description:
 *   Board-level wiring for the HDMI output in DVI mode over TMDS: bring up
 *   the HDMI TX controller (clocks, resets, VO0_GRF routing, register file),
 *   power the HDPTX PHY and its TMDS PLL to the mode's pixel rate, then
 *   register the VOP framebuffer that feeds the controller over the IPI.
 *
 *   The order is PHY-first, not VOP-first: on the HDMI path the PHY's own
 *   PLL generates the pixel clock and rk3576_vop.c reparents the video port
 *   straight onto it, so the VOP has no clock until the PHY is running.
 *
 *   Mutually exclusive with CONFIG_KICKPI_K7_MIPI_DSI -- see the choice in
 *   the board Kconfig.
 *
 *   Caller must have brought up the RK3576 clock tree
 *   (rk3576_clk_tree_initialize() in board_late_initialize()).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_hdmi_initialize(void);
#endif

#ifdef CONFIG_KICKPI_K7_CAMERA
/****************************************************************************
 * Name: kickpi_k7_camera_initialize
 *
 * Description:
 *   Bring up the on-board OV5647 camera module on the CSI0 connector:
 *   gate the module on through its one control GPIO, mux and start the
 *   I2C4 control bus, bind the sensor driver, bring up the CSI HOST and
 *   VICAP capture path, and register /dev/video0.
 *
 *   Requires the clock tree (rk3576_clk_tree_initialize()) to have run,
 *   which board_late_initialize() does first.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_camera_initialize(void);

/****************************************************************************
 * Name: kickpi_k7_camera_set_mode
 *
 * Description:
 *   Point the receive chain at another of the sensor's capture modes.  The
 *   mode is named by its index in the sensor driver's mode table; see
 *   ov5647.h.
 *
 *   A change of frame rate within one geometry does nothing here, because
 *   nothing outside the sensor depends on it.  A change of geometry also
 *   changes the D-PHY link rate and the capture engine's frame buffers, and
 *   both drivers are asked to move in place -- the lane rate as a PHY power
 *   cycle, the geometry as a buffer change -- so the receive chain's clocks,
 *   resets, power domain, interrupt and 3A control device are all left alone.
 *
 *   The capture device has to be closed first, because the sensor has to be
 *   stopped for the D-PHY to accept new timing parameters.  In practice that
 *   means this is called by the application between closing the camera and
 *   opening it again, not by anything that is streaming.
 *
 * Input Parameters:
 *   index - The mode to switch to, one of the sensor's OV5647_MODE_*.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure, with the receive
 *   chain left in the mode that was already configured.
 *
 ****************************************************************************/

int kickpi_k7_camera_set_mode(unsigned int index);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H */
