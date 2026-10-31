#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the actual DWC3 resource and endpoint configuration functions."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'chips/rk3576/rk3576_usb.c'
source = source_path.read_text(encoding='utf-8')

def function(name):
    start = source.index('static int ' + name + '(')
    while source.find(';', start) < source.find('\n{', start):
        start = source.index('static int ' + name + '(', start + 1)
    end = source.index('\n}\n', start) + 3
    return source[start:end]

has_init = 'static int rk3576_usb_epresources(void)\n{' in source
header = ROOT / 'chips/rk3576/hardware/rk3576_usb.h'
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#define OK 0
#define RK3576_NPHYEPS 8
struct rk3576_ep_s { uint8_t phyep; };
static unsigned resources[RK3576_NPHYEPS], calls, configurations;
static int fail_endpoint = -1;
static bool fail_start;
static int rk3576_usb_epcmd(uint8_t ep, uint32_t cmd, uint32_t p0,
                            uint32_t p1, uint32_t p2, uint32_t *rsc)
{
  (void)p1; (void)p2; (void)rsc;
  assert(ep < RK3576_NPHYEPS);
  calls++;
  if (cmd == DEPCMD_DEPSTARTCFG) {
    assert(ep == 0 && p0 == 0);
    memset(resources, 0, sizeof(resources));
    return fail_start ? -ETIMEDOUT : 0;
  }
  if (cmd == DEPCMD_SETTRANSFRESOURCE) {
    assert(p0 == 1);
    if (ep == fail_endpoint || resources[ep]) return -EIO;
    resources[ep]++;
  } else {
    assert(cmd == DEPCMD_SETEPCONFIG);
    assert(resources[ep] == 1);
    configurations++;
  }
  return 0;
}
'''
initialize = r'''
  fail_start = true;
  assert(rk3576_usb_epresources() == -ETIMEDOUT && calls == 1);
  fail_start = false;
  fail_endpoint = 3;
  assert(rk3576_usb_epresources() == -EIO);
  assert(resources[2] == 1 && resources[3] == 0 && resources[4] == 0);
  fail_endpoint = -1;
  calls = 0;
  assert(rk3576_usb_epresources() == 0 && calls == 9);
'''
main = r'''
  for (unsigned cycle = 0; cycle < 1000; cycle++) {
    for (unsigned ep = 0; ep < RK3576_NPHYEPS; ep++) {
      struct rk3576_ep_s endpoint = {(uint8_t)ep};
      assert(rk3576_usb_epconfig(&endpoint, ep < 2 ? 0 : 2,
                               ep < 2 ? 64 : 512, ep < 2) == 0);
      assert(resources[ep] == 1);
    }
  }
  assert(configurations == 8000);
  return 0;
}
'''
code = '#include "' + str(header).replace('\\', '/') + '"\n' + fixture
if has_init:
    code += function('rk3576_usb_epresources')
code += function('rk3576_usb_epconfig') + '\nint main(void)\n{\n'
code += initialize if has_init else '  for (unsigned i=0; i<8; i++) resources[i]=1;\n'
code += main
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'nuttx').mkdir()
    (path / 'nuttx/config.h').write_text('')
    test = path / 'resources.c'
    binary = path / 'resources'
    test.write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
                    '-Werror', '-I' + str(path), str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('USB_PERSISTENT_RESOURCES_PASS')
