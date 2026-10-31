#!/usr/bin/env python3
# tools/tests/test_usb_irq_serialization.py
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to you under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Compile the real IRQ entry with an instrumented critical-section API."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'chips/rk3576/rk3576_usb.c').read_text()
start = source.index('static int rk3576_usb_interrupt(int irq, void *context, '
                     'void *arg)\n{')
end = source.index('\n}\n', start) + 3
entry = source[start:end]

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned irqstate_t;
#define OK 0
#define RK3576_EVTBUF_SIZE 4096
#define DWC3_GEVNTCOUNT 0
#define EVT_IS_DEVT(evt) ((evt) & 1)
struct rk3576_usb_s { uint32_t *evtbuf; uint32_t evtoff; };
static uint32_t events[RK3576_EVTBUF_SIZE / 4] = {1, 2, 2};
static struct rk3576_usb_s g_usbdev = {events, 0};
static unsigned depth, pending, acks, calls;
static irqstate_t enter_critical_section(void) { return depth++; }
static void leave_critical_section(irqstate_t saved)
{
  assert(depth == saved + 1);
  depth = saved;
}
static uint32_t rk3576_usb_getreg(uint32_t reg)
{
  (void)reg;
  assert(depth == 1);
  return pending;
}
static void rk3576_usb_putreg(uint32_t value, uint32_t reg)
{
  (void)reg;
  assert(depth == 1 && value == 4 && pending >= 4);
  pending -= value;
  if (++acks == 2) pending += 4; /* DMA posts an event during draining. */
}
static void up_invalidate_dcache(uintptr_t start, uintptr_t end)
{
  assert(depth == 1 && end - start == RK3576_EVTBUF_SIZE);
}
static void rk3576_usb_epevent(struct rk3576_usb_s *priv, uint32_t event)
{
  irqstate_t saved;
  (void)priv;
  (void)event;
  assert(depth == 1); /* busy/TRB/queue state must be serialized together. */
  saved = enter_critical_section(); /* Completion callback resubmission. */
  assert(depth == 2);
  leave_critical_section(saved);
  calls++;
}
#define rk3576_usb_devevent rk3576_usb_epevent
'''
MAIN = r'''
int main(void)
{
  assert(rk3576_usb_interrupt(0, NULL, NULL) == OK);
  assert(depth == 0 && acks == 0 && calls == 0);
  pending = 8;
  assert(rk3576_usb_interrupt(0, NULL, NULL) == OK);
  assert(depth == 0 && pending == 0 && acks == 3 && calls == 3);
  assert(g_usbdev.evtoff == 12);
  return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    test = path / 'usb_irq.c'
    binary = path / 'usb_irq'
    test.write_text(HARNESS + entry + MAIN)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', str(test), '-o',
                    str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('USB_IRQ_SERIALIZATION_PASS')
