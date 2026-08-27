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

#include <nuttx/audio/es8388.h>

#define AUDIOCTL_DEVICE       "/dev/audio/pcm0"
#define AUDIOCTL_INPUT_DEVICE "/dev/audio/pcm_in0"

struct audioctl_mode_s
{
  FAR const char *name;
  enum es8388_output_route_e route;
};

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

static void audioctl_usage(void)
{
  fputs("usage: audioctl [auto|headphones|speaker|both|off]\n"
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
  bool update = false;
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

  close(input_fd);
  close(fd);
  return EXIT_SUCCESS;
}
