/****************************************************************************
 * arch/arm64/include/rk3576/rk3576_shmem_layout.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __ARCH_ARM64_INCLUDE_RK3576_RK3576_SHMEM_LAYOUT_H
#define __ARCH_ARM64_INCLUDE_RK3576_RK3576_SHMEM_LAYOUT_H

/****************************************************************************
 * The shared-memory arena layout, common to both AMP domains.
 *
 * The address and size are a contract between this firmware and the Linux
 * side's reserved-memory node.  Neither side may change them alone: a size
 * mismatch would shift every slot and corrupt data silently, which is why the
 * arena header carries the size and each side checks it rather than assuming.
 *
 * The region is mapped non-cacheable by both sides, so neither has to
 * maintain cache state for the other.  That removes an entire class of
 * "occasionally one window is stale" faults that are otherwise very hard to
 * attribute.
 *
 * Placement is a fixed partition rather than a dynamic allocator.  The two
 * domains share no lock hardware, so a dynamic free list would need either
 * lock-free compare-and-swap or single-sided ownership; both are more
 * machinery than the known, bounded workload justifies.  A fixed table also
 * means a consumer that never releases its grant can only stall its own
 * service, not starve the others.
 *
 ****************************************************************************/

#include <stdint.h>

#define NYAMP_SHMEM_BASE 0x47c00000U
#define NYAMP_SHMEM_SIZE (4U * 1024U * 1024U)

/* Arena header.  Written once by the compute domain when it claims the region,
 * and read by the control domain to learn the peer's geometry.  magic is
 * checked before anything else so a foreign or uninitialized region is
 * refused rather than written over.
 */

#define NYAMP_ARENA_MAGIC   0x414d594eU /* "NYMA" in little endian */
#define NYAMP_ARENA_VERSION 1U

/* Bring-up progress marker.  The compute domain has no console -- the board's
 * only UART belongs to the control domain -- so a probe that fails partway
 * otherwise leaves no reachable trace.  It stamps how far it got here and the
 * control domain reads it back over the region both sides share.
 */

#define NYAMP_ARENA_MAGIC_OFFSET      0U
#define NYAMP_ARENA_VERSION_OFFSET    4U
#define NYAMP_ARENA_SIZE_OFFSET       8U
#define NYAMP_ARENA_GENERATION_OFFSET 12U
#define NYAMP_ARENA_TRACE_OFFSET      24U

#define NYAMP_ARENA_TRACE_MAPPED      0x4d41504dU /* "MAPM" little endian */
#define NYAMP_ARENA_TRACE_READY       0x52444159U /* "YADR" little endian */

struct nyamp_arena_s
{
  uint32_t magic;
  uint32_t version;
  uint32_t size;       /* Must equal NYAMP_SHMEM_SIZE.          */
  uint32_t generation; /* Compute-domain generation.            */
  uint32_t offset;     /* Byte offset of this header, always 0. */
  uint32_t slot_count;
  uint32_t trace; /* Bring-up progress, see the markers.   */
  uint32_t reserved[121];
};

/* Slot table.  Offsets are absolute within the arena and 64 KiB aligned so a
 * slot never shares a page with another service.
 *
 * BUF_SHARED carries the two bulk transfers that never overlap: model pulls
 * (BLOB windows, control -> compute) and synthesized speech (TTS windows,
 * compute -> control).  The compute domain lets one of them own the slot at
 * a time -- a synthesis that finds a pull in flight, or the reverse, is
 * answered BUSY -- and the lease in each grant names the owner, so reuse
 * needs no extra bookkeeping on the control side.
 */

#define NYAMP_SLOT_HEADER       0x00000000U
#define NYAMP_SLOT_HEADER_SIZE  0x00001000U /* 4 KiB  */

#define NYAMP_SLOT_SHARED       0x00001000U
#define NYAMP_SLOT_SHARED_SIZE  0x00100000U /* 1 MiB  */

#define NYAMP_SLOT_PHONEME      0x00101000U
#define NYAMP_SLOT_PHONEME_SIZE 0x00010000U /* 64 KiB */

/* Capture audio, control domain -> compute domain (KWS stream, ASR windows).
 *
 * It has a slot of its own instead of alternating over BUF_SHARED because
 * the wake word stream never stops: BUF_SHARED is also the window every
 * model pull goes through, and one synthesized sentence fills all of it, so
 * sharing would make the listener deaf for the length of every pull.  The
 * arena had 2.9 MiB unassigned, so the split costs nothing, and the control
 * domain needs no change to follow it: it writes where the grant says.
 *
 * One stream owns the slot at a time and the grant covers all of it; the
 * producer picks its windows inside the grant (one, or two used in turn).
 * 256 KiB is four one-second float32 windows.
 */

#define NYAMP_SLOT_CAPTURE      0x00120000U
#define NYAMP_SLOT_CAPTURE_SIZE 0x00040000U /* 256 KiB */

/* ASR input window: one second of 16 kHz float32 mono.  The control domain
 * fills a window, submits it and releases it before writing the next, so the
 * transfer is windowed rather than a whole utterance.  The control domain is
 * the memory-constrained side, and a window keeps its buffer small and makes
 * utterance length independent of the slot size.
 */

#define NYAMP_WINDOW_SAMPLES 16000U
#define NYAMP_WINDOW_BYTES   (NYAMP_WINDOW_SAMPLES * 4U) /* 64 KiB */

#endif /* __ARCH_ARM64_INCLUDE_RK3576_RK3576_SHMEM_LAYOUT_H */
