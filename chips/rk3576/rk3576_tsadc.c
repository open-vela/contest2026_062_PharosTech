/****************************************************************************
 * chips/rk3576/rk3576_tsadc.c
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
 * RK3576 TSADC (Temperature-Sensor ADC) driver.
 *
 * Exposes the RK3576 on-die temperature sensor to the NuttX sensor
 * framework (include/nuttx/sensors/sensor.h).  The TSADC is run in auto
 * mode: the controller periodically converts the enabled channels and keeps
 * the latest result in TSADC_DATA0..5.  Each of the six probe channels is
 * published as an independent on-die temperature sensor node, named after
 * the probe location (TRM Table 19-1).  The "temp" infix keeps each name
 * recognizable as a temperature sensor by tools such as sensorscope that
 * match on the sensor type:
 *
 *   /dev/uorb/sensor_temp_soc_center     chip center
 *   /dev/uorb/sensor_temp_soc_bigcore    big core
 *   /dev/uorb/sensor_temp_soc_littlecore little core
 *   /dev/uorb/sensor_temp_soc_gpu        GPU
 *   /dev/uorb/sensor_temp_soc_npu        NPU
 *   /dev/uorb/sensor_temp_soc_ddr        DDR
 *
 * The driver implements the sensor "fetch" operation: a user that opens a
 * node and reads from it obtains the latest ADC result for that channel,
 * converted to temperature.  Because TSADC runs continuously in auto mode
 * and the .fetch path always returns fresh data, no background thread or
 * polling interval is required.
 *
 * rk3576_tsadc_initialize() brings up the controller clocks and starts auto
 * conversion, then hands the array of per-channel sensor descriptors back
 * to the board layer, which calls sensor_custom_register() on each one.
 *
 * Source of hardware knowledge: RK3576 TRM, Chapter 19 (TS-ADC), V1.2.
 *
 * Channel -> probe location (TRM Table 19-1):
 *   0: near chip center, 1: big core, 2: little core, 3: GPU, 4: NPU,
 *   5: DDR.
 *
 * Temperature conversion (TRM Table 19-2):
 *   code:  -40C -> 220, 25C -> 285, 85C -> 345, 125C -> 385 (typical).
 * Within this operational range the code -> temperature map is exactly
 * linear at 1 C per code (see rk3576_tsadc_code_to_temp), so no look-up
 * table is needed: a single offset converts, and readings beyond the SOC's
 * normal operating range are clamped to the usable extremes.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <sys/param.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/sensors/sensor.h>
#include <nuttx/uorb.h>

#include "arm64_arch.h"
#include "hardware/rk3576_tsadc.h"
#include "rk3576_tsadc.h"

#ifdef CONFIG_RK3576_TSADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Conversion clock must be 2 MHz (TRM 19.5.2).  This is purely a runtime
 * sanity value; the actual frequency is configured by the CLK framework
 * from clk_tsadc_div. */

#define RK3576_TSADC_CLK_HZ 2000000u

/* Auto sampling period (TRM 19.4.3).  Value is in units of the TSADC clock
 * cycles, controlling the interleave between full-channel scan rounds.
 * We pick a period that yields roughly a 250 ms rescan interval in normal
 * operation and 50 ms in the hot (over-threshold) operation. */

#define RK3576_TSADC_AUTO_PERIOD_VAL    250000u /* ~250ms normal   */
#define RK3576_TSADC_AUTO_PERIOD_HT_VAL 50000u  /* ~50ms hot       */

/* Debounce counts for high-temperature interrupt/TSHUT (TRM reset 0x03). */

#define RK3576_TSADC_INT_DEBOUNCE   4
#define RK3576_TSADC_TSHUT_DEBOUNCE 4

/* Channels enabled in auto mode by default: all six. */

#define RK3576_TSADC_AUTO_SRC_ALL 0x003f

/* Number of on-die temperature probe channels exposed. */

#define RK3576_TSADC_NCHANNELS 6

/* How long to wait after power-on before the first conversion completes.
 * 2 MHz clock, a full round plus margin fits comfortably in 10 ms. */

#define RK3576_TSADC_STARTUP_DELAY_NS 10000000u

/* Code -> temperature is linear at 1 C per code across the SOC's normal
 * operating range (TRM Table 19-2): code 220 == -40 C, code 385 == 125 C.
 * CODE_OFFSET = 25 - 285 with the 25 C / code 285 reference point, i.e.
 * T = code - 260.  Below or above that range the SOC cannot operate, so
 * the reading is clamped to the usable temperature extremes. */

#define RK3576_TSADC_CODE_MIN    220 /* code at -40 C  */
#define RK3576_TSADC_CODE_MAX    385 /* code at 125 C  */
#define RK3576_TSADC_TEMP_MIN    (-40)
#define RK3576_TSADC_TEMP_MAX    125
#define RK3576_TSADC_CODE_OFFSET 260

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One per-channel sensor instance.  struct sensor_lowerhalf_s MUST be the
 * first member so that a lowerhalf pointer cast to
 * struct rk3576_tsadc_channel_s* recovers the channel index. */

struct rk3576_tsadc_channel_s
{
  struct sensor_lowerhalf_s lower; /* Common lower-half interface  */
  int channel;                     /* TSADC channel index 0..5    */
};

struct rk3576_tsadc_priv_s
{
  uintptr_t base; /* TSADC register base (RK3576_TSADC_ADDR) */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_tsadc_fetch(FAR struct sensor_lowerhalf_s *lower,
                              FAR struct file *filep, FAR char *buffer,
                              size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct sensor_ops_s g_rk3576_tsadc_ops = {
  .fetch = rk3576_tsadc_fetch,
};

static struct rk3576_tsadc_priv_s g_rk3576_tsadc_priv = {
  .base = RK3576_TSADC_ADDR,
};

/* Per-channel sensor instances and the descriptor table handed to the
 * board.  Each descriptor pairs the lower-half handle with the name to
 * publish the node under.  The per-channel name matches the probe location
 * reported in TRM Table 19-1. */

static struct rk3576_tsadc_channel_s g_rk3576_tsadc_ch[RK3576_TSADC_NCHANNELS];

static const struct rk3576_tsadc_sensor_s g_rk3576_tsadc_sensors_init[] = {
  { NULL, "/dev/uorb/sensor_temp_soc_center" },
  { NULL, "/dev/uorb/sensor_temp_soc_bigcore" },
  { NULL, "/dev/uorb/sensor_temp_soc_litcore" },
  { NULL, "/dev/uorb/sensor_temp_soc_gpu" },
  { NULL, "/dev/uorb/sensor_temp_soc_npu" },
  { NULL, "/dev/uorb/sensor_temp_soc_ddr" },
};

static struct rk3576_tsadc_sensor_s
    g_rk3576_tsadc_sensors[RK3576_TSADC_NCHANNELS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_tsadc_getreg
 ****************************************************************************/

static inline uint32_t
rk3576_tsadc_getreg(FAR struct rk3576_tsadc_priv_s *priv, unsigned int offset)
{
  return getreg32(priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_tsadc_putreg
 ****************************************************************************/

static inline void rk3576_tsadc_putreg(FAR struct rk3576_tsadc_priv_s *priv,
                                       unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_tsadc_code_to_temp
 *
 * Description:
 *   Convert a 10-bit ADC code to a temperature in integer degrees Celsius
 *   and store it through *temp.  Within the SOC's normal operating range
 *   (code 220..385, i.e. -40..125 C) the TRM code -> temperature map is
 *   exactly linear at 1 C per code, so a single offset converts
 *   (T = code - 260).  Codes outside that range denote temperatures beyond
 *   the SOC's operating envelope and are clamped to the usable extremes.
 *
 * Input Parameters:
 *   code - Raw 10-bit ADC code.
 *   temp - Output: temperature in degrees Celsius.
 *
 * Returned Value:
 *   Always zero (OK); the conversion cannot fail.
 ****************************************************************************/

static int rk3576_tsadc_code_to_temp(uint32_t code, FAR int *temp)
{
  code &= RK3576_TSADC_DATA_MASK;

  /* Beyond the SOC's normal operating range the reading is meaningless;
   * clamp to the usable temperature extremes instead of extrapolating. */

  if (code < RK3576_TSADC_CODE_MIN)
    {
      *temp = RK3576_TSADC_TEMP_MIN;
      return OK;
    }

  if (code > RK3576_TSADC_CODE_MAX)
    {
      *temp = RK3576_TSADC_TEMP_MAX;
      return OK;
    }

  /* In range the TRM map is 1 C per code with 25 C at code 285,
   * so T = code - 260. */

  *temp = (int)code - RK3576_TSADC_CODE_OFFSET;
  return OK;
}

/****************************************************************************
 * Name: rk3576_tsadc_get_data
 *
 * Description:
 *   Read the latest auto-mode conversion result for a given channel index
 *   and store the converted temperature (integer degrees Celsius) through
 *   *temp.
 *
 * Input Parameters:
 *   priv    - TSADC private instance.
 *   channel - TSADC channel index (0..5).
 *   temp    - Output: temperature in degrees Celsius.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure (e.g. -EAGAIN when
 *   auto conversion is not yet running).
 ****************************************************************************/

static int rk3576_tsadc_get_data(FAR struct rk3576_tsadc_priv_s *priv,
                                 int channel, FAR int *temp)
{
  uint32_t status;
  uint32_t code;

  /* Auto mode must be running; if not, refuse to read a stale value. */

  status = rk3576_tsadc_getreg(priv, RK3576_TSADC_AUTO_STATUS);
  if (!(status & TSADC_AUTO_STATUS_IN_PROGRESS))
    {
      return -EAGAIN;
    }

  code =
      rk3576_tsadc_getreg(priv, RK3576_TSADC_DATA0 + (uintptr_t)channel * 4);

  return rk3576_tsadc_code_to_temp(code, temp);
}

/****************************************************************************
 * Name: rk3576_tsadc_fetch
 *
 * Description:
 *   sensor_ops_s::fetch implementation.  Called from the upper-half driver
 *   when an application reads from the sensor node; returns the latest
 *   temperature on the given TSADC channel as struct sensor_temp.
 ****************************************************************************/

static int rk3576_tsadc_fetch(FAR struct sensor_lowerhalf_s *lower,
                              FAR struct file *filep, FAR char *buffer,
                              size_t buflen)
{
  FAR struct rk3576_tsadc_channel_s *ch =
      (FAR struct rk3576_tsadc_channel_s *)lower;
  FAR struct sensor_temp *temp;
  struct timespec ts;
  int celsius;
  int ret;

  if (buflen != sizeof(struct sensor_temp))
    {
      return -EINVAL;
    }

  ret = rk3576_tsadc_get_data(&g_rk3576_tsadc_priv, ch->channel, &celsius);
  if (ret < 0)
    {
      return ret;
    }

  temp = (FAR struct sensor_temp *)buffer;

  /* struct sensor_temp::timestamp is in microseconds. */

  clock_systime_timespec(&ts);
  temp->timestamp =
      (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);

  temp->temperature = (float)celsius;
  return buflen;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_tsadc_initialize
 *
 * Description:
 *   Initialize the RK3576 TSADC, run it in auto mode, and hand back the
 *   array of per-channel on-die temperature sensors.  The board layer is
 *   responsible for calling sensor_custom_register() on each entry to
 *   publish the corresponding /dev/uorb/sensor_soc_* node.  The returned
 *   array is owned by the driver and valid for the system lifetime.
 *
 * Input Parameters:
 *   sensors - Output: pointer to the array of sensor descriptors.
 *   num     - Output: number of descriptors (i.e. number of TSADC channels).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 ****************************************************************************/

int rk3576_tsadc_initialize(FAR struct rk3576_tsadc_sensor_s **sensors,
                            FAR int *num)
{
  FAR struct rk3576_tsadc_priv_s *priv = &g_rk3576_tsadc_priv;
  struct clk_s *pclk;
  struct clk_s *clk;
  int i;
  int ret;

  /* Bring up the TSADC clocks via the CLK framework. */

  pclk = clk_get("pclk_tsadc");
  if (!pclk)
    {
      therr("TSADC: failed to get pclk_tsadc\n");
      return -ENODEV;
    }

  ret = clk_enable(pclk);
  if (ret < 0)
    {
      therr("TSADC: failed to enable pclk_tsadc: %d\n", ret);
      return ret;
    }

  clk = clk_get("clk_tsadc");
  if (!clk)
    {
      therr("TSADC: failed to get clk_tsadc\n");
      goto err_pclk;
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      therr("TSADC: failed to enable clk_tsadc: %d\n", ret);
      goto err_clk;
    }

  /* Ensure the conversion clock is 2 MHz.  The code->temperature table
   * and all timing registers assume a 2 MHz conversion clock, so a rate
   * change failure must abort instead of silently producing wrong values. */

  ret = clk_set_rate(clk, RK3576_TSADC_CLK_HZ);
  if (ret < 0)
    {
      therr("TSADC: failed to set clk_tsadc to %u Hz: %d\n",
            RK3576_TSADC_CLK_HZ, ret);
      goto err_clk;
    }

  /* TRM 19.5.2: CLK_DEM_TS must run at 2 MHz, divided from the 24 MHz OSC
   * (24 MHz / 12).  clk_set_rate() only reports success of the rate change;
   * it does not confirm the rate the divider actually produced.  The
   * conversion/indication timing register values below are tuned for
   * exactly 2 MHz, so read the resulting frequency back and abort if the
   * divider could not land on it. */

  if (clk_get_rate(clk) != RK3576_TSADC_CLK_HZ)
    {
      therr("TSADC: clk_tsadc is %lu Hz, expected %u Hz\n",
            (unsigned long)clk_get_rate(clk), RK3576_TSADC_CLK_HZ);
      goto err_clk;
    }

  /* Configure the auto-mode timing registers (TRM 19.4.3).  Default reset
   * values are already close, but set them explicitly for determinism. */

  rk3576_tsadc_putreg(priv, RK3576_TSADC_AUTO_PERIOD,
                      RK3576_TSADC_AUTO_PERIOD_VAL);
  rk3576_tsadc_putreg(priv, RK3576_TSADC_AUTO_PERIOD_HT,
                      RK3576_TSADC_AUTO_PERIOD_HT_VAL);
  rk3576_tsadc_putreg(priv, RK3576_TSADC_HIGH_INT_DEBOUNCE,
                      RK3576_TSADC_INT_DEBOUNCE);
  rk3576_tsadc_putreg(priv, RK3576_TSADC_HIGH_TSHUT_DEBOUNCE,
                      RK3576_TSADC_TSHUT_DEBOUNCE);

  /* Select all six channels for auto mode. */

  rk3576_tsadc_putreg(priv, RK3576_TSADC_AUTO_SRC,
                      TSADC_WRITE_MASKED(RK3576_TSADC_AUTO_SRC_ALL));

  /* Start auto conversion (AUTO_CON bit0 = 1).  Q_SEL (bit1) is left at 0
   * so the raw (positive temperature coefficient) output is used. */

  rk3576_tsadc_putreg(priv, RK3576_TSADC_AUTO_CON,
                      TSADC_WRITE_MASKED(TSADC_AUTO_CON_AUTO_EN));

  /* Wait for the first conversion round to complete so that fetch never
   * returns -EAGAIN immediately after bring-up. */

  up_udelay(RK3576_TSADC_STARTUP_DELAY_NS / 1000u);

  /* Set up one sensor lower-half instance per TSADC channel and pair it
   * with its published node name. */

  for (i = 0; i < RK3576_TSADC_NCHANNELS; i++)
    {
      g_rk3576_tsadc_ch[i].lower.type = SENSOR_TYPE_AMBIENT_TEMPERATURE;
      g_rk3576_tsadc_ch[i].lower.ops = &g_rk3576_tsadc_ops;
      g_rk3576_tsadc_ch[i].lower.nbuffer = 1;
      g_rk3576_tsadc_ch[i].channel = i;

      g_rk3576_tsadc_sensors[i].lower = &g_rk3576_tsadc_ch[i].lower;
      g_rk3576_tsadc_sensors[i].name = g_rk3576_tsadc_sensors_init[i].name;
    }

  *sensors = g_rk3576_tsadc_sensors;
  *num = RK3576_TSADC_NCHANNELS;

  sninfo("TSADC: %d on-die temperature sensors ready\n",
         RK3576_TSADC_NCHANNELS);

  return OK;

  /* Clock already enabled: undo it, then fall through to the pclk-only
   * cleanup.  This keeps every clk_enable() paired with exactly one
   * clk_disable() so the CLK framework's reference counts stay balanced. */

err_clk:
  clk_disable(clk);

err_pclk:
  clk_disable(pclk);
  return ret;
}

#endif /* CONFIG_RK3576_TSADC */
