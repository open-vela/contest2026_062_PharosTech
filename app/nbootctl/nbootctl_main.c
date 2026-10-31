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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/boardctl.h>

#include "nbootctl_bootctrl.h"
#include "nbootctl_part.h"

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
static int nbootctl_handoff(unsigned int *medium_out,
                            unsigned int *domain_out,
                            unsigned int *slot_out);
static int nbootctl_status(void);
static int nbootctl_parse_slot(const char *value, unsigned int *slot);
static int nbootctl_parse_u64(const char *value, uint64_t *out);
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
         : reason == 3 ? "ram"
                       : "unknown";
}

/****************************************************************************
 * Name: nbootctl_handoff
 ****************************************************************************/

static int nbootctl_handoff(unsigned int *medium_out,
                            unsigned int *domain_out,
                            unsigned int *slot_out)
{
  uint64_t generation;
  unsigned int medium;
  unsigned int domain;
  unsigned int reason;
  unsigned int slot;
  int ret;

  /* The register read is shared with code that reports slot state without
   * a console; only the wording of a failure is this tool's own.
   */

  ret = nbootctl_handoff_read(&medium, &domain, &slot, &reason, &generation);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: %s\n",
              ret == -EBADMSG ? "invalid N-Boot handoff fields"
                              : "no valid N-Boot handoff");
      return 1;
    }

  /* slot is a slot of domain.  An AMP image started from RAM runs from no
   * slot at all.
   */

  printf("medium=%s\ndomain=%s\nslot=%s\ngeneration=%llu\nreason=%s\n",
         nbootctl_medium_name(medium),
         domain == NBOOTCTL_DOMAIN_AMP ? "amp" : "nuttx",
         slot == 0 ? "a" : slot == 1 ? "b" : "none",
         (unsigned long long)generation, nbootctl_reason_name(reason));
  *medium_out = medium;
  *domain_out = domain;
  *slot_out = slot;
  return 0;
}

/****************************************************************************
 * Name: nbootctl_status
 ****************************************************************************/

static int nbootctl_status(void)
{
  unsigned int medium;
  unsigned int domain;
  unsigned int slot;
  int ret;

  ret = nbootctl_handoff(&medium, &domain, &slot);
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
 * Name: nbootctl_parse_u64
 *
 * Description:
 *   A sector number or count, decimal or 0x-prefixed.  An empty string and
 *   trailing text are errors: a half-parsed LBA is a write to the wrong
 *   place.
 *
 ****************************************************************************/

static int nbootctl_parse_u64(const char *value, uint64_t *out)
{
  char *end;

  if (value[0] == '\0' || value[0] == '-')
    {
      return -1;
    }

  *out = strtoull(value, &end, 0);
  return *end == '\0' ? 0 : -1;
}

/****************************************************************************
 * Name: nbootctl_reboot
 ****************************************************************************/

static int nbootctl_reboot(enum nbootctl_target_e target)
{
  unsigned int medium;
  unsigned int domain;
  unsigned int slot;
  int ret;

  ret = nbootctl_handoff(&medium, &domain, &slot);
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
                  "       nbootctl reboot console|fastboot|nuttx-a|nuttx-b\n"
                  "\n"
                  "  raw writes (SHA-256 checked at both ends)\n"
                  "       nbootctl digest FILE\n"
                  "       nbootctl verify-part FILE SHA256\n"
                  "       nbootctl write-part PARTITION FILE SHA256\n"
                  "       nbootctl write-raw LBA SECTORS FILE SHA256\n"
                  "       nbootctl write-gpt FILE SHA256\n"
                  "       nbootctl check-raw LBA SECTORS SHA256\n"
#ifdef CONFIG_FSUTILS_MKFATFS
                  "       nbootctl format PARTITION\n"
#endif
                  "\n"
                  "  PARTITION is one of uboot, trust, bootctrl, nuttx_a,\n"
                  "  nuttx_b, amp_a, amp_b, config, data.  SHA256 is the\n"
                  "  file's digest as hex; the write is refused unless it\n"
                  "  matches, and the medium is read back afterwards.  A\n"
                  "  raw write does not update bootctrl: use stage for a\n"
                  "  slot that is meant to boot.\n");
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  unsigned int medium;
  unsigned int running_domain;
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
      ret = nbootctl_handoff(&medium, &running_domain, &running_slot);
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
      ret = nbootctl_handoff(&medium, &running_domain, &running_slot);
      if (ret != 0)
        {
          return 1;
        }

      /* The running slot only protects its own domain: under AMP neither
       * NuttX slot runs, and staging "nuttx" replaces the inactive one.
       */

      ret = nbootctl_bootctrl_stage(
          medium, argv[2],
          nbootctl_running_slot(running_domain, running_slot,
                                strcmp(argv[2], "amp") == 0
                                    ? NBOOTCTL_DOMAIN_AMP
                                    : NBOOTCTL_DOMAIN_NUTTX),
          argv[3]);
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

      ret = nbootctl_handoff(&medium, &running_domain, &running_slot);
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
      ret = nbootctl_handoff(&medium, &running_domain, &running_slot);
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

  /* Raw writes.  The digest is mandatory in every form: there is
   * deliberately no way to ask for a write without one.  The medium is the
   * one this boot came from, as for the slot commands.
   */

  if (argc == 3 && strcmp(argv[1], "digest") == 0)
    {
      return nbootctl_part_digest(argv[2]) < 0 ? 1 : 0;
    }

  if (argc == 4 && strcmp(argv[1], "verify-part") == 0)
    {
      ret = nbootctl_part_verify(argv[2], argv[3]);
      if (ret < 0 && ret != -EKEYREJECTED)
        {
          fprintf(stderr, "nbootctl: verify-part failed: %d\n", ret);
        }

      return ret < 0 ? 1 : 0;
    }

  if (argc == 5 && strcmp(argv[1], "write-part") == 0)
    {
      ret = nbootctl_handoff(&medium, &running_domain, &running_slot);
      if (ret != 0)
        {
          return 1;
        }

      ret = nbootctl_part_write(medium, argv[2], argv[3], argv[4]);
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: write-part %s failed: %d\n", argv[2],
                  ret);
          return 1;
        }

      return 0;
    }

  if ((argc == 6 && strcmp(argv[1], "write-raw") == 0) ||
      (argc == 5 && strcmp(argv[1], "check-raw") == 0))
    {
      bool writing = argc == 6;
      uint64_t lba;
      uint64_t sectors;

      if (nbootctl_parse_u64(argv[2], &lba) != 0 ||
          nbootctl_parse_u64(argv[3], &sectors) != 0)
        {
          nbootctl_usage();
          return 1;
        }

      ret = nbootctl_handoff(&medium, &running_domain, &running_slot);
      if (ret != 0)
        {
          return 1;
        }

      ret = writing ? nbootctl_part_write_raw(medium, lba, sectors, argv[4],
                                              argv[5])
                    : nbootctl_part_check_raw(medium, lba, sectors, argv[4]);
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: %s %s failed: %d\n", argv[1], argv[2],
                  ret);
          return 1;
        }

      return 0;
    }

  if (argc == 4 && strcmp(argv[1], "write-gpt") == 0)
    {
      ret = nbootctl_handoff(&medium, &running_domain, &running_slot);
      if (ret != 0)
        {
          return 1;
        }

      ret = nbootctl_part_write_gpt(medium, argv[2], argv[3]);
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: write-gpt failed: %d\n", ret);
          return 1;
        }

      return 0;
    }

#ifdef CONFIG_FSUTILS_MKFATFS
  if (argc == 3 && strcmp(argv[1], "format") == 0)
    {
      ret = nbootctl_handoff(&medium, &running_domain, &running_slot);
      if (ret != 0)
        {
          return 1;
        }

      return nbootctl_format_partition(medium, argv[2]) < 0 ? 1 : 0;
    }
#endif

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
