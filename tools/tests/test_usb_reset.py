#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the actual USB reset path terminates transfers before completion."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'chips/rk3576/rk3576_usb.c'
source = source_path.read_text()
start = source.index('      case DEVT_USBRST:')
end = source.index('      case DEVT_CONNECTDONE:', start)
reset = source[start:end]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define DEVT_USBRST 1
#define RK3576_NPHYEPS 8
#define DWC3_DCFG 0
#define DCFG_DEVADDR_MASK 0x7f
#define DEPCMD_ENDTRANSFER 8
#define DEPCMD_HIPRI_FORCERM 0x800
#define DEPCMD_CMDIOC 0x100
#define DEPCMD_PARAM_SHIFT 16
#define EP0_IDLE 0
#define ESHUTDOWN 108
#define uinfo(...) ((void)0)
struct usbdev_ep_s { int index; };
struct rk3576_ep_s {
  struct usbdev_ep_s ep;
  bool stalled, busy;
  int rscidx, reqq;
};
struct rk3576_usb_s {
  struct rk3576_ep_s eps[8];
  int devaddr, ep0state, usbdev;
  void *driver;
};
static struct rk3576_usb_s device;
static bool hardware[8];
static unsigned ended, completed, disconnects;
static uint32_t rk3576_usb_getreg(int reg) { (void)reg; return 42; }
static void rk3576_usb_putreg(uint32_t v, int r) { (void)v; (void)r; }
static int rk3576_usb_epcmd(uint8_t ep, uint32_t cmd,
                          int a, int b, int c, void *d) {
  (void)a; (void)b; (void)c; (void)d;
  assert((cmd & 0xff) == DEPCMD_ENDTRANSFER);
  assert(hardware[ep]); hardware[ep] = false; ended++;
  return 0;
}
static void *sq_peek(int *q) { return *q ? q : NULL; }
static void rk3576_usb_reqcomplete(struct rk3576_ep_s *ep, int status) {
  assert(status == -ESHUTDOWN);
  assert(!hardware[ep->ep.index]); ep->reqq--; completed++;
}
static int rk3576_ep_disable(struct usbdev_ep_s *ep) {
  struct rk3576_ep_s *p = &device.eps[ep->index];
  if (p->busy) {
    rk3576_usb_epcmd(ep->index, DEPCMD_ENDTRANSFER, 0, 0, 0, NULL);
    p->busy = false;
  }
  while (sq_peek(&p->reqq)) rk3576_usb_reqcomplete(p, -ESHUTDOWN);
  return 0;
}
static void disconnected(void *driver, int *dev) {
  (void)driver; (void)dev;
  for (int i=0; i<8; i++) assert(!hardware[i]);
  (void)rk3576_ep_disable;
  disconnects++;
}
#define CLASS_DISCONNECT disconnected
static void reset(struct rk3576_usb_s *priv) {
  switch (DEVT_USBRST) {
'''
code += reset + r'''
  }
}
int main(void) {
  device.driver = &device;
  for (int cycle=0; cycle<100; cycle++) {
    for (int i=0; i<8; i++) {
      device.eps[i].ep.index = i;
      device.eps[i].busy = true;
      device.eps[i].rscidx = 1;
      device.eps[i].reqq = i < 2 ? 0 : 4;
      hardware[i] = true;
    }
    reset(&device);
    reset(&device);
  }
  assert(ended == 800 && completed == 2400 && disconnects == 200);
  return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'reset.c').write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
                    '-Werror', str(path / 'reset.c'), '-o', str(path / 'reset')], check=True)
    subprocess.run([str(path / 'reset')], check=True)
print('USB_RESET_TERMINATION_PASS')
