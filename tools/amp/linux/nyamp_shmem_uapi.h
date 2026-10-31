/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * nyamp_shmem_uapi.h - interface between the shared-memory driver and the
 * Nyabula compute-domain daemon.
 *
 * The arena layout itself lives on the firmware side (chips/rk3576/
 * rk3576_shmem_layout.h) because that is what actually places the slots.  Only
 * the values both sides must agree on are repeated here.
 */

#ifndef __NYAMP_SHMEM_UAPI_H
#define __NYAMP_SHMEM_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define NYAMP_SHMEM_DEVICE_NAME "nyamp-shmem"

/* Arena header, written once by this driver when it claims the region.  The
 * control domain reads it to check that both sides agree on the size; a
 * disagreement would shift every slot.
 */

/* The arena header layout is a contract with the control domain, whose copy of
 * it lives in chips/rk3576/include/rk3576_shmem_layout.h.  The two trees build
 * separately, so the values are repeated rather than shared; a disagreement
 * would shift every slot and corrupt audio silently, which is why
 * tools/amp/test_shmem_layout.py compares the two and fails on drift.
 */

#define NYAMP_SHMEM_MAGIC             0x414d594eU /* "NYMA" in little endian */
#define NYAMP_SHMEM_VERSION           1U

#define NYAMP_ARENA_MAGIC_OFFSET      0U
#define NYAMP_ARENA_VERSION_OFFSET    4U
#define NYAMP_ARENA_SIZE_OFFSET       8U
#define NYAMP_ARENA_GENERATION_OFFSET 12U
#define NYAMP_ARENA_TRACE_OFFSET      24U

#define NYAMP_ARENA_TRACE_MAPPED      0x4d41504dU /* "MAPM" little endian */
#define NYAMP_ARENA_TRACE_READY       0x52444159U /* "YADR" little endian */

struct nyamp_shmem_info
{
  uint32_t magic;
  uint32_t version;
  uint32_t size;
  uint32_t reserved;
  uint64_t base_phys;
};

#define NYAMP_SHMEM_IOC_INFO _IOR('N', 0x01, struct nyamp_shmem_info)

#endif /* __NYAMP_SHMEM_UAPI_H */
