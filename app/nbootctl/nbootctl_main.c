/****************************************************************************
 * apps/system/nbootctl/nbootctl_main.c
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

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/boardctl.h>

#include "nbootctl_bootctrl.h"

enum nbootctl_target_e
{
  NBOOTCTL_TARGET_CONSOLE = 1,
  NBOOTCTL_TARGET_FASTBOOT = 2,
  NBOOTCTL_TARGET_SLOT_A = 3,
  NBOOTCTL_TARGET_SLOT_B = 4,
};

/* Private Function Prototypes */

static const char *nbootctl_medium_name(unsigned int medium);
static const char *nbootctl_reason_name(unsigned int reason);
static int nbootctl_handoff(unsigned int *medium_out, unsigned int *slot_out);
static int nbootctl_status(void);
static int nbootctl_parse_slot(const char *value, unsigned int *slot);
static int nbootctl_reboot(enum nbootctl_target_e target);
static void nbootctl_usage(void);

/****************************************************************************
 * Name: nbootctl_medium_name
 ****************************************************************************/

static const char *nbootctl_medium_name(unsigned int medium)
{
  return medium == 1 ? "sd" : medium == 2 ? "emmc" : "unknown";
}

/****************************************************************************
 * Name: nbootctl_reason_name
 ****************************************************************************/

static const char *nbootctl_reason_name(unsigned int reason)
{
  return reason == 0   ? "normal"
         : reason == 1 ? "requested-slot"
         : reason == 2 ? "fallback"
                       : "unknown";
}

/****************************************************************************
 * Name: nbootctl_handoff
 ****************************************************************************/

static int nbootctl_handoff(unsigned int *medium_out, unsigned int *slot_out)
{
  uint64_t generation;
  unsigned int medium;
  unsigned int reason;
  unsigned int slot;
  int ret;

  /* The register read is shared with code that reports slot state without
   * a console; only the wording of a failure is this tool's own.
   */

  ret = nbootctl_handoff_read(&medium, &slot, &reason, &generation);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: %s\n",
              ret == -EBADMSG ? "invalid N-Boot handoff fields"
                              : "no valid N-Boot handoff");
      return 1;
    }

  printf("medium=%s\nslot=%c\ngeneration=%llu\nreason=%s\n",
         nbootctl_medium_name(medium), slot ? 'b' : 'a',
         (unsigned long long)generation, nbootctl_reason_name(reason));
  *medium_out = medium;
  *slot_out = slot;
  return 0;
}

/****************************************************************************
 * Name: nbootctl_status
 ****************************************************************************/

static int nbootctl_status(void)
{
  unsigned int medium;
  unsigned int slot;
  int ret;

  ret = nbootctl_handoff(&medium, &slot);
  if (ret != 0)
    {
      return ret;
    }

  return nbootctl_bootctrl_status(medium);
}

/****************************************************************************
 * Name: nbootctl_parse_slot
 ****************************************************************************/

static int nbootctl_parse_slot(const char *value, unsigned int *slot)
{
  if (strcmp(value, "a") == 0)
    {
      *slot = 0;
      return 0;
    }

  if (strcmp(value, "b") == 0)
    {
      *slot = 1;
      return 0;
    }

  return -1;
}

/****************************************************************************
 * Name: nbootctl_reboot
 ****************************************************************************/

static int nbootctl_reboot(enum nbootctl_target_e target)
{
  unsigned int medium;
  unsigned int slot;
  int ret;

  ret = nbootctl_handoff(&medium, &slot);
  if (ret != 0)
    {
      return 1;
    }

  ret = nbootctl_bootctrl_request(medium, target);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: reboot request failed: %d\n", ret);
      return 1;
    }

  printf("nbootctl: one-shot target %u stored\n", (unsigned int)target);
  fflush(stdout);
  __asm__ volatile("dsb sy" ::: "memory");
  boardctl(BOARDIOC_RESET, 0);
  nbootctl_bootctrl_request(medium, 0);
  fprintf(stderr, "nbootctl: reset returned unexpectedly\n");
  return 1;
}

/****************************************************************************
 * Name: nbootctl_usage
 ****************************************************************************/

static void nbootctl_usage(void)
{
  fprintf(stderr, "usage: nbootctl status\n"
                  "       nbootctl verify nuttx|amp a|b\n"
                  "       nbootctl set-active nuttx|amp a|b\n"
                  "       nbootctl mark-successful nuttx|amp a|b\n"
                  "       nbootctl stage nuttx|amp IMAGE\n"
                  "       nbootctl clone nuttx|amp a|b a|b\n"
                  "       nbootctl update-nboot IMAGE\n"
                  "       nbootctl reboot console|fastboot|nuttx-a|nuttx-b\n");
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  unsigned int medium;
  unsigned int running_slot;
  unsigned int slot;
  int ret;

  enum nbootctl_target_e target;

  if (argc == 2 && strcmp(argv[1], "status") == 0)
    {
      return nbootctl_status();
    }

  if (argc == 4 &&
      (strcmp(argv[1], "verify") == 0 || strcmp(argv[1], "set-active") == 0 ||
       strcmp(argv[1], "mark-successful") == 0))
    {
      ret = nbootctl_handoff(&medium, &running_slot);
      if (ret != 0 || nbootctl_parse_slot(argv[3], &slot) != 0)
        {
          nbootctl_usage();
          return 1;
        }

      if (strcmp(argv[1], "verify") == 0)
        {
          ret = nbootctl_bootctrl_verify(medium, argv[2], slot);
        }
      else if (strcmp(argv[1], "set-active") == 0)
        {
          ret = nbootctl_bootctrl_set_active(medium, argv[2], slot);
        }
      else
        {
          ret = nbootctl_bootctrl_mark_successful(medium, argv[2], slot);
        }
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: %s failed: %d\n", argv[1], ret);
          return 1;
        }
      printf("%s %s %c: OK\n", argv[1], argv[2], slot ? 'b' : 'a');
      return 0;
    }

  if (argc == 4 && strcmp(argv[1], "stage") == 0)
    {
      ret = nbootctl_handoff(&medium, &running_slot);
      if (ret != 0)
        {
          return 1;
        }

      ret = nbootctl_bootctrl_stage(medium, argv[2], running_slot, argv[3]);
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: stage failed: %d\n", ret);
          return 1;
        }

      return 0;
    }

  if (argc == 5 && strcmp(argv[1], "clone") == 0)
    {
      unsigned int target_slot;

      ret = nbootctl_handoff(&medium, &running_slot);
      if (ret != 0 || nbootctl_parse_slot(argv[3], &slot) != 0 ||
          nbootctl_parse_slot(argv[4], &target_slot) != 0)
        {
          nbootctl_usage();
          return 1;
        }

      ret = nbootctl_bootctrl_clone(medium, argv[2], slot, target_slot);
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: clone failed: %d\n", ret);
          return 1;
        }

      return 0;
    }

  if (argc == 3 && strcmp(argv[1], "update-nboot") == 0)
    {
      ret = nbootctl_handoff(&medium, &running_slot);
      if (ret != 0)
        {
          return 1;
        }

      ret = nbootctl_update_nboot(medium, argv[2]);
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: update-nboot failed: %d\n", ret);
          return 1;
        }

      return 0;
    }

  if (argc != 3 || strcmp(argv[1], "reboot") != 0)
    {
      nbootctl_usage();
      return 1;
    }

  if (strcmp(argv[2], "console") == 0)
    {
      target = NBOOTCTL_TARGET_CONSOLE;
    }
  else if (strcmp(argv[2], "fastboot") == 0)
    {
      target = NBOOTCTL_TARGET_FASTBOOT;
    }
  else if (strcmp(argv[2], "nuttx-a") == 0)
    {
      target = NBOOTCTL_TARGET_SLOT_A;
    }
  else if (strcmp(argv[2], "nuttx-b") == 0)
    {
      target = NBOOTCTL_TARGET_SLOT_B;
    }
  else
    {
      nbootctl_usage();
      return 1;
    }

  return nbootctl_reboot(target);
}
