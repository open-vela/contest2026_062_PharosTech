/****************************************************************************
 * app/audioctl/audioctl_main.c
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
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8388.h>

#ifdef CONFIG_NYABULA_CORE_AUDIO
#  include "ny_product_audio.h"
#endif

#define AUDIOCTL_DEVICE       "/dev/audio/pcm0"
#define AUDIOCTL_INPUT_DEVICE "/dev/audio/pcm_in0"

/* A level that a command does not change. */

#ifdef CONFIG_NYABULA_CORE_AUDIO
#  define AUDIOCTL_KEEP       NY_PRODUCT_AUDIO_KEEP
#else
#  define AUDIOCTL_KEEP       (-1)

/* AUDIO_FU_VOLUME runs 0..1000; the driver takes the microphone gain in dB,
 * 0..24 in 3 dB steps.
 */

#  define AUDIOCTL_VOLUME_SCALE 10
#  define AUDIOCTL_GAIN_DB_MAX  24
#  define AUDIOCTL_GAIN_DB_STEP 3
#endif

struct audioctl_mode_s
{
  FAR const char *name;
  enum es8388_output_route_e route;
};

static FAR const char *audioctl_route_name(enum es8388_output_route_e route);
static FAR const char *audioctl_input_name(enum es8388_input_route_e route);
static int audioctl_parse_input(FAR const char *name,
                                FAR enum es8388_input_route_e *route);
static int audioctl_parse_percent(FAR const char *text, FAR int *value);
static int audioctl_parse_switch(FAR const char *text, FAR int *value);
#ifndef CONFIG_NYABULA_CORE_AUDIO
static int audioctl_feature(int fd, uint16_t unit, uint16_t value);
#endif
static int audioctl_set_levels(int fd, int input_fd, int volume, int muted,
                               int gain);
static void audioctl_print_levels(void);
static void audioctl_usage(void);

static const struct audioctl_mode_s g_audioctl_modes[] = {
  { "auto", ES8388_OUTPUT_ROUTE_AUTO },
  { "headphones", ES8388_OUTPUT_ROUTE_LINE1 },
  { "speaker", ES8388_OUTPUT_ROUTE_LINE2 },
  { "both", ES8388_OUTPUT_ROUTE_BOTH },
  { "off", ES8388_OUTPUT_ROUTE_NONE },
};

static FAR const char *audioctl_route_name(enum es8388_output_route_e route)
{
  size_t index;

  for (index = 0;
       index < sizeof(g_audioctl_modes) / sizeof(g_audioctl_modes[0]); index++)
    {
      if (g_audioctl_modes[index].route == route)
        {
          return g_audioctl_modes[index].name;
        }
    }

  return "unknown";
}

static FAR const char *audioctl_input_name(enum es8388_input_route_e route)
{
  switch (route)
    {
      case ES8388_INPUT_ROUTE_NONE:
        return "off";
      case ES8388_INPUT_ROUTE_LINE1:
        return "headset";
      case ES8388_INPUT_ROUTE_LINE2:
        return "main";
      case ES8388_INPUT_ROUTE_BOTH:
        return "both";
      default:
        return "unknown";
    }
}

static int audioctl_parse_input(FAR const char *name,
                                FAR enum es8388_input_route_e *route)
{
  if (strcmp(name, "main") == 0)
    {
      *route = ES8388_INPUT_ROUTE_LINE2;
    }
  else if (strcmp(name, "headset") == 0)
    {
      *route = ES8388_INPUT_ROUTE_LINE1;
    }
  else if (strcmp(name, "both") == 0)
    {
      *route = ES8388_INPUT_ROUTE_BOTH;
    }
  else if (strcmp(name, "off") == 0)
    {
      *route = ES8388_INPUT_ROUTE_NONE;
    }
  else
    {
      return -EINVAL;
    }

  return 0;
}

static int audioctl_parse_percent(FAR const char *text, FAR int *value)
{
  FAR char *end;
  long parsed = strtol(text, &end, 10);

  if (end == text || *end != '\0' || parsed < 0 || parsed > 100)
    {
      return -EINVAL;
    }

  *value = (int)parsed;
  return 0;
}

static int audioctl_parse_switch(FAR const char *text, FAR int *value)
{
  if (strcmp(text, "on") == 0)
    {
      *value = 1;
    }
  else if (strcmp(text, "off") == 0)
    {
      *value = 0;
    }
  else
    {
      return -EINVAL;
    }

  return 0;
}

#ifndef CONFIG_NYABULA_CORE_AUDIO
static int audioctl_feature(int fd, uint16_t unit, uint16_t value)
{
  struct audio_caps_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len = sizeof(struct audio_caps_s);
  desc.caps.ac_type = AUDIO_TYPE_FEATURE;
  desc.caps.ac_format.hw = unit;
  desc.caps.ac_controls.hw[0] = value;
  return ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&desc) < 0
             ? -errno
             : 0;
}
#endif

/****************************************************************************
 * Name: audioctl_set_levels
 *
 * Description:
 *   The ES8388 driver cannot be asked for its volume, mute or microphone
 *   gain.  With the Nyabula audio service built in, the levels go through
 *   it: it writes the codec, remembers what it wrote and keeps the control
 *   panel on the same numbers.  Without it they are written to the codec
 *   directly and nothing remembers them.
 *
 ****************************************************************************/

static int audioctl_set_levels(int fd, int input_fd, int volume, int muted,
                               int gain)
{
#ifdef CONFIG_NYABULA_CORE_AUDIO
  (void)fd;
  (void)input_fd;
  return ny_product_audio_set_levels(volume, muted, gain);
#else
  int ret = 0;

  if (volume != AUDIOCTL_KEEP)
    {
      ret = audioctl_feature(fd, AUDIO_FU_VOLUME,
                             (uint16_t)(volume * AUDIOCTL_VOLUME_SCALE));
    }

  /* The driver releases the DAC mute by itself when the next stream starts:
   * without the service this mute only lasts for the current one.
   */

  if (ret == 0 && muted != AUDIOCTL_KEEP)
    {
      ret = audioctl_feature(fd, AUDIO_FU_MUTE, (uint16_t)muted);
    }

  if (ret == 0 && gain != AUDIOCTL_KEEP)
    {
      int steps = AUDIOCTL_GAIN_DB_MAX / AUDIOCTL_GAIN_DB_STEP;
      int db = (gain * steps + 50) / 100 * AUDIOCTL_GAIN_DB_STEP;

      ret = audioctl_feature(input_fd, AUDIO_FU_INP_GAIN, (uint16_t)db);
    }

  return ret;
#endif
}

static void audioctl_print_levels(void)
{
#ifdef CONFIG_NYABULA_CORE_AUDIO
  struct ny_product_audio_levels_s levels;

  if (ny_product_audio_levels(&levels) == 0)
    {
      printf("volume=%d mute=%s mic_gain=%d mic_gain_db=%d\n", levels.volume,
             levels.muted ? "on" : "off", levels.gain, levels.gain_db);
      return;
    }
#endif

  /* No readback in the driver and nobody kept a record. */

  printf("volume=n/a mute=n/a mic_gain=n/a\n");
}

static void audioctl_usage(void)
{
  fputs("usage: audioctl [auto|headphones|speaker|both|off]\n"
        "       audioctl volume <0-100>\n"
        "       audioctl mute [on|off]\n"
        "       audioctl mic gain <0-100>\n"
        "       audioctl mono [on|off]\n"
        "       audioctl swap [on|off]\n"
        "       audioctl polarity [left|right] [normal|invert]\n"
        "       audioctl mic [main|headset|both|off]\n"
        "       audioctl mic mute [on|off]\n",
        stderr);
}

int main(int argc, FAR char *argv[])
{
  struct es8388_control_s control;
  struct es8388_control_s input_control;
  bool input_update = false;
  bool level_update = false;
  bool update = false;
  int volume = AUDIOCTL_KEEP;
  int muted = AUDIOCTL_KEEP;
  int gain = AUDIOCTL_KEEP;
  size_t index;
  int input_fd;
  int fd;

  fd = open(AUDIOCTL_DEVICE, O_RDWR);
  if (fd < 0)
    {
      fprintf(stderr, "audioctl: open %s failed: %d\n", AUDIOCTL_DEVICE,
              errno);
      return EXIT_FAILURE;
    }

  input_fd = open(AUDIOCTL_INPUT_DEVICE, O_RDWR);
  if (input_fd < 0)
    {
      fprintf(stderr, "audioctl: open %s failed: %d\n", AUDIOCTL_INPUT_DEVICE,
              errno);
      close(fd);
      return EXIT_FAILURE;
    }

  if (ioctl(fd, ES8388IOC_GET_CONTROL, (unsigned long)(uintptr_t)&control) < 0)
    {
      fprintf(stderr, "audioctl: get controls failed: %d\n", errno);
      close(input_fd);
      close(fd);
      return EXIT_FAILURE;
    }

  if (ioctl(input_fd, ES8388IOC_GET_CONTROL,
            (unsigned long)(uintptr_t)&input_control) < 0)
    {
      fprintf(stderr, "audioctl: get input controls failed: %d\n", errno);
      close(input_fd);
      close(fd);
      return EXIT_FAILURE;
    }

  if (argc == 2)
    {
      for (index = 0;
           index < sizeof(g_audioctl_modes) / sizeof(g_audioctl_modes[0]);
           index++)
        {
          if (strcmp(argv[1], g_audioctl_modes[index].name) == 0)
            {
              control.route = g_audioctl_modes[index].route;
              update = true;
              break;
            }
        }
    }
  else if ((argc == 3 && strcmp(argv[1], "volume") == 0) ||
           (argc == 3 && strcmp(argv[1], "mute") == 0) ||
           (argc == 4 && strcmp(argv[1], "mic") == 0 &&
            strcmp(argv[2], "gain") == 0))
    {
      int ret = argc == 4 ? audioctl_parse_percent(argv[3], &gain)
                : strcmp(argv[1], "mute") == 0
                    ? audioctl_parse_switch(argv[2], &muted)
                    : audioctl_parse_percent(argv[2], &volume);
      if (ret < 0)
        {
          audioctl_usage();
          close(input_fd);
          close(fd);
          return EXIT_FAILURE;
        }

      level_update = true;
    }
  else if (argc == 3 && strcmp(argv[1], "mono") == 0)
    {
      if (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)
        {
          audioctl_usage();
          close(fd);
          return EXIT_FAILURE;
        }

      control.mono = strcmp(argv[2], "on") == 0;
      update = true;
    }
  else if (argc == 3 && strcmp(argv[1], "swap") == 0)
    {
      if (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)
        {
          audioctl_usage();
          close(fd);
          return EXIT_FAILURE;
        }

      control.swap = strcmp(argv[2], "on") == 0;
      update = true;
    }
  else if (argc == 3 && strcmp(argv[1], "mic") == 0)
    {
      if (audioctl_parse_input(argv[2], &input_control.input_route) < 0)
        {
          audioctl_usage();
          close(fd);
          return EXIT_FAILURE;
        }

      input_update = true;
    }
  else if (argc == 4 && strcmp(argv[1], "mic") == 0 &&
           strcmp(argv[2], "mute") == 0)
    {
      if (strcmp(argv[3], "on") != 0 && strcmp(argv[3], "off") != 0)
        {
          audioctl_usage();
          close(fd);
          return EXIT_FAILURE;
        }

      input_control.microphone_muted = strcmp(argv[3], "on") == 0;
      input_update = true;
    }
  else if (argc == 4 && strcmp(argv[1], "polarity") == 0)
    {
      bool invert;

      if ((strcmp(argv[2], "left") != 0 && strcmp(argv[2], "right") != 0) ||
          (strcmp(argv[3], "normal") != 0 && strcmp(argv[3], "invert") != 0))
        {
          audioctl_usage();
          close(fd);
          return EXIT_FAILURE;
        }

      invert = strcmp(argv[3], "invert") == 0;
      if (strcmp(argv[2], "left") == 0)
        {
          control.invert_left = invert;
        }
      else
        {
          control.invert_right = invert;
        }

      update = true;
    }
  else if (argc != 1)
    {
      audioctl_usage();
      close(fd);
      return EXIT_FAILURE;
    }

  if (argc == 2 && !update)
    {
      audioctl_usage();
      close(fd);
      return EXIT_FAILURE;
    }

  if (level_update)
    {
      int ret = audioctl_set_levels(fd, input_fd, volume, muted, gain);
      if (ret < 0)
        {
          fprintf(stderr, "audioctl: set levels failed: %d\n", -ret);
          close(input_fd);
          close(fd);
          return EXIT_FAILURE;
        }
    }

  if (input_update)
    {
      input_control.mask = ES8388_CONTROL_INPUT;
      if (ioctl(input_fd, ES8388IOC_SET_CONTROL,
                (unsigned long)(uintptr_t)&input_control) < 0)
        {
          fprintf(stderr, "audioctl: set input controls failed: %d\n", errno);
          close(input_fd);
          close(fd);
          return EXIT_FAILURE;
        }
    }

  if (update)
    {
      control.mask = ES8388_CONTROL_OUTPUT;
      if (ioctl(fd, ES8388IOC_SET_CONTROL,
                (unsigned long)(uintptr_t)&control) < 0)
        {
          fprintf(stderr, "audioctl: set controls failed: %d\n", errno);
          close(input_fd);
          close(fd);
          return EXIT_FAILURE;
        }
    }

  if (ioctl(input_fd, ES8388IOC_GET_CONTROL,
            (unsigned long)(uintptr_t)&input_control) < 0)
    {
      fprintf(stderr, "audioctl: refresh input controls failed: %d\n", errno);
      close(input_fd);
      close(fd);
      return EXIT_FAILURE;
    }

  if (ioctl(fd, ES8388IOC_GET_CONTROL, (unsigned long)(uintptr_t)&control) < 0)
    {
      fprintf(stderr, "audioctl: refresh controls failed: %d\n", errno);
      close(fd);
      return EXIT_FAILURE;
    }

  printf("mode=%s active=%s channel=%s swap=%s polarity_l=%s "
         "polarity_r=%s mic=%s mic_mute=%s headphones=%s\n",
         audioctl_route_name(control.route),
         audioctl_route_name(control.active_route),
         control.mono ? "mono" : "stereo", control.swap ? "on" : "off",
         control.invert_left ? "invert" : "normal",
         control.invert_right ? "invert" : "normal",
         audioctl_input_name(input_control.input_route),
         input_control.microphone_muted ? "on" : "off",
         control.headphones_connected ? "connected" : "disconnected");
  audioctl_print_levels();

  close(input_fd);
  close(fd);
  return EXIT_SUCCESS;
}
