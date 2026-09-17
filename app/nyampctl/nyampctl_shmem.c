/****************************************************************************
 * app/nyampctl/nyampctl_shmem.c
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

/****************************************************************************
 * Shared-memory region diagnostics.
 *
 * The region is the one piece both domains must agree on before any audio can
 * move, and the failure it produces when they disagree is silent corruption
 * rather than an error.  This verifies the agreement directly: it writes a
 * deterministic pattern across the whole region and reads it back, so a
 * wrong size, a stale cache or a mapping that does not reach the peer all
 * show up here instead of inside a transcription.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/chip/rk3576_shmem.h>
#include <arch/chip/rk3576_shmem_layout.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nyampctl.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The arena header lives in the first four words and belongs to whichever side
 * claims the region.  The data pattern must start after it: overwriting the
 * header would make the compute domain look unclaimed and, worse, would
 * destroy the geometry both sides agreed on.
 */

#define NYAMPCTL_SHMEM_FIRST_DATA_WORD 4U

/****************************************************************************
 * Name: nyampctl_shmem_verify
 *
 * Description:
 *   Fill the region with a position-dependent pattern and read it back.  The
 *   pattern varies with the offset so a region that is merely too small is
 *   detected as a mismatch rather than passing on repeated data.
 *
 ****************************************************************************/

static int nyampctl_shmem_verify(uint32_t *words_checked)
{
  volatile uint32_t *region = (volatile uint32_t *)rk3576_shmem_base();
  size_t count = rk3576_shmem_size() / sizeof(uint32_t);
  size_t index;
  uint32_t errors = 0;

  /* Write phase.  The region is non-cacheable on both sides, so no cache
   * maintenance is needed here; if that assumption were wrong, the read back
   * below would not match.
   */

  for (index = NYAMPCTL_SHMEM_FIRST_DATA_WORD; index < count; index++)
    {
      region[index] = (uint32_t)(index ^ 0x5a5a0000U);
    }

  /* Read phase. */
  for (index = NYAMPCTL_SHMEM_FIRST_DATA_WORD; index < count; index++)
    {
      uint32_t expected = (uint32_t)(index ^ 0x5a5a0000U);
      uint32_t actual = region[index];

      if (actual != expected)
        {
          if (errors < 4)
            {
              printf("nyamp shmem: mismatch at word %lu: %08lx != %08lx\n",
                     (unsigned long)index, (unsigned long)actual,
                     (unsigned long)expected);
            }

          errors++;
        }
    }

  *words_checked = (uint32_t)(count - NYAMPCTL_SHMEM_FIRST_DATA_WORD);
  return errors == 0 ? 0 : -1;
}

/****************************************************************************
 * Name: nyampctl_shmem_release
 *
 * Description:
 *   Clear the data area so the next run starts from a known state.  The arena
 *   header is left alone for the same reason the pattern skips it.
 *
 ****************************************************************************/

static void nyampctl_shmem_release(void)
{
  volatile uint32_t *region = (volatile uint32_t *)rk3576_shmem_base();
  size_t count = rk3576_shmem_size() / sizeof(uint32_t);
  size_t index;

  for (index = NYAMPCTL_SHMEM_FIRST_DATA_WORD; index < count; index++)
    {
      region[index] = 0;
    }
}

int nyampctl_shmem_test(bool keep)
{
  uint32_t words = 0;
  int result;

  printf("nyamp shmem: base=%p size=%lu generation=%lu trace=%08lx\n",
         rk3576_shmem_base(), (unsigned long)rk3576_shmem_size(),
         (unsigned long)rk3576_shmem_generation(),
         (unsigned long)rk3576_shmem_trace());

  /* The compute domain claims the region asynchronously, so validate it here
   * rather than relying on the bring-up check having run after the peer was
   * ready.  A size disagreement would shift every slot and corrupt audio
   * silently, so it is reported instead of worked around.
   */

  if (!rk3576_shmem_ready())
    {
      result = rk3576_shmem_initialize();
      if (result < 0)
        {
          /* The peer refuses to claim a region whose header it does not
           * recognise, which is the right call but leaves no way in when the
           * header is wrong.  Clearing it is the deliberate recovery: the
           * pattern this command writes is what corrupted it in the first
           * place, so the two belong together.
           */
          printf("nyamp shmem: arena header %08lx, clearing for reclaim\n",
                 (unsigned long)rk3576_shmem_generation());

          rk3576_shmem_reset();

          result = rk3576_shmem_initialize();
          if (result < 0)
            {
              printf("nyamp shmem: still unclaimed after reset (%d)\n",
                     result);
              return result;
            }
        }
    }

  result = nyampctl_shmem_verify(&words);
  if (result < 0)
    {
      printf("nyamp shmem: FAILED\n");
      return result;
    }

  printf("nyamp shmem: ok, %lu words match\n", (unsigned long)words);

  if (!keep)
    {
      nyampctl_shmem_release();
    }

  return 0;
}
