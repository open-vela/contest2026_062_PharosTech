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
#include <syslog.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8388.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/pcm.h>
#include <nuttx/i2c/i2c_master.h>

#include "kickpi_k7.h"
#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk3576_sai.h"

#ifdef CONFIG_KICKPI_K7_AUDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KICKPI_K7_ES8388_I2C_BUS  3      /* ES8388 on I2C3 (2ac60000) */
#define KICKPI_K7_ES8388_I2C_ADDR 0x10   /* DTS es8388@10             */
#define KICKPI_K7_ES8388_I2C_FREQ 100000 /* 100 kHz control clock     */
#define KICKPI_K7_SAI_BUS         1      /* SAI1                      */
#define KICKPI_K7_PCM_DEVNAME     "pcm0"

/* SAI1 default signal group (DTS sai1m0, all GPIO4 bank, alt func 1):
 * MCLK=GPIO4_A2, SCLK=GPIO4_A3, LRCK=GPIO4_A5, SDO0=GPIO4_A7, SDI0=GPIO4_B3.
 */

#define KICKPI_K7_SAI1_MCLK (GPIO_PORT4 | GPIO_PIN_A2 | GPIO_ALT | GPIO_AF1)
#define KICKPI_K7_SAI1_SCLK (GPIO_PORT4 | GPIO_PIN_A3 | GPIO_ALT | GPIO_AF1)
#define KICKPI_K7_SAI1_LRCK (GPIO_PORT4 | GPIO_PIN_A5 | GPIO_ALT | GPIO_AF1)
#define KICKPI_K7_SAI1_SDO0 (GPIO_PORT4 | GPIO_PIN_A7 | GPIO_ALT | GPIO_AF1)
#define KICKPI_K7_SAI1_SDI0 (GPIO_PORT4 | GPIO_PIN_B3 | GPIO_ALT | GPIO_AF1)

/* ES8388 control bus: I2C3 default group (DTS i2c3m0-xfer, GPIO4 bank, alt
 * func 11): SCL=GPIO4_B4, SDA=GPIO4_B5.
 */

#define KICKPI_K7_I2C3_SCL (GPIO_PORT4 | GPIO_PIN_B4 | GPIO_ALT | GPIO_AF11)
#define KICKPI_K7_I2C3_SDA (GPIO_PORT4 | GPIO_PIN_B5 | GPIO_ALT | GPIO_AF11)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct es8388_lower_s g_es8388_lower = {
  .frequency = KICKPI_K7_ES8388_I2C_FREQ,
  .address = KICKPI_K7_ES8388_I2C_ADDR,
};

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
  struct audio_lowerhalf_s *es8388;
  struct audio_lowerhalf_s *pcm;
  struct i2c_master_s *i2c;
  struct i2s_dev_s *i2s;
  int ret;

  /* Mux the SAI1 signal group. */

  rk3576_config_gpio(KICKPI_K7_SAI1_MCLK);
  rk3576_config_gpio(KICKPI_K7_SAI1_SCLK);
  rk3576_config_gpio(KICKPI_K7_SAI1_LRCK);
  rk3576_config_gpio(KICKPI_K7_SAI1_SDO0);
  rk3576_config_gpio(KICKPI_K7_SAI1_SDI0);

  /* Bring up the I2C3 control bus and the SAI1 I2S data interface. */

  /* Mux the I2C3 control bus (SCL/SDA). */

  rk3576_config_gpio(KICKPI_K7_I2C3_SCL);
  rk3576_config_gpio(KICKPI_K7_I2C3_SDA);

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

  es8388 = es8388_initialize(i2c, i2s, &g_es8388_lower);
  if (es8388 == NULL)
    {
      syslog(LOG_ERR, "ERROR: es8388_initialize failed\n");
      return -ENODEV;
    }

  pcm = pcm_decode_initialize(es8388);
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

  syslog(LOG_INFO, "INFO: audio ready: /dev/audio/%s (ES8388 on SAI1)\n",
         KICKPI_K7_PCM_DEVNAME);
  return OK;
}

#endif /* CONFIG_KICKPI_K7_AUDIO */
