/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/src/kickpi_k7_appinit.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/types.h>
#include <syslog.h>
#include <nuttx/board.h>
#include "kickpi_k7.h"

#ifdef CONFIG_RK3576_SDMMC
#  include <nuttx/sdio.h>
#  include <nuttx/mmcsd.h>

/* 由芯片层 chips/rk3576/rk3576_sdmmc.c 提供 */

FAR struct sdio_dev_s *rk3576_sdmmc_initialize(int slotno);
#endif

#ifdef CONFIG_FS_TMPFS
#  include <sys/mount.h>
#endif

#if defined(CONFIG_RK3576_SDMMC) && defined(CONFIG_GPT_PARTITION)
#  include <sys/mount.h>
#  include <nuttx/fs/partition.h>

/****************************************************************************
 * Name: kickpi_k7_partition_handler
 *
 * Description:
 *   parse_block_partition 对 /dev/mmcsd0 上每个有效 GPT 分区回调本函数。
 *   GPT 布局(build_sd.sh):index0=uboot(BL33)、index1=trust、index2=rootfs。
 *   为每个分区注册块设备节点 /dev/mmcsd0p{index+1}(只建映射,不写盘,对启动件安全)。
 ****************************************************************************/

static void kickpi_k7_partition_handler(FAR struct partition_s *part,
                                        FAR void *arg)
{
  char devname[] = "/dev/mmcsd0p0";

  if (part->index < 9)
    {
      /* ASCII 数字:0x31='1'。注意不能用 (char)(1+index)(那是控制字符)。 */

      devname[sizeof(devname) - 2] = (char)(0x31 + part->index);
      register_blockpartition(devname, 0660, "/dev/mmcsd0",
                              part->firstblock, part->nblocks);
      syslog(LOG_INFO, "INFO: 分区 %s firstblock=%lu nblocks=%lu\n",
             devname, (unsigned long)part->firstblock,
             (unsigned long)part->nblocks);
    }
}

/****************************************************************************
 * Name: kickpi_k7_mount_data
 *
 * Description:
 *   解析 GPT 注册 /dev/mmcsd0pN,再把 rootfs 分区(p3)挂成 FAT 到 /data。
 *   ★ 只 mount,【绝不 mkfatfs】——启动路径做格式化,分区一旦映射错就写坏启动件
 *   导致无限重启(踩过)。首次格式化用手动 `mkfatfs /dev/mmcsd0p3` 或 PC 预格式化。
 *   任何失败只 syslog 跳过,不阻断启动。
 ****************************************************************************/

static void kickpi_k7_mount_data(void)
{
  const char *datadev = "/dev/mmcsd0p3";   /* GPT 第3分区 rootfs(sector 32768) */
  const char *datadir = "/data";
  int ret;

  ret = parse_block_partition("/dev/mmcsd0", kickpi_k7_partition_handler,
                              NULL);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: 解析 /dev/mmcsd0 GPT 分区失败: %d\n", ret);
      return;
    }

  /* 只挂载已格式化的 FAT;未格式化(mount 失败)只告警,不在此格式化 */

  ret = mount(datadev, datadir, "vfat", 0, NULL);
  if (ret >= 0)
    {
      syslog(LOG_INFO, "INFO: 已挂载 %s (vfat) -> %s\n", datadev, datadir);
    }
  else
    {
      syslog(LOG_WARNING,
             "WARNING: %s 未挂载(可能未格式化,%d)。首次用 `mkfatfs %s` 手动格式化\n",
             datadev, ret, datadev);
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
#ifdef CONFIG_RK3576_SDMMC
  /* 挂载 SD 卡槽 (SDMMC0) -> /dev/mmcsd0。失败只告警,不阻断启动。 */

  FAR struct sdio_dev_s *sdmmc = rk3576_sdmmc_initialize(0);
  if (sdmmc == NULL)
    {
      syslog(LOG_ERR, "ERROR: rk3576_sdmmc_initialize 失败\n");
    }
  else
    {
      int ret = mmcsd_slotinitialize(0, sdmmc);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize 失败: %d\n", ret);
        }
    }
#endif

#ifdef CONFIG_FS_TMPFS
  /* 挂 tmpfs 到 /tmp,供固件热更新(rb 收固件、k7flash 读) */

  if (mount(NULL, "/tmp", "tmpfs", 0, NULL) < 0)
    {
      syslog(LOG_ERR, "ERROR: mount /tmp (tmpfs) 失败\n");
    }
#endif

#if defined(CONFIG_RK3576_SDMMC) && defined(CONFIG_GPT_PARTITION)
  /* 解析 GPT 分区 + 挂 rootfs 的 FAT 到 /data(只 mount 不格式化) */

  kickpi_k7_mount_data();
#endif

  return OK;
}
