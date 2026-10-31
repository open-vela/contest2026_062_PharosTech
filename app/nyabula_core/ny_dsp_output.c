/****************************************************************************
 * app/nyabula_core/ny_dsp_output.c
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

#include "ny_dsp_output.h"
#include "ny_product_audio.h"
#include "ny_product_dsp.h"
#include "nyadsp_clock.h"
#include "nyadsp_resample.h"
#ifdef CONFIG_NYABULA_CORE_BT
#include "ny_product_bt.h"
#endif
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NY_DSP_OUTPUT_FRAMES      256
#define NY_DSP_OUTPUT_BUFFERS     8
#define NY_DSP_OUTPUT_USB_BUFFERS (NY_DSP_OUTPUT_BUFFERS + 2)
#define NY_DSP_OUTPUT_USB_FRAMES  (NY_DSP_OUTPUT_FRAMES + 8)
#define NY_DSP_OUTPUT_TAIL        (NYADSP_MAX_DELAY_FRAMES + 2048)

struct ny_dsp_output_s
{
  struct ny_audio_stream_s *satellite;
  struct ny_audio_stream_s *bass;
  struct nyadsp_s *dsp;
  struct nyadsp_s *pending;
  struct nyadsp_resample_s *src;
  struct nyadsp_clock_s clock;
  uint32_t revision;
  uint32_t rate;
  uint8_t channels;
  size_t frames;
  size_t warming;
  int error;
  bool owned;
  float bass_gain;
  float input[NY_DSP_OUTPUT_FRAMES * 2];
  float output[NY_DSP_OUTPUT_FRAMES * 3];
  float previous[NY_DSP_OUTPUT_FRAMES * 3];
  float mono[NY_DSP_OUTPUT_USB_FRAMES];
  int16_t satellites[NY_DSP_OUTPUT_FRAMES * 2];
  int16_t subwoofer[NY_DSP_OUTPUT_USB_FRAMES * 2];
};

struct nyadsp_s *
ny_dsp_output_create(const struct ny_product_dsp_settings_s *settings);
static int ny_dsp_output_open_bass(struct ny_dsp_output_s *output);
static int16_t ny_dsp_output_s16(float sample);
static int ny_dsp_output_block(struct ny_dsp_output_s *output, int timeout_ms);
static int ny_dsp_output_frame(struct ny_dsp_output_s *output,
                               const float input[2], int timeout_ms);

/****************************************************************************
 * Name: ny_dsp_output_create
 ****************************************************************************/

struct nyadsp_s *
ny_dsp_output_create(const struct ny_product_dsp_settings_s *settings)
{
  enum nyadsp_filter_e kinds[NYADSP_INPUT_EQ_BANDS];
  size_t i;
  for (i = 0; i < NYADSP_INPUT_EQ_BANDS; i++)
    {
      kinds[i] = settings->eq_enabled && settings->enabled[i]
                     ? settings->kinds[i]
                     : NYADSP_FILTER_OFF;
    }

  return nyadsp_create_input_eq_split(
      &settings->filter, settings->input, kinds, NYADSP_INPUT_EQ_BANDS,
      &settings->crossover, !settings->bass, NULL);
}

/****************************************************************************
 * Name: ny_dsp_output_open_bass
 ****************************************************************************/

static int ny_dsp_output_open_bass(struct ny_dsp_output_s *output)
{
  struct ny_audio_stream_config_s stream;
  size_t queued;
  int ret;

  memset(&stream, 0, sizeof(stream));
  stream.path = CONFIG_NYABULA_CORE_DSP_BASS_DEVICE;
  stream.rate = NYADSP_SAMPLE_RATE;
  stream.channels = 2;
  stream.chunk = NY_DSP_OUTPUT_FRAMES * 4;
  stream.buffers = NY_DSP_OUTPUT_USB_BUFFERS;
  stream.prefill = 4;
  ret = ny_audio_stream_open(&output->bass, &stream);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_audio_stream_unity(output->bass);
  if (ret < 0)
    {
      ny_audio_stream_close(output->bass);
      output->bass = NULL;
      return ret;
    }

  /* A newly attached output joins the existing presentation timeline.
   * Keep it open across later on/off switches: silence, not device restart.
   */

  queued = ny_audio_stream_queued(output->satellite);
  memset(output->subwoofer, 0, sizeof(output->subwoofer));
  while (queued > 0)
    {
      size_t bytes = queued < NY_DSP_OUTPUT_FRAMES * 4
                         ? queued
                         : NY_DSP_OUTPUT_FRAMES * 4;
      ssize_t written =
          ny_audio_stream_write(output->bass, output->subwoofer, bytes, 100);
      if (written != bytes)
        {
          ny_audio_stream_close(output->bass);
          output->bass = NULL;
          return written < 0 ? written : -ETIMEDOUT;
        }
      queued -= bytes;
    }

  nyadsp_clock_init(&output->clock);
  return 0;
}

/****************************************************************************
 * Name: ny_dsp_output_s16
 ****************************************************************************/

static int16_t ny_dsp_output_s16(float sample)
{
  return (int16_t)lrintf(fminf(32767, fmaxf(-32768, sample * 32768)));
}

/****************************************************************************
 * Name: ny_dsp_output_block
 ****************************************************************************/

static int ny_dsp_output_block(struct ny_dsp_output_s *output, int timeout_ms)
{
  struct ny_product_dsp_settings_s config;
  struct nyadsp_stats_s stats;
  struct ny_audio_stream_stats_s usb;
  uint32_t revision;
  size_t i;
  ssize_t written;
  size_t frames = output->frames;

#ifdef CONFIG_NYABULA_CORE_BT
  if (ny_product_bt_call_active())
    {
      return -EINTR;
    }
#endif
  struct nyadsp_s *prepared = ny_product_dsp_take_update(&config, &revision);
  if (prepared != NULL)
    {
      /* The control thread prepared this filter. Warm it before crossfade. */

      nyadsp_destroy(output->pending);
      output->pending = prepared;
      if (config.bass && output->bass == NULL)
        {
          int ret = ny_dsp_output_open_bass(output);
          if (ret < 0)
            {
              return ret;
            }
        }

      output->revision = revision;
      output->warming = 2048;
      for (i = 0; i < NYADSP_OUTPUTS; i++)
        {
          size_t tail = config.filter.output[i].delay_frames + 2048;
          if (tail > output->warming)
            {
              output->warming = tail;
            }
        }
    }

  if (nyadsp_process(output->dsp, output->input, output->output, frames) < 0)
    {
      return -EINVAL;
    }

  if (output->pending != NULL)
    {
      nyadsp_process(output->pending, output->input, output->previous, frames);
      if (output->warming > 0)
        {
          output->warming =
              output->warming > frames ? output->warming - frames : 0;
        }
      else
        {
          for (i = 0; i < frames * 3; i++)
            {
              float mix = (float)(i / 3 + 1) / frames;
              output->output[i] +=
                  (output->previous[i] - output->output[i]) * mix;
            }

          nyadsp_destroy(output->dsp);
          output->dsp = output->pending;
          output->pending = NULL;
        }
    }

  for (i = 0; i < frames; i++)
    {
      output->satellites[2 * i] = ny_dsp_output_s16(output->output[3 * i]);
      output->satellites[2 * i + 1] =
          ny_dsp_output_s16(output->output[3 * i + 1]);
    }

  /* Only the satellite device paces production. Compare outstanding audio
   * at the same source-frame boundary, not USB capacity against a constant.
   */

  written = ny_audio_stream_write(output->satellite, output->satellites,
                                  frames * 4, timeout_ms);
  if (written != frames * 4)
    {
      return written < 0 ? written : -ETIMEDOUT;
    }

  if (output->bass != NULL)
    {
      size_t target = ny_audio_stream_queued(output->satellite) / 4;
      target = target > frames ? target - frames : 1;
      size_t count = nyadsp_clock_process(
          &output->clock, output->output, frames, output->mono,
          NY_DSP_OUTPUT_USB_FRAMES, ny_audio_stream_queued(output->bass) / 4,
          target);
      float gain = 1.0f;
#ifdef CONFIG_NYABULA_CORE_AUDIO
      gain = ny_product_audio_output_level() / 1000.0f;
#endif
      for (i = 0; i < count; i++)
        {
          /* Muting is exact; other volume changes ramp over one block. */
          float level = gain == 0
                            ? 0
                            : output->bass_gain +
                                  (gain - output->bass_gain) * (i + 1) / count;
          output->subwoofer[2 * i] = output->subwoofer[2 * i + 1] =
              ny_dsp_output_s16(output->mono[i] * level);
        }
      output->bass_gain = gain;

      written = ny_audio_stream_write(output->bass, output->subwoofer,
                                      count * 4, timeout_ms);
      if (written != count * 4)
        {
          return written < 0 ? written : -ETIMEDOUT;
        }
    }

  nyadsp_get_stats(output->dsp, &stats);
  ny_audio_stream_stats(output->bass, &usb);
  ny_product_dsp_progress(frames, stats.output_peak,
                          (output->clock.ratio - 1) * 1000000, usb.starved,
                          ny_audio_stream_queued(output->satellite) / 4,
                          ny_audio_stream_queued(output->bass) / 4);
  output->frames = 0;
  return 0;
}

/****************************************************************************
 * Name: ny_dsp_output_frame
 ****************************************************************************/

static int ny_dsp_output_frame(struct ny_dsp_output_s *output,
                               const float input[2], int timeout_ms)
{
  float converted[NYADSP_RESAMPLE_MAX_OUTPUT * 2];
  size_t count = nyadsp_resample_push(output->src, input, converted);
  size_t i;
  for (i = 0; i < count; i++)
    {
      /* Sinc interpolation can overshoot legal input sample peaks. */

      output->input[2 * output->frames] =
          fmaxf(-1, fminf(1, converted[2 * i]));
      output->input[2 * output->frames + 1] =
          fmaxf(-1, fminf(1, converted[2 * i + 1]));
      output->frames++;
      if (output->frames == NY_DSP_OUTPUT_FRAMES)
        {
          int ret = ny_dsp_output_block(output, timeout_ms);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return 0;
}

/****************************************************************************
 * Name: ny_dsp_output_open
 ****************************************************************************/

int ny_dsp_output_open(struct ny_dsp_output_s **result,
                       const struct ny_audio_stream_config_s *config)
{
  struct ny_dsp_output_s *output = calloc(1, sizeof(*output));
  struct ny_product_dsp_settings_s settings;
  struct ny_audio_stream_config_s stream;
  int ret;
  *result = NULL;
  if (output == NULL)
    {
      return -ENOMEM;
    }

  output->rate = config->rate;
  output->channels = config->channels;
  output->src = nyadsp_resample_create(config->rate);
  if (output->src == NULL)
    {
      ret = -ENOTSUP;
      goto fail;
    }

  ret =
      ny_product_dsp_acquire(config->dsp_source, &settings, &output->revision);
  if (ret < 0)
    {
      goto fail;
    }

  output->owned = true;
  output->dsp = ny_dsp_output_create(&settings);
  if (output->dsp == NULL)
    {
      ret = -ENOMEM;
      goto fail;
    }

  memset(&stream, 0, sizeof(stream));
  stream.path = config->path;
  stream.wav = config->wav;
  stream.rate = NYADSP_SAMPLE_RATE;
  stream.channels = 2;
  stream.chunk = NY_DSP_OUTPUT_FRAMES * 4;
  stream.buffers = NY_DSP_OUTPUT_BUFFERS;
  stream.prefill = 4;
  ret = ny_audio_stream_open(&output->satellite, &stream);
  if (ret < 0)
    {
      goto fail;
    }

  if (settings.bass)
    {
      ret = ny_dsp_output_open_bass(output);
      if (ret < 0)
        {
          goto fail;
        }
    }

  nyadsp_clock_init(&output->clock);
  *result = output;
  return 0;
fail:
  output->error = ret;
  ny_dsp_output_close(output);
  return ret;
}

/****************************************************************************
 * Name: ny_dsp_output_write
 ****************************************************************************/

ssize_t ny_dsp_output_write(struct ny_dsp_output_s *output, const void *pcm,
                            size_t bytes, int timeout_ms)
{
  const int16_t *samples = pcm;
  size_t frame;
  size_t frames = bytes / (output->channels * 2);
  if (output->error < 0)
    {
      return output->error;
    }

  if (bytes % (output->channels * 2) != 0)
    {
      return -EINVAL;
    }

  for (frame = 0; frame < frames; frame++)
    {
      float input[2];
      input[0] = samples[frame * output->channels] / 32768.0f;
      input[1] =
          samples[frame * output->channels + output->channels - 1] / 32768.0f;
      output->error = ny_dsp_output_frame(output, input, timeout_ms);
      if (output->error < 0)
        {
          return output->error;
        }
    }

  return bytes;
}

/****************************************************************************
 * Name: ny_dsp_output_queued
 ****************************************************************************/

size_t ny_dsp_output_queued(struct ny_dsp_output_s *output)
{
  size_t frames =
      ny_audio_stream_queued(output->satellite) / 4 + output->frames;
  /* Keep the caller's original byte/time contract (including mono TTS). */

  return (uint64_t)frames * output->rate * output->channels * 2 /
         NYADSP_SAMPLE_RATE;
}

/****************************************************************************
 * Name: ny_dsp_output_drain
 ****************************************************************************/

int ny_dsp_output_drain(struct ny_dsp_output_s *output, int timeout_ms)
{
  float zero[2] = { 0, 0 };
  size_t i;
  int ret;
  if (output->error < 0)
    {
      return output->error;
    }

  for (i = 0; i < 48; i++)
    {
      ret = ny_dsp_output_frame(output, zero, timeout_ms);
      if (ret < 0)
        {
          output->error = ret;
          return ret;
        }
    }

  for (i = 0; i < NY_DSP_OUTPUT_TAIL; i++)
    {
      output->input[2 * output->frames] = 0;
      output->input[2 * output->frames + 1] = 0;
      output->frames++;
      if (output->frames == NY_DSP_OUTPUT_FRAMES)
        {
          ret = ny_dsp_output_block(output, timeout_ms);
          if (ret < 0)
            {
              output->error = ret;
              return ret;
            }
        }
    }

  if (output->frames > 0)
    {
      ret = ny_dsp_output_block(output, timeout_ms);
      if (ret < 0)
        {
          output->error = ret;
          return ret;
        }
    }

  ret = ny_audio_stream_drain(output->satellite, timeout_ms);
  if (ret == 0 && output->bass != NULL)
    {
      ret = ny_audio_stream_drain(output->bass, timeout_ms);
    }

  output->error = ret;
  return ret;
}

/****************************************************************************
 * Name: ny_dsp_output_stats
 ****************************************************************************/

void ny_dsp_output_stats(struct ny_dsp_output_s *output,
                         struct ny_audio_stream_stats_s *stats)
{
  ny_audio_stream_stats(output->satellite, stats);
}

/****************************************************************************
 * Name: ny_dsp_output_close
 ****************************************************************************/

void ny_dsp_output_close(struct ny_dsp_output_s *output)
{
  if (output == NULL)
    {
      return;
    }

  ny_audio_stream_close(output->bass);
  ny_audio_stream_close(output->satellite);
  nyadsp_destroy(output->dsp);
  nyadsp_destroy(output->pending);
  nyadsp_resample_destroy(output->src);
  if (output->owned)
    {
      ny_product_dsp_release(output->error);
    }

  free(output);
}

/****************************************************************************
 * Name: ny_dsp_output_pause
 ****************************************************************************/

int ny_dsp_output_pause(struct ny_dsp_output_s *output, bool paused)
{
  /* The player serializes this call with writes and stops producing while
   * paused. Let both bounded queues finish together instead of pausing two
   * unrelated hardware clocks at different sample positions. In particular,
   * USB Audio does not expose a lower-half pause callback. DSP/filter history
   * and every queued sample remain intact for the next write on resume.
   */

  (void)output;
  (void)paused;
  return 0;
}
