/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_mipi_dsi.c
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
 * On-board MIPI DSI LCD panel bring-up for the kickpi-k7 (RK3576).
 *
 * Wiring map (see kickpi-k7-dsi-display.txt):
 *   - DSI data/clock lanes use dedicated pins (no GPIO mux needed).
 *   - LCD_BL_PWM : GPIO0_B5 / PWM1_CH1  (backlight)
 *   - LCD_RST    : GPIO0_A2             (panel reset, active-low)
 *   - LCD_PWREN  : GPIO0_C6             (panel power enable)
 *
 * Panel: WKS50HF072-WCT (5.0", 720 x 1280, DSI video mode).
 *
 * NOTE: The DSI panel is NOT a fixed on-board accessory; users may swap in a
 * different panel module.  However, this board code currently supports only
 * ONE model -- the WKS50HF072-WCT -- whose DCS init sequence and video-mode
 * timing are hard-coded below.  If a different panel is attached, its data
 * sheet must be consulted and the timing / init sequence re-tuned; this code
 * is NOT a drop-in match for arbitrary DSI panels.
 *
 * WKS50HF072-WCT:
 *   - 720 x 1280 portrait, RGB888, 4 data lanes, ~64 MHz pixel clock
 *   - Timing: HFP=52 HSYNC=25 HBP=26 (htotal=823),
 *             VFP=5 VSYNC=2 VBP=9    (vtotal=1296)  -> ~60 Hz
 *
 * Bring-up sequence (per the RK3576 chip drivers; the DSI driver owns the
 * DCPHY, so board code never touches the PHY directly):
 *   1. rk3576_mipi_dsi_initialize(cfg)          -- DSI-2 host; brings up the
 *                                               DCPHY and enters Command mode
 *   2. mipi_dsi_host_register / device_register -- bind panel device
 *   3. send DCS init sequence (CRI command path, PHY already powered)
 *   4. rk3576_mipi_dsi_enable_video(...)        -- program IPI timing, then
 *                                               transition to Video mode
 *   5. rk3576_vop_initialize(...)               -- register /dev/fbN
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <assert.h>
#include <errno.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/video/mipi_display.h>
#include <nuttx/video/mipi_dsi.h>

#include "rk3576_gpio.h"
#include "rk3576_mipi_dsi.h"
#include "rk3576_pwm.h"
#include "rk3576_vop.h"

#ifdef CONFIG_KICKPI_K7_MIPI_DSI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DSI-2 video-mode packet type.  The RK3576 DSI-2 controller encodes the
 * video transmission mode in DSI2_VID_TX_CFG.vid_mode_type (TRM 18.3.2.3)
 * as: 0 = non-burst with sync pulses, 1 = non-burst with sync events,
 * 2 = burst.  The value is passed verbatim through rk3576_dsi_config.
 */

#define KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS (0x1)

/* Panel geometry and link configuration. */

#define KICKPI_K7_MIPI_DSI_XRES   720
#define KICKPI_K7_MIPI_DSI_YRES   1280
#define KICKPI_K7_MIPI_DSI_LANES  4
#define KICKPI_K7_MIPI_DSI_FORMAT MIPI_DSI_FMT_RGB888
#define KICKPI_K7_MIPI_DSI_PIXCLK 64000000u /* Hz */
#define KICKPI_K7_MIPI_DSI_HS_RATE         \
  384000000u /* Hz: pixclk * bpp / lanes = \
              * 64e6 * 24 / 4 */

/* Panel timing (porches / sync, in pixels / lines). */

#define KICKPI_K7_HSYNC_LEN    25
#define KICKPI_K7_HFRONT_PORCH 52
#define KICKPI_K7_HBACK_PORCH  26
#define KICKPI_K7_VSYNC_LEN    2
#define KICKPI_K7_VFRONT_PORCH 5
#define KICKPI_K7_VBACK_PORCH  9

/* DSI host bus number (the RK3576 driver sets host.bus = 0). */

#define KICKPI_K7_DSI_BUS 0
#define KICKPI_K7_DSI_VC  0 /* Virtual channel */

/* Panel control pins. */

#define KICKPI_K7_MIPI_DSI_RST   (GPIO_PORT0 | GPIO_PIN_A2)
#define KICKPI_K7_MIPI_DSI_PWREN (GPIO_PORT0 | GPIO_PIN_C6)

/* Backlight: PWM1 channel 1 -> GPIO0_B5.  The controller index 1 selects
 * the second RK3576 PWM controller (PWM1 @ 0x2ADD0000, per the chip
 * hardware/rk3576_pwm.h enumeration RK3576_PWM1).  Registers the PWM as
 * /dev/pwm0 and starts it through the lower-half ops.
 */

#define KICKPI_K7_BL_PWM_CTRL 1 /* RK3576_PWM1 (PWM1 @ 0x2ADD0000) */
#define KICKPI_K7_BL_PWM_CH   1
#define KICKPI_K7_BL_DEVNAME  "pwm0"
#define KICKPI_K7_BL_FREQ_HZ  1000
#define KICKPI_K7_BL_DUTY     (1 << 15) /* 50% duty (ub16_t) */

/* Backlight PWM output pin: GPIO0_B5 muxed to PWM1_CH1_M0 (AF 0xc = 12,
 * per the RK3576 TRM IOMUX table for GPIO0_B5). */

#define KICKPI_K7_BL_PWM_PIN (GPIO_PORT0 | GPIO_PIN_B5)
#define KICKPI_K7_BL_PWM_AF  12

/* MIPI packet types used by this panel's DCS init sequence. */

#define KICKPI_K7_PKT_GEN_LONG 0x39 /* Generic long write */
#define KICKPI_K7_PKT_DCS_LONG 0x23 /* DCS long write */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One entry of the panel DCS init sequence.  `data` carries the payload
 * bytes (after the DSI type/delay/word-count header), `type` is the raw
 * MIPI packet type to transmit (kept verbatim from the panel DT node), and
 * `delay_ms` is the wait after sending the command.
 */

struct kickpi_k7_mipi_dsi_cmd_s
{
  uint8_t type;            /* MIPI packet data type */
  uint8_t delay_ms;        /* Post-command delay in ms */
  uint8_t len;             /* Payload length (bytes) */
  FAR const uint8_t *data; /* Payload */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

#define _PANEL_INIT(...)              \
  .data = (uint8_t[]){ __VA_ARGS__ }, \
  .len = sizeof((uint8_t[]){ __VA_ARGS__ }) / sizeof(uint8_t)

/* Consolidated init sequence.  Each entry reproduces one line of the
 * `panel-init-sequence` DT node verbatim.  `type` is the raw MIPI packet
 * type (0x23 = DCS write, 0x39 = generic long write), `delay_ms` is the
 * post-command wait decoded from the DT second byte (0x78 = 120 ms for
 * sleep-out, 0x14 = 20 ms for display-on), and `data`/`len` are the payload
 * bytes that follow the word-count field.
 */

static const struct kickpi_k7_mipi_dsi_cmd_s g_kickpi_k7_mipi_dsi_init[] = {
  { KICKPI_K7_PKT_GEN_LONG, 0, _PANEL_INIT(0xFF, 0x98, 0x81, 0x03) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x01, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x02, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x03, 0x73) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x04, 0x03) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x05, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x06, 0x0a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x07, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x08, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x09, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0a, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0b, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0c, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0d, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0e, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0f, 0x1d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x10, 0x0d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x11, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x12, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x13, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x14, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x15, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x16, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x17, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x18, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x19, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1a, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1b, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1c, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1d, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1e, 0x40) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1f, 0x80) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x20, 0x06) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x21, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x22, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x23, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x24, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x25, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x26, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x27, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x28, 0x33) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x29, 0x03) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2a, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2b, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2c, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2d, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2e, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2f, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x30, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x31, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x32, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x33, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x34, 0x04) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x35, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x36, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x37, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x38, 0x3c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x39, 0x35) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3a, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3b, 0x40) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3c, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3d, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3e, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3f, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x40, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x41, 0x88) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x42, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x43, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x44, 0x1f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x50, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x51, 0x23) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x52, 0x45) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x53, 0x67) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x54, 0x89) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x55, 0xab) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x56, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x57, 0x23) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x58, 0x45) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x59, 0x67) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5a, 0x89) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5b, 0xab) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5c, 0xcd) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5d, 0xef) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5e, 0x11) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5f, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x60, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x61, 0x15) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x62, 0x14) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x63, 0x0e) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x64, 0x0f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x65, 0x0c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x66, 0x0d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x67, 0x06) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x68, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x69, 0x07) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6a, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6b, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6c, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6d, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6e, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6f, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x70, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x71, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x72, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x73, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x74, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x75, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x76, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x77, 0x14) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x78, 0x15) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x79, 0x0e) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7a, 0x0f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7b, 0x0c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7c, 0x0d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7d, 0x06) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7e, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7f, 0x07) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x80, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x81, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x82, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x83, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x84, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x85, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x86, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x87, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x88, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x89, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x8A, 0x02) },

  /* Page 0x04 register block. */
  { KICKPI_K7_PKT_GEN_LONG, 0, _PANEL_INIT(0xff, 0x98, 0x81, 0x04) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6D, 0x08) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6F, 0x05) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x70, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x71, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x82, 0x0f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x84, 0x0f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x85, 0x0d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x32, 0xac) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x8c, 0x80) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3c, 0xf5) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3a, 0x24) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xb5, 0x07) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x31, 0x45) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x88, 0x33) },

  /* Page 0x01 gamma correction block. */
  { KICKPI_K7_PKT_GEN_LONG, 0, _PANEL_INIT(0xff, 0x98, 0x81, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x22, 0x09) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x31, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x53, 0x8a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x55, 0xa2) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x50, 0x81) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x51, 0x85) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x60, 0x20) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA0, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA1, 0x1a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA2, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA3, 0x18) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA4, 0x16) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA5, 0x29) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA6, 0x1d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA7, 0x1e) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA8, 0x84) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA9, 0x1c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAA, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAB, 0x75) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAC, 0x1a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAD, 0x19) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAE, 0x4d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAF, 0x22) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xB0, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xB1, 0x54) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xB2, 0x66) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xB3, 0x39) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC0, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC1, 0x1a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC2, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC3, 0x13) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC4, 0x16) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC5, 0x29) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC6, 0x1d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC7, 0x1e) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC8, 0x84) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC9, 0x1c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCA, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCB, 0x75) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCC, 0x1a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCD, 0x19) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCE, 0x4d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCF, 0x22) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xD0, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xD1, 0x54) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xD2, 0x66) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xD3, 0x39) },

  /* Page 0x00 (normal). */
  { KICKPI_K7_PKT_GEN_LONG, 0, _PANEL_INIT(0xff, 0x98, 0x81, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x35, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x36, 0x03) },
  { KICKPI_K7_PKT_DCS_LONG, 120,
    _PANEL_INIT(0x11, 0x00) }, /* 23 78: sleep out, 120ms  */
  { KICKPI_K7_PKT_DCS_LONG, 20,
    _PANEL_INIT(0x29, 0x00) }, /* 23 14: display on, 20ms  */
};

#undef _PANEL_INIT

/* Panel video-mode timing (feeding both the DSI IPI and the VOP). */

static const struct rk3576_dsi_video_timing g_kickpi_k7_mipi_dsi_timing = {
  .hactive = KICKPI_K7_MIPI_DSI_XRES,
  .hfront_porch = KICKPI_K7_HFRONT_PORCH,
  .hback_porch = KICKPI_K7_HBACK_PORCH,
  .hsync_len = KICKPI_K7_HSYNC_LEN,
  .vactive = KICKPI_K7_MIPI_DSI_YRES,
  .vfront_porch = KICKPI_K7_VFRONT_PORCH,
  .vback_porch = KICKPI_K7_VBACK_PORCH,
  .vsync_len = KICKPI_K7_VSYNC_LEN,
  .pixel_clock = KICKPI_K7_MIPI_DSI_PIXCLK,
};

/* GPIO handles claimed once in configure_pins() and cached here.  They are
 * deliberately NOT re-acquired later: the RK3576 GPIO driver enforces
 * single-occupancy, so a second rk3576_gpio_get() on the same pin fails
 * with -EBUSY (and the board code would otherwise silently fail to release
 * the panel reset).
 */

static FAR struct gpio_dev_s *g_kickpi_k7_mipi_dsi_pwren = NULL;
static FAR struct gpio_dev_s *g_kickpi_k7_mipi_dsi_rst = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_configure_pins
 *
 * Description:
 *   Claim and drive the panel control pins: LCD_RST and LCD_PWREN as
 *   outputs.  The DSI data/clock lanes use dedicated pins (no GPIO mux).
 *   The backlight PWM pin is muxed by the PWM driver path.
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_configure_pins(void)
{
  int ret;

  /* Power enable: high = panel powered. */

  if (!g_kickpi_k7_mipi_dsi_pwren)
    {
      ret = rk3576_gpio_get(KICKPI_K7_MIPI_DSI_PWREN,
                            &g_kickpi_k7_mipi_dsi_pwren);
      DEBUGASSERT(ret == OK);
    }

  rk3576_gpio_set_mode(g_kickpi_k7_mipi_dsi_pwren, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_pwren, true);

  /* Reset: hold asserted, then release after a settle delay (driven later
   * once the panel power has stabilised).  The handle is claimed once here
   * and cached; release_reset() reuses it instead of re-acquiring, because
   * a second rk3576_gpio_get() on the same pin would be rejected by the
   * single-occupancy check (-EBUSY).
   */

  if (!g_kickpi_k7_mipi_dsi_rst)
    {
      ret = rk3576_gpio_get(KICKPI_K7_MIPI_DSI_RST, &g_kickpi_k7_mipi_dsi_rst);
      DEBUGASSERT(ret == OK);
    }

  rk3576_gpio_set_mode(g_kickpi_k7_mipi_dsi_rst, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_rst, false);
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_release_reset
 *
 * Description:
 *   De-assert the panel reset line after a nominal power-up settle delay
 *   (a few ms).  Uses the RST handle cached by configure_pins().
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_release_reset(void)
{
  up_mdelay(5);

  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_rst, true);
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_backlight_enable
 *
 * Description:
 *   Mux GPIO0_B5 to the PWM1_CH1 alternate function, then bring up PWM1
 *   channel 1 as the backlight driver and start it at the configured duty
 *   cycle.  The pin mux must be done before rk3576_pwm_initialize() — the
 *   PWM lower-half driver never configures GPIO (see its header comment).
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_backlight_enable(void)
{
  static FAR struct gpio_dev_s *bl_handle = NULL;
  FAR struct pwm_lowerhalf_s *pwm;
  struct pwm_info_s info;
  int ret;

  /* Claim the backlight pin once and route it to PWM1_CH1_M0 (AF12). */

  if (bl_handle == NULL)
    {
      ret = rk3576_gpio_get(KICKPI_K7_BL_PWM_PIN, &bl_handle);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: backlight GPIO claim failed: %d\n", ret);
          return;
        }
    }

  rk3576_gpio_set_af(bl_handle, KICKPI_K7_BL_PWM_AF);

  pwm = rk3576_pwm_initialize(KICKPI_K7_BL_PWM_CTRL, KICKPI_K7_BL_PWM_CH);
  if (pwm == NULL)
    {
      syslog(LOG_ERR, "ERROR: backlight PWM initialize failed\n");
      return;
    }

  if (pwm_register(KICKPI_K7_BL_DEVNAME, pwm) < 0)
    {
      syslog(LOG_ERR, "ERROR: backlight PWM register failed\n");
      return;
    }

  memset(&info, 0, sizeof(info));
  info.frequency = KICKPI_K7_BL_FREQ_HZ;
#ifdef CONFIG_PWM_MULTICHAN
  info.channels[0].duty = KICKPI_K7_BL_DUTY;
#else
  info.duty = KICKPI_K7_BL_DUTY;
#endif

  pwm->ops->setup(pwm);
  pwm->ops->start(pwm, &info);
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_send_init_sequence
 *
 * Description:
 *   Send the panel DCS init sequence verbatim.  Each entry is transmitted
 *   with its original packet type via mipi_dsi_transfer(), then any
 *   decoded delay is applied with up_mdelay().
 *
 ****************************************************************************/

static int
kickpi_k7_mipi_dsi_send_init_sequence(FAR struct mipi_dsi_device *device)
{
  int i;
  int ncmds = nitems(g_kickpi_k7_mipi_dsi_init);

  for (i = 0; i < ncmds; i++)
    {
      FAR const struct kickpi_k7_mipi_dsi_cmd_s *cmd =
          &g_kickpi_k7_mipi_dsi_init[i];
      struct mipi_dsi_msg msg;
      ssize_t ret;

      memset(&msg, 0, sizeof(msg));
      msg.channel = KICKPI_K7_DSI_VC;
      msg.type = cmd->type;
      msg.tx_buf = cmd->data;
      msg.tx_len = cmd->len;

      ret = mipi_dsi_transfer(device, &msg);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: DSI init cmd %d failed: %zd\n", i, ret);
          return (int)ret;
        }

      if (cmd->delay_ms > 0)
        {
          up_mdelay(cmd->delay_ms);
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_initialize
 ****************************************************************************/

int kickpi_k7_mipi_dsi_initialize(void)
{
  FAR struct mipi_dsi_host *host;
  FAR struct mipi_dsi_device *dev;
  struct rk3576_dsi_config dsi_cfg;
  struct rk3576_vop_config vop_cfg;
  int ret;

  /* 1. Build the DSI link configuration.  rk3576_mipi_dsi_initialize()
   *    brings up the DCPHY and enters Command mode internally, so the
   *    panel DCS init sequence can be sent right after this call.
   */

  memset(&dsi_cfg, 0, sizeof(dsi_cfg));
  dsi_cfg.lanes = KICKPI_K7_MIPI_DSI_LANES;
  dsi_cfg.format = KICKPI_K7_MIPI_DSI_FORMAT;
  dsi_cfg.video_mode = KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS;
  dsi_cfg.hs_rate = KICKPI_K7_MIPI_DSI_HS_RATE;

  host = rk3576_mipi_dsi_initialize(&dsi_cfg);
  if (host == NULL)
    {
      syslog(LOG_ERR, "ERROR: rk3576_mipi_dsi_initialize failed\n");
      return -ENODEV;
    }

  /* 2. Register the host and bind a panel device. */

  ret = mipi_dsi_host_register(host);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_host_register failed: %d\n", ret);
      return ret;
    }

  dev = mipi_dsi_device_register(host, "kickpi-k7-lcd", KICKPI_K7_DSI_VC);
  if (dev == NULL)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_device_register failed\n");
      return -ENODEV;
    }

  /* 3. Power the panel and release reset, then send the DCS init. */

  kickpi_k7_mipi_dsi_configure_pins();
  kickpi_k7_mipi_dsi_release_reset();

  ret = kickpi_k7_mipi_dsi_send_init_sequence(dev);
  if (ret < 0)
    {
      return ret;
    }

  /* Verify the panel is on the link by reading its DCS power mode.  This is
   * a Bus-Turnaround (BTA) read, so it only succeeds if the panel is truly
   * connected and answering — a timeout here means the init writes drained
   * but nothing ever came back over lane 0.
   */

  {
    uint8_t pwrmode = 0;
    ssize_t n = mipi_dsi_dcs_read(dev, MIPI_DCS_GET_POWER_MODE, &pwrmode, 1);

    if (n == 1)
      {
        syslog(LOG_INFO, "kickpi-k7: panel power mode = 0x%02x\n", pwrmode);
      }
    else
      {
        syslog(LOG_WARNING, "kickpi-k7: panel power mode read failed: %d\n",
               (int)n);
      }
  }

  /* 4. Program the DSI video timing and switch to Video mode. */

  ret = rk3576_mipi_dsi_enable_video(host, &g_kickpi_k7_mipi_dsi_timing);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: rk3576_mipi_dsi_enable_video failed: %d\n", ret);
      return ret;
    }

  /* 5. Register the VOP framebuffer feeding the DSI IPI on PORT0. */

  memset(&vop_cfg, 0, sizeof(vop_cfg));
  vop_cfg.xres = KICKPI_K7_MIPI_DSI_XRES;
  vop_cfg.yres = KICKPI_K7_MIPI_DSI_YRES;
  vop_cfg.iface = RK3576_VOP_IFACE_MIPI_DSI;
  vop_cfg.port = RK3576_VOP_PORT0;
  vop_cfg.display = RK3576_VOP_DISPLAY_DEFAULT;
  vop_cfg.plane = 0;
  vop_cfg.hsync_len = KICKPI_K7_HSYNC_LEN;
  vop_cfg.hfront_porch = KICKPI_K7_HFRONT_PORCH;
  vop_cfg.hback_porch = KICKPI_K7_HBACK_PORCH;
  vop_cfg.vsync_len = KICKPI_K7_VSYNC_LEN;
  vop_cfg.vfront_porch = KICKPI_K7_VFRONT_PORCH;
  vop_cfg.vback_porch = KICKPI_K7_VBACK_PORCH;

  ret = rk3576_vop_initialize(&vop_cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: rk3576_vop_initialize failed: %d\n", ret);
      return ret;
    }

  /* 7. Backlight on last, after the panel is displaying. */

  kickpi_k7_mipi_dsi_backlight_enable();

  syslog(LOG_INFO, "kickpi-k7: MIPI DSI LCD bring-up OK\n");
  return OK;
}

#endif /* CONFIG_KICKPI_K7_MIPI_DSI */
