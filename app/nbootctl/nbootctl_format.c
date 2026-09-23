/****************************************************************************
 * apps/system/nbootctl/nbootctl_format.c
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

/* Filesystem creation for the store partitions.
 *
 * Two callers, one implementation.  The board's storage driver reaches
 * here through kickpi_k7_storage_format_hook() when it finds a partition
 * that has never been formatted, and an operator can ask directly with
 * `nbootctl format <partition>`.  Both want the same thing: a fresh FAT
 * filesystem on a named partition.
 *
 * The formatter itself is the mkfatfs utility's, whose header is public in
 * the application tree.  Calling it from here rather than from the driver
 * keeps the dependency pointing the way it should -- an application may
 * use a board service, not the other way round.
 *
 * This file is only built with CONFIG_FSUTILS_MKFATFS.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <fsutils/mkfatfs.h>

#include "nbootctl_part.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The board declares this one weak and owns no header an application could
 * include, so the prototype the strong definition is checked against lives
 * here.
 */

int kickpi_k7_storage_format_hook(FAR const char *path);

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int nbootctl_format_device(FAR const char *path);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nbootctl_format_device
 *
 * Description:
 *   Put a FAT filesystem on a block device node.
 *
 *   Nothing here looks at mount points: mkfatfs writes over the structures
 *   a mounted filesystem is using, and the result is not merely wrong but
 *   undefined.  The caller is expected to know whether the partition is in
 *   use.
 *
 ****************************************************************************/

static int nbootctl_format_device(FAR const char *path)
{
  struct fat_format_s fmt = FAT_FORMAT_INITIALIZER;
  int ret;

  if (path == NULL)
    {
      return -EINVAL;
    }

  /* The initializer leaves FAT width and cluster size on autoselect, so
   * mkfatfs sizes both from the device, and the FAT count at two: an
   * all-zero structure is rejected as invalid.  Only the label is set, so
   * the store partitions are recognisable from a host.  ff_volumelabel is
   * a fixed 11-byte field, not a pointer.
   */

  memcpy(fmt.ff_volumelabel, "NYABULA    ", sizeof(fmt.ff_volumelabel));

  ret = mkfatfs(path, &fmt);
  if (ret < 0)
    {
      /* The autoselect search gives up on a volume of tens of gigabytes
       * (seen on the 28 GiB data partition), where the only width that fits
       * is FAT32.  Ask for it outright; a volume too small for FAT32 fails
       * this second attempt as well and the first error is the one to
       * report.
       */

      int first = errno;
      struct fat_format_s wide = FAT_FORMAT_INITIALIZER;

      wide.ff_fattype = 32;
      memcpy(wide.ff_volumelabel, fmt.ff_volumelabel,
             sizeof(wide.ff_volumelabel));
      if (mkfatfs(path, &wide) < 0)
        {
          return -first;
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nbootctl_format_partition
 ****************************************************************************/

int nbootctl_format_partition(unsigned int medium, FAR const char *partition)
{
  char path[NBOOTCTL_FORMAT_PATH_MAX];
  int ret;

  ret = nbootctl_part_device_path(medium, partition, path, sizeof(path));
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: cannot format %s: %d\n",
              partition != NULL ? partition : "(null)", ret);
      return ret;
    }

  printf("nbootctl: formatting %s\n", path);
  ret = nbootctl_format_device(path);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: format %s failed: %d\n", path, ret);
      return ret;
    }

  printf("nbootctl: %s formatted\n", path);
  return 0;
}

/****************************************************************************
 * Name: kickpi_k7_storage_format_hook
 *
 * Description:
 *   Strong definition of the weak hook the board declares.
 *
 *   The board source is compiled into every configuration of this board,
 *   including ones that do not include nbootctl, so it can only carry a
 *   weak reference.  When this application is present it takes over and a
 *   partition found blank at boot is prepared automatically.
 *
 ****************************************************************************/

int kickpi_k7_storage_format_hook(FAR const char *path)
{
  if (path == NULL)
    {
      return -EINVAL;
    }

  return nbootctl_format_device(path);
}
