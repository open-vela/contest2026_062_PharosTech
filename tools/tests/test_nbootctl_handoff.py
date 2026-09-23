# SPDX-License-Identifier: Apache-2.0
"""Decode N-Boot handoff headers with the actual reader, register by register."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source_path = (Path(sys.argv[1]) if len(sys.argv) > 1 else
               root / 'app/nbootctl/nbootctl_bootctrl.c')
source = source_path.read_text().replace('\r\n', '\n')
header = (source_path.parent / 'nbootctl_bootctrl.h').read_text()
header = header.replace('\r\n', '\n')


def function(pattern):
    match = re.search(pattern, source)
    assert match, pattern
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


defines = '\n'.join(re.findall(r'^#define NBOOTCTL_(?:HANDOFF|GENERATION)\w+ .*$',
                               source, re.M))
defines += '\n' + '\n'.join(re.findall(
    r'^#define NBOOTCTL_(?:DOMAIN|SLOT|REASON)\w+ .*$', header, re.M))
reader = function(r'int nbootctl_handoff_read\([^;]*?\)\n\{')
running = function(r'unsigned int nbootctl_running_slot\([^;]*?\)\n\{')
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
''' + defines + r'''
static uint32_t g_nbootctl_handoff_header;
static uint64_t g_nbootctl_handoff_generation;
static volatile bool g_nbootctl_handoff_latched;
static uint32_t regs[3];
static int reads, tear;
static uint32_t nbootctl_reg_read(uintptr_t address)
{
  uint32_t value = regs[(address - NBOOTCTL_HANDOFF_REG) / 4];

  /* A writer in the middle of an update: the second look differs. */

  if (address == NBOOTCTL_HANDOFF_REG && tear && reads++ > 0)
    {
      value ^= 1;
    }

  return value;
}
''' + reader + running + r'''
static int decode(uint32_t header, unsigned int *medium, unsigned int *domain,
                  unsigned int *slot, unsigned int *reason)
{
  uint64_t generation;
  int ret;

  g_nbootctl_handoff_latched = false;
  regs[0] = header; regs[1] = 0x89abcdefu; regs[2] = 0x01234567u;
  reads = 0;
  ret = nbootctl_handoff_read(medium, domain, slot, reason, &generation);
  if (ret == 0)
    {
      assert(generation == 0x0123456789abcdefull);
    }

  return ret;
}

int main(void)
{
  unsigned int medium, domain, slot, reason;

  /* What N-Boot has always written for a NuttX slot still decodes as one:
   * eMMC slot b, normal; SD slot a, fallback.
   */

  assert(decode(0x4e482021u, &medium, &domain, &slot, &reason) == 0);
  assert(medium == 2 && domain == NBOOTCTL_DOMAIN_NUTTX && slot == 1 &&
         reason == 0);
  assert(decode(0x4e482210u, &medium, &domain, &slot, &reason) == 0);
  assert(medium == 1 && domain == NBOOTCTL_DOMAIN_NUTTX && slot == 0 &&
         reason == 2);

  /* The AMP control domain, from eMMC slot b. */

  assert(decode(0x4e482025u, &medium, &domain, &slot, &reason) == 0);
  assert(medium == 2 && domain == NBOOTCTL_DOMAIN_AMP && slot == 1 &&
         reason == 0);
  assert(nbootctl_running_slot(domain, slot, NBOOTCTL_DOMAIN_AMP) == 1);
  assert(nbootctl_running_slot(domain, slot, NBOOTCTL_DOMAIN_NUTTX) ==
         NBOOTCTL_SLOT_NONE);

  /* An AMP image from RAM: a medium, and no slot in either domain. */

  assert(decode(0x4e482314u, &medium, &domain, &slot, &reason) == 0);
  assert(medium == 1 && domain == NBOOTCTL_DOMAIN_AMP &&
         slot == NBOOTCTL_SLOT_NONE && reason == NBOOTCTL_REASON_RAM);
  assert(nbootctl_running_slot(domain, slot, NBOOTCTL_DOMAIN_AMP) ==
         NBOOTCTL_SLOT_NONE);
  assert(nbootctl_running_slot(domain, slot, NBOOTCTL_DOMAIN_NUTTX) ==
         NBOOTCTL_SLOT_NONE);

  /* Refused: nothing there, another version, no medium, the reserved bit,
   * an unknown domain or reason, and a RAM record that claims a slot or a
   * NuttX domain.
   */

  assert(decode(0, NULL, NULL, NULL, NULL) == -ENODEV);
  assert(decode(0x4e483021u, NULL, NULL, NULL, NULL) == -ENODEV);
  assert(decode(0x4e482001u, NULL, NULL, NULL, NULL) == -EBADMSG);
  assert(decode(0x4e482023u, NULL, NULL, NULL, NULL) == -EBADMSG);
  assert(decode(0x4e482029u, NULL, NULL, NULL, NULL) == -EBADMSG);
  assert(decode(0x4e482421u, NULL, NULL, NULL, NULL) == -EBADMSG);
  assert(decode(0x4e482315u, NULL, NULL, NULL, NULL) == -EBADMSG);
  assert(decode(0x4e482310u, NULL, NULL, NULL, NULL) == -EBADMSG);

  /* A header that changes between the two looks is not a handoff. */

  tear = 1;
  assert(decode(0x4e482025u, NULL, NULL, NULL, NULL) == -ENODEV);
  tear = 0;

  /* Once read, the record no longer depends on the registers. */

  assert(decode(0x4e482025u, NULL, NULL, NULL, NULL) == 0);
  regs[0] = 0; regs[1] = 0; regs[2] = 0;
  assert(nbootctl_handoff_read(&medium, &domain, &slot, &reason, NULL) == 0);
  assert(medium == 2 && domain == NBOOTCTL_DOMAIN_AMP && slot == 1);

  /* But an invalid record is never kept. */

  assert(decode(0x4e482029u, NULL, NULL, NULL, NULL) == -EBADMSG);
  regs[0] = 0x4e482021u;
  assert(nbootctl_handoff_read(&medium, &domain, &slot, NULL, NULL) == 0);
  assert(domain == NBOOTCTL_DOMAIN_NUTTX && slot == 1);

  puts("NBOOTCTL_HANDOFF_PASS nuttx amp ram rejected torn latched");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nbootctl-handoff-') as directory:
    path = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    path.write_text(program)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
