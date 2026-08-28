/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_audio.c
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
 * On-board audio bring-up: wire the RK3576 SAI1 I2S controller
 * (i2s_dev_s, PL330 DMA data path) to the on-board Everest ES8388 codec
 * (I2C3, 7-bit address 0x10) and register the combined PCM device as
 * /dev/audio/pcm0.  The SAI1 signal group (MCLK/SCLK/LRCK/SDO0/SDI0) is
 * muxed here since there is no pinctrl framework yet.  SAI1 is the I2S
 * master and provides MCLK to the codec.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/atomic.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8388.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/pcm.h>
#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>

#include "arm64_internal.h"
#include "kickpi_k7.h"
#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk3576_sai.h"

#ifdef CONFIG_KICKPI_K7_AUDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KICKPI_K7_ES8388_I2C_BUS    3      /* ES8388 on I2C3 (2ac60000) */
#define KICKPI_K7_ES8388_I2C_ADDR   0x10   /* DTS es8388@10             */
#define KICKPI_K7_ES8388_I2C_FREQ   100000 /* 100 kHz control clock     */
#define KICKPI_K7_SAI_BUS           1      /* SAI1                      */
#define KICKPI_K7_PCM_DEVNAME       "pcm0"
#define KICKPI_K7_PCM_INPUT_DEVNAME "pcm_in0"

#define KICKPI_K7_HP_DETECT_POLL_MS 100
#define KICKPI_K7_HP_DEBOUNCE_COUNT 2
#define KICKPI_K7_SPK_ENABLE_MS     30
#define KICKPI_K7_SPK_DISABLE_MS    40
#define KICKPI_K7_HP_DETECT         (GPIO_PORT0 | GPIO_PIN_D3)
#define KICKPI_K7_SPK_ENABLE        (GPIO_PORT2 | GPIO_PIN_B1)

/* SYS_GRF_SOC_CON18 bit 1 routes SAI1 MCLK to the selected IO pin. */

#define KICKPI_K7_SYS_GRF_SOC_CON18 0x26046400
#define KICKPI_K7_SAI1_MCLKOUT_BIT  (1u << 1)

/* SAI1 default signal group (DTS sai1m0, all GPIO4 bank, alt func 1):
 * MCLK=GPIO4_A2, SCLK=GPIO4_A3, LRCK=GPIO4_A5, SDO0=GPIO4_A7, SDI0=GPIO4_B3.
 */

#define KICKPI_K7_SAI1_MCLK (GPIO_PORT4 | GPIO_PIN_A2)
#define KICKPI_K7_SAI1_SCLK (GPIO_PORT4 | GPIO_PIN_A3)
#define KICKPI_K7_SAI1_LRCK (GPIO_PORT4 | GPIO_PIN_A5)
#define KICKPI_K7_SAI1_SDO0 (GPIO_PORT4 | GPIO_PIN_A7)
#define KICKPI_K7_SAI1_SDI0 (GPIO_PORT4 | GPIO_PIN_B3)
#define KICKPI_K7_SAI1_AF   1

/* ES8388 control bus: I2C3 default group (DTS i2c3m0-xfer, GPIO4 bank, alt
 * func 11): SCL=GPIO4_B4, SDA=GPIO4_B5.
 */

#define KICKPI_K7_I2C3_SCL (GPIO_PORT4 | GPIO_PIN_B4)
#define KICKPI_K7_I2C3_SDA (GPIO_PORT4 | GPIO_PIN_B5)
#define KICKPI_K7_I2C3_AF  11

/****************************************************************************
 * Private Data
 ****************************************************************************/

static void kickpi_k7_audio_set_output_power(enum es8388_output_route_e route,
                                             bool enable);
static enum es8388_output_route_e kickpi_k7_audio_resolve_output(void);
static bool kickpi_k7_audio_headphones_connected(void);

static const struct es8388_lower_s g_es8388_lower = {
  .frequency = KICKPI_K7_ES8388_I2C_FREQ,
  .address = KICKPI_K7_ES8388_I2C_ADDR,
  .stream_type = AUDIO_TYPE_OUTPUT,
  .swap_dac_lr = true,
  .set_output = kickpi_k7_audio_set_output_power,
  .resolve_output = kickpi_k7_audio_resolve_output,
  .headphones_connected = kickpi_k7_audio_headphones_connected,
};

static bool g_audio_initialized;
static atomic_t g_headphones_connected;
static bool g_headphones_sample;
static uint8_t g_headphones_debounce;
static FAR struct audio_lowerhalf_s *g_es8388;
static FAR struct audio_lowerhalf_s *g_es8388_input;
static struct es8388_lower_s g_es8388_capture_lower;
static FAR struct gpio_dev_s *g_headphone_gpio;
static FAR struct gpio_dev_s *g_speaker_gpio;
static struct work_s g_headphone_work;
static mutex_t g_audio_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions

 * ****************************************************************************/

static bool kickpi_k7_audio_read_headphones(void)
{
  return rk3576_gpio_read_bit(g_headphone_gpio);
}

static bool kickpi_k7_audio_headphones_connected(void)
{
  return atomic_read(&g_headphones_connected) != 0;
}

static enum es8388_output_route_e kickpi_k7_audio_resolve_output(void)
{
  return kickpi_k7_audio_headphones_connected() ? ES8388_OUTPUT_ROUTE_LINE1
                                                : ES8388_OUTPUT_ROUTE_LINE2;
}

static void kickpi_k7_audio_set_output_power(enum es8388_output_route_e route,
                                             bool enable)
{
  bool speaker = enable && (route == ES8388_OUTPUT_ROUTE_LINE2 ||
                            route == ES8388_OUTPUT_ROUTE_BOTH);

  if (speaker)
    {
      usleep(KICKPI_K7_SPK_ENABLE_MS * USEC_PER_MSEC);
    }

  rk3576_gpio_write_bit(g_speaker_gpio, speaker);

  if (!speaker)
    {
      usleep(KICKPI_K7_SPK_DISABLE_MS * USEC_PER_MSEC);
    }
}

static void kickpi_k7_audio_headphone_worker(void *arg)
{
  bool sample = kickpi_k7_audio_read_headphones();
  int ret;

  (void)arg;

  if (sample != g_headphones_sample)
    {
      g_headphones_sample = sample;
      g_headphones_debounce = 1;
    }
  else if (g_headphones_debounce < KICKPI_K7_HP_DEBOUNCE_COUNT)
    {
      g_headphones_debounce++;
    }

  if (g_headphones_debounce == KICKPI_K7_HP_DEBOUNCE_COUNT &&
      sample != kickpi_k7_audio_headphones_connected())
    {
      ret = nxmutex_lock(&g_audio_lock);
      if (ret >= 0)
        {
          struct es8388_control_s control;

          atomic_set(&g_headphones_connected, sample);
          ret = g_es8388->ops->ioctl(g_es8388, ES8388IOC_GET_CONTROL,
                                     (unsigned long)(uintptr_t)&control);
          if (ret >= 0 && control.route == ES8388_OUTPUT_ROUTE_AUTO)
            {
              control.mask = ES8388_CONTROL_OUTPUT;
              g_es8388->ops->ioctl(g_es8388, ES8388IOC_SET_CONTROL,
                                   (unsigned long)(uintptr_t)&control);
            }

          nxmutex_unlock(&g_audio_lock);
        }
    }

  work_queue(LPWORK, &g_headphone_work, kickpi_k7_audio_headphone_worker, NULL,
             MSEC2TICK(KICKPI_K7_HP_DETECT_POLL_MS));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_audio_initialize
 *
 * Description:
 *   Mux the SAI1 pins, bring up the SAI1 I2S master and the ES8388 codec
 *   over I2C3, and register the PCM device (/dev/audio/pcm0).
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int kickpi_k7_audio_initialize(void)
{
  struct es8388_control_s control;
  struct audio_lowerhalf_s *pcm;
  struct i2c_master_s *i2c;
  struct i2s_dev_s *i2s;
  int ret;

  if (g_audio_initialized)
    {
      return OK;
    }

  ret = rk3576_gpio_get(KICKPI_K7_SPK_ENABLE, &g_speaker_gpio);
  if (ret < 0)
    {
      return ret;
    }

  rk3576_gpio_set_af(g_speaker_gpio, 0);
  rk3576_gpio_set_pull(g_speaker_gpio, RK3576_GPIO_PULLDOWN);
  rk3576_gpio_write_bit(g_speaker_gpio, false);
  rk3576_gpio_set_mode(g_speaker_gpio, RK3576_GPIO_OUTPUT);

  ret = rk3576_gpio_get(KICKPI_K7_HP_DETECT, &g_headphone_gpio);
  if (ret < 0)
    {
      return ret;
    }

  rk3576_gpio_set_af(g_headphone_gpio, 0);
  rk3576_gpio_set_pull(g_headphone_gpio, RK3576_GPIO_PULLUP);
  rk3576_gpio_set_schmitt(g_headphone_gpio, true);
  rk3576_gpio_set_mode(g_headphone_gpio, RK3576_GPIO_INPUT);
  atomic_set(&g_headphones_connected, kickpi_k7_audio_read_headphones());
  g_headphones_sample = kickpi_k7_audio_headphones_connected();
  g_headphones_debounce = KICKPI_K7_HP_DEBOUNCE_COUNT;

#define _SET_GPIO_AF(pinset, af)                       \
  do                                                   \
    {                                                  \
      static FAR struct gpio_dev_s *handle;            \
      if (handle == NULL)                              \
        {                                              \
          ret = rk3576_gpio_get((pinset), &handle);    \
          if (ret < 0)                                 \
            {                                          \
              syslog(LOG_ERR,                          \
                     "ERROR: failed to get gpio dev, " \
                     "pinset: %u\n",                   \
                     (pinset));                        \
              return ret;                              \
            }                                          \
        }                                              \
      rk3576_gpio_set_af(handle, (af));                \
    }                                                  \
  while (0)

  /* Mux the SAI1 signal group. */

  _SET_GPIO_AF(KICKPI_K7_SAI1_MCLK, KICKPI_K7_SAI1_AF);
  _SET_GPIO_AF(KICKPI_K7_SAI1_SCLK, KICKPI_K7_SAI1_AF);
  _SET_GPIO_AF(KICKPI_K7_SAI1_LRCK, KICKPI_K7_SAI1_AF);
  _SET_GPIO_AF(KICKPI_K7_SAI1_SDO0, KICKPI_K7_SAI1_AF);
  _SET_GPIO_AF(KICKPI_K7_SAI1_SDI0, KICKPI_K7_SAI1_AF);

  /* This K7-specific hiword-masked route is board policy, not a property of

   * * the SAI controller.  Clearing the bit enables MCLK output; hardware
   *
   * readback is 0x1d when the route is active.
   */

  putreg32(KICKPI_K7_SAI1_MCLKOUT_BIT << 16, KICKPI_K7_SYS_GRF_SOC_CON18);

  /* Bring up the I2C3 control bus and the SAI1 I2S data interface. */

  /* Mux the I2C3 control bus (SCL/SDA). */

  _SET_GPIO_AF(KICKPI_K7_I2C3_SCL, KICKPI_K7_I2C3_AF);
  _SET_GPIO_AF(KICKPI_K7_I2C3_SDA, KICKPI_K7_I2C3_AF);

  i2c = rk3576_i2c_initialize(KICKPI_K7_ES8388_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: I2C%d init failed\n", KICKPI_K7_ES8388_I2C_BUS);
      return -ENODEV;
    }

  i2s = rk3576_sai_initialize(KICKPI_K7_SAI_BUS);
  if (i2s == NULL)
    {
      syslog(LOG_ERR, "ERROR: SAI%d init failed\n", KICKPI_K7_SAI_BUS);
      return -ENODEV;
    }

  /* Bind the codec to the I2C/I2S interfaces, wrap it in the PCM decoder
   * (so it accepts WAV/PCM streams) and register the audio device.
   */

  g_es8388 = es8388_initialize(i2c, i2s, &g_es8388_lower);
  if (g_es8388 == NULL)
    {
      syslog(LOG_ERR, "ERROR: es8388_initialize failed\n");
      return -ENODEV;
    }

  memset(&control, 0, sizeof(control));
  control.mask = ES8388_CONTROL_ALL;
  control.route = ES8388_OUTPUT_ROUTE_AUTO;
  control.input_route = ES8388_INPUT_ROUTE_LINE2;
  ret = g_es8388->ops->ioctl(g_es8388, ES8388IOC_SET_CONTROL,
                             (unsigned long)(uintptr_t)&control);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ES8388 control init failed: %d\n", ret);
      return ret;
    }

  pcm = pcm_decode_initialize(g_es8388);
  if (pcm == NULL)
    {
      syslog(LOG_ERR, "ERROR: pcm_decode_initialize failed\n");
      return -ENODEV;
    }

  ret = audio_register(KICKPI_K7_PCM_DEVNAME, pcm);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: audio_register(%s) failed: %d\n",
             KICKPI_K7_PCM_DEVNAME, ret);
      return ret;
    }

  /* Use a raw codec instance for capture.  PCM decode is output-only; the
   *
   * input device accepts empty audio pipeline buffers filled by SAI RX.
   */

  g_es8388_capture_lower = g_es8388_lower;
  g_es8388_capture_lower.stream_type = AUDIO_TYPE_INPUT;
  g_es8388_input = es8388_initialize(i2c, i2s, &g_es8388_capture_lower);
  if (g_es8388_input == NULL)
    {
      syslog(LOG_ERR, "ERROR: ES8388 capture init failed\n");
      return -ENODEV;
    }

  ret = g_es8388_input->ops->ioctl(g_es8388_input, ES8388IOC_SET_CONTROL,
                                   (unsigned long)(uintptr_t)&control);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ES8388 capture control init failed: %d\n", ret);
      return ret;
    }

  ret = audio_register(KICKPI_K7_PCM_INPUT_DEVNAME, g_es8388_input);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: audio_register(%s) failed: %d\n",
             KICKPI_K7_PCM_INPUT_DEVNAME, ret);
      return ret;
    }

  syslog(LOG_INFO,
         "INFO: audio ready: /dev/audio/%s and %s "
         "(ES8388 on SAI1)\n",
         KICKPI_K7_PCM_DEVNAME, KICKPI_K7_PCM_INPUT_DEVNAME);
  g_audio_initialized = true;
  work_queue(LPWORK, &g_headphone_work, kickpi_k7_audio_headphone_worker, NULL,
             MSEC2TICK(KICKPI_K7_HP_DETECT_POLL_MS));
  return OK;
}

#endif /* CONFIG_KICKPI_K7_AUDIO */
