// SPDX-License-Identifier: GPL-2.0
/*
 * nyamp_shmem.c - Nyabula AMP shared-memory region (Linux compute domain)
 *
 * The control domain and the compute domain exchange audio through a reserved
 * region instead of through RPMsg messages.  A second of 16 kHz float32 PCM is
 * 64 KiB and the TTS vocoder emits 1 MiB in one call, so carrying those as
 * RPMsg chunks would take hundreds of round trips; the messages carry only a
 * descriptor and the bytes live here.
 *
 * This driver does one thing: it maps the reserved region into userspace and
 * reports its geometry.  Placement, ownership and lifetime are the daemon's
 * policy, kept out of the kernel so it can be tested without a board.
 *
 * The region is already reserved by the device tree with no-map, which keeps
 * it out of the linear mapping and out of the page allocator.  no-map does not
 * prevent ioremap, and the vendor rpmsg driver maps its vrings the same way.
 * The mapping is non-cacheable, matching the control domain's mapping of the
 * same pages: one physical page shared under two cacheabilities is undefined
 * behaviour, and it would show up as an occasional stale sample rather than a
 * clean failure.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "nyamp_shmem_uapi.h"

#define NYAMP_SHMEM_DRIVER_NAME "nyamp-shmem"

/*
 * The arena header is validated before the region is handed to userspace.  A
 * mismatch means the two domains disagree about the region's geometry, which
 * would shift every slot; reporting it is the only safe response.
 */

struct nyamp_shmem
{
  struct device *dev;
  void __iomem *base;
  phys_addr_t phys;
  resource_size_t size;
  u32 generation;
  struct miscdevice misc;
  struct mutex lock;
};

static struct nyamp_shmem *nyamp_shmem;

static int nyamp_shmem_open(struct inode *inode, struct file *file)
{
  struct nyamp_shmem *shmem =
      container_of(file->private_data, struct nyamp_shmem, misc);

  file->private_data = shmem;
  return 0;
}

static int nyamp_shmem_mmap(struct file *file, struct vm_area_struct *vma)
{
  struct nyamp_shmem *shmem = file->private_data;
  unsigned long length = vma->vm_end - vma->vm_start;
  unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

  if (length == 0 || length > shmem->size || offset > shmem->size ||
      length > shmem->size - offset)
    {
      dev_err(shmem->dev, "mmap out of range: offset %#lx length %#lx\n",
              offset, length);
      return -EINVAL;
    }

  /* vm_flags is not directly writable on this kernel; the helper keeps the
   * locking contract for VMA flag updates.
   */
  vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

  return remap_pfn_range(vma, vma->vm_start,
                         (shmem->phys + offset) >> PAGE_SHIFT, length,
                         vma->vm_page_prot);
}

static long nyamp_shmem_ioctl(struct file *file, unsigned int command,
                              unsigned long argument)
{
  struct nyamp_shmem *shmem = file->private_data;

  switch (command)
    {
      case NYAMP_SHMEM_IOC_INFO:
        {
          struct nyamp_shmem_info info = {
            .magic = NYAMP_SHMEM_MAGIC,
            .version = NYAMP_SHMEM_VERSION,
            .size = shmem->size,
            .base_phys = shmem->phys,
          };

          if (copy_to_user((void __user *)argument, &info, sizeof(info)))
            {
              return -EFAULT;
            }

          return 0;
        }

      default:
        return -ENOTTY;
    }
}

static const struct file_operations nyamp_shmem_fops = {
  .owner = THIS_MODULE,
  .open = nyamp_shmem_open,
  .mmap = nyamp_shmem_mmap,
  .unlocked_ioctl = nyamp_shmem_ioctl,
  .llseek = no_llseek,
};

static int nyamp_shmem_probe(struct platform_device *pdev)
{
  struct nyamp_shmem *shmem;
  struct device_node *region;
  struct resource resource;
  u32 magic;
  int ret;

  shmem = devm_kzalloc(&pdev->dev, sizeof(*shmem), GFP_KERNEL);
  if (!shmem)
    {
      return -ENOMEM;
    }

  shmem->dev = &pdev->dev;
  mutex_init(&shmem->lock);

  /* Identifies this claim of the region.  A restarted daemon gets a new one,
   * which is how every grant issued before the restart is recognised as stale
   * without either side keeping a shared record.  Zero is reserved for "no
   * generation", so a failure to obtain randomness still yields a valid value.
   */
  shmem->generation = (u32)ktime_get_ns();
  if (shmem->generation == 0)
    {
      shmem->generation = 1;
    }

  /* Record how far probe gets in a place this side can actually report.
   *
   * The compute domain has no console: the board's only UART belongs to the
   * control domain, so dev_err output here goes nowhere reachable.  The
   * region itself is the one channel both sides share, so each step is
   * stamped into it and the control domain can read the progress back.
   */

  /* The region is referenced rather than being the device node itself: the
   * kernel only creates platform devices for reserved-memory children whose
   * compatible is on its own allow-list, and that list is not ours to extend.
   */

  region = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
  if (!region)
    {
      dev_err(&pdev->dev, "no memory-region reference\n");
      return -ENODEV;
    }

  ret = of_address_to_resource(region, 0, &resource);
  of_node_put(region);
  if (ret)
    {
      dev_err(&pdev->dev, "memory-region has no address: %d\n", ret);
      return ret;
    }

  shmem->phys = resource.start;
  shmem->size = resource_size(&resource);

  /* devm_ioremap rather than devm_ioremap_resource: the latter refuses no-map
   * memory, which is exactly what the reserved region is.
   */
  shmem->base = devm_ioremap(&pdev->dev, shmem->phys, shmem->size);
  if (!shmem->base)
    {
      dev_err(&pdev->dev, "ioremap %pa failed\n", &shmem->phys);
      return -ENOMEM;
    }

  /* Stamp the reaching of this point into the region itself.  The compute
   * domain has no console -- the board's only UART belongs to the control
   * domain -- so dev_err output goes nowhere reachable, and the region is the
   * one channel both sides share.  A probe that stops below is then still
   * distinguishable from one that never ran.
   */
  writel(NYAMP_ARENA_TRACE_MAPPED, shmem->base + NYAMP_ARENA_TRACE_OFFSET);

  /* Claim unconditionally.  The device tree hands this region to this driver
   * alone, so whatever the header holds is either DRAM left over from power
   * on -- seen on the board as 0xffffffff, which an earlier "refuse a foreign
   * magic" check took for another owner and so never claimed the region -- or
   * a header from before a warm reset, whose generation no longer matches
   * this boot.  Neither is worth preserving, and probe runs once per boot, so
   * there is no live claim to trample.
   *
   * The magic goes in last: the control domain polls it, and must not see a
   * valid magic in front of a size or generation that is still stale.
   */
  if (magic != 0 && magic != NYAMP_SHMEM_MAGIC)
    {
      dev_info(&pdev->dev, "replacing stale arena magic %#x\n", magic);
    }

  if (magic == 0)
    {
      /* First claim: publish the arena header so the control domain can
       * validate its own view of the region against ours.  The generation
       * identifies this claim; a later one means every previous grant is
       * void, which is how a restarted daemon invalidates stale references.
       */
      writel(NYAMP_SHMEM_MAGIC, shmem->base + NYAMP_ARENA_MAGIC_OFFSET);
      writel(NYAMP_SHMEM_VERSION, shmem->base + NYAMP_ARENA_VERSION_OFFSET);
      writel((u32)shmem->size, shmem->base + NYAMP_ARENA_SIZE_OFFSET);
      writel(shmem->generation, shmem->base + NYAMP_ARENA_GENERATION_OFFSET);
    }
  else if (readl(shmem->base + NYAMP_ARENA_SIZE_OFFSET) != (u32)shmem->size)
    {
      dev_err(&pdev->dev, "arena size %u, driver expects %u\n",
              readl(shmem->base + NYAMP_ARENA_SIZE_OFFSET), (u32)shmem->size);
      return -EINVAL;
    }

  shmem->misc.name = NYAMP_SHMEM_DEVICE_NAME;
  shmem->misc.minor = MISC_DYNAMIC_MINOR;
  shmem->misc.fops = &nyamp_shmem_fops;
  shmem->misc.mode = 0600;

  ret = misc_register(&shmem->misc);
  if (ret)
    {
      dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
      return ret;
    }

  platform_set_drvdata(pdev, shmem);
  nyamp_shmem = shmem;

  /* Probe reached the end: the control domain reads this back to tell a
   * complete bring-up from one that stopped partway.
   */
  writel(NYAMP_ARENA_TRACE_READY, shmem->base + NYAMP_ARENA_TRACE_OFFSET);

  dev_info(&pdev->dev, "%s ready: %pa %pa\n", NYAMP_SHMEM_DEVICE_NAME,
           &shmem->phys, &shmem->size);
  return 0;
}

static int nyamp_shmem_remove(struct platform_device *pdev)
{
  struct nyamp_shmem *shmem = platform_get_drvdata(pdev);

  misc_deregister(&shmem->misc);
  nyamp_shmem = NULL;
  return 0;
}

static const struct of_device_id nyamp_shmem_of_match[] = {
  { .compatible = "nyabula,amp-shmem" },
  {},
};
MODULE_DEVICE_TABLE(of, nyamp_shmem_of_match);

static struct platform_driver nyamp_shmem_driver = {
  .probe = nyamp_shmem_probe,
  .remove = nyamp_shmem_remove,
  .driver = {
    .name = NYAMP_SHMEM_DRIVER_NAME,
    .of_match_table = nyamp_shmem_of_match,
  },
};

static int __init nyamp_shmem_init(void)
{
  return platform_driver_register(&nyamp_shmem_driver);
}

static void __exit nyamp_shmem_exit(void)
{
  platform_driver_unregister(&nyamp_shmem_driver);
}

module_init(nyamp_shmem_init);
module_exit(nyamp_shmem_exit);

MODULE_AUTHOR("Pharos Tech");
MODULE_DESCRIPTION("Nyabula AMP shared-memory region");
MODULE_LICENSE("GPL v2");
