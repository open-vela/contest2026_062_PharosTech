/****************************************************************************
 * app/k7flash/k7flash_main.c
 *
 * KICKPI-K7 板上固件热更新落盘工具。把一个新固件(Rockchip FIT,即
 * build_sd 产出的 uboot_nuttx.img)写进 SD 卡的 uboot 槽,读回校验,重启生效。
 *
 * ★ 安全护栏(硬性):
 *   1. 目标块设备写死 "/dev/mmcsd0"(SD 卡),起始扇区写死 16384(uboot 槽)。
 *      命令行只收 <file>,不接受设备/偏移参数 —— 无默认全盘、无通配。
 *   2. eMMC host 在本 BSP 从未实例化,代码物理够不到 eMMC(0x2A330000)。
 *   3. 写范围不越过 trust 槽(扇区 24576),绝不碰 idbloader(64)/GPT(0)。
 *   4. 写前校验是合法 FIT(magic 0xd00dfeed);写后逐扇区读回校验。
 *   5. 任一步失败:保持运行、告警、不重启(避免半写砖卡)。
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/boardctl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <nuttx/fs/fs.h>

/****************************************************************************
 * 护栏常量(写死,不可由命令行覆盖)
 ****************************************************************************/

#define K7_TARGET_DEV   "/dev/mmcsd0"   /* 只写 SD 卡,永不 eMMC */
#define K7_UBOOT_SECTOR 16384           /* uboot 槽起始扇区 */
#define K7_TRUST_SECTOR 24576           /* trust 槽起始(写不可越过此) */
#define K7_SECTOR_SIZE  512
#define K7_FIT_MAGIC    0xd00dfeedu      /* Rockchip/U-Boot FIT(FDT) magic */
#define K7_MAX_BYTES    ((K7_TRUST_SECTOR - K7_UBOOT_SECTOR) * K7_SECTOR_SIZE)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t k7_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/****************************************************************************
 * k7flash_main
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct inode *bnode = NULL;
  uint8_t *buf = NULL;
  uint8_t *vbuf = NULL;
  const char *path;
  struct stat st;
  size_t nbytes;
  size_t nsectors;
  ssize_t rn;
  int fd = -1;
  int ret;

  if (argc != 2)
    {
      fprintf(stderr, "用法: k7flash <fw.img>  (固件=build_sd 产出的 uboot_nuttx.img/FIT)\n");
      return 1;
    }

  path = argv[1];

  /* 1) 读固件到内存 */

  if (stat(path, &st) < 0)
    {
      fprintf(stderr, "错误: 打不开 %s: %d\n", path, errno);
      return 1;
    }

  nbytes = (size_t)st.st_size;
  if (nbytes == 0 || nbytes > K7_MAX_BYTES)
    {
      fprintf(stderr, "错误: 固件大小 %zu 非法(需 1..%d 字节,即不越过 trust 槽)\n",
              nbytes, K7_MAX_BYTES);
      return 1;
    }

  nsectors = (nbytes + K7_SECTOR_SIZE - 1) / K7_SECTOR_SIZE;

  /* buffer 按 cache line(64B)对齐:满足 SDMMC IDMAC 的 dmapreflight,走多块 DMA;
   * 否则驱动会自动回退单块 PIO(仍能工作,只是慢)。
   */

  buf  = memalign(64, nsectors * K7_SECTOR_SIZE);
  vbuf = memalign(64, nsectors * K7_SECTOR_SIZE);
  if (buf == NULL || vbuf == NULL)
    {
      fprintf(stderr, "错误: 内存不足\n");
      goto errout;
    }

  memset(buf, 0, nsectors * K7_SECTOR_SIZE);   /* 末扇区补零 */
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "错误: open %s: %d\n", path, errno);
      goto errout;
    }

  if (read(fd, buf, nbytes) != (ssize_t)nbytes)
    {
      fprintf(stderr, "错误: 读固件不完整\n");
      goto errout;
    }

  close(fd);
  fd = -1;

  /* 2) 校验是合法 FIT(magic) */

  if (k7_be32(buf) != K7_FIT_MAGIC)
    {
      fprintf(stderr, "错误: 不是合法 FIT(magic=%08x,期望 %08x),拒绝写入\n",
              k7_be32(buf), K7_FIT_MAGIC);
      goto errout;
    }

  /* 3) 取块设备(写死 SD,不接受参数) */

  ret = find_blockdriver(K7_TARGET_DEV, 0, &bnode);
  if (ret < 0 || bnode->u.i_bops->write == NULL ||
      bnode->u.i_bops->read == NULL)
    {
      fprintf(stderr, "错误: 找不到可读写块设备 %s\n", K7_TARGET_DEV);
      goto errout;
    }

  printf("k7flash: 写 %zu 字节(%zu 扇区)到 %s @sector %d ...\n",
         nbytes, nsectors, K7_TARGET_DEV, K7_UBOOT_SECTOR);

  /* 4) 多块写(buffer 已 cache-line 对齐 → SDMMC IDMAC 一次搬多扇区;
   * 不对齐时驱动自动回退单块 PIO)。
   */

  rn = bnode->u.i_bops->write(bnode, buf, K7_UBOOT_SECTOR, nsectors);
  if (rn != (ssize_t)nsectors)
    {
      fprintf(stderr, "错误: 写失败(rn=%zd,期望 %zu)\n", rn, nsectors);
      goto errout;
    }

  /* 5) 多块读回校验 */

  rn = bnode->u.i_bops->read(bnode, vbuf, K7_UBOOT_SECTOR, nsectors);
  if (rn != (ssize_t)nsectors || memcmp(buf, vbuf, nbytes) != 0)
    {
      fprintf(stderr, "错误: 校验失败(rn=%zd) —— 不重启,请重试\n", rn);
      goto errout;
    }

  free(buf);
  free(vbuf);
  buf = NULL;
  vbuf = NULL;

  /* 6) 更新成功:删除临时固件,回收 tmpfs 空间(/tmp 是 RAM) */

  if (unlink(path) == 0)
    {
      printf("k7flash: 已清理 %s,回收空间\n", path);
    }

  printf("k7flash: 写入+校验通过。3 秒后重启生效...\n");
  fflush(stdout);
  sleep(3);

  /* 6) 重启 → 新固件启动 */

#ifdef CONFIG_BOARDCTL_RESET
  boardctl(BOARDIOC_RESET, 0);
#else
  printf("k7flash: 未启用 BOARDCTL_RESET,请手动 reset 生效\n");
#endif
  return 0;

errout:
  if (fd >= 0)
    {
      close(fd);
    }

  free(buf);
  free(vbuf);
  return 1;
}
