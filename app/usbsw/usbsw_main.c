/****************************************************************************
 * apps/examples/usbsw/usbsw_main.c
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
 * Soft connect/disconnect switch for the RK3576 USB OTG0 device
 * controller: toggles DCTL.RunStop, which is electrically equivalent to
 * plugging/unplugging the cable as seen by the host.  Lets the serial
 * Ymodem fallback run without touching the cable (host CDC/ADB traffic
 * otherwise floods the console).
 *
 *   usbsw off  - drop off the bus (host sees a detach)
 *   usbsw on   - reconnect (host re-enumerates)
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define USBSW_DCTL    0x2300c704ul /* OTG0 base + DWC3 DCTL */
#define USBSW_RUNSTOP (1ul << 31)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  volatile uint32_t *dctl = (volatile uint32_t *)USBSW_DCTL;
  uint32_t val = *dctl;

  if (argc == 2 && strcmp(argv[1], "off") == 0)
    {
      *dctl = val & ~USBSW_RUNSTOP;
      printf("usbsw: bus disconnected (DCTL %08lx -> %08lx)\n",
             (unsigned long)val, (unsigned long)*dctl);
    }
  else if (argc == 2 && strcmp(argv[1], "on") == 0)
    {
      *dctl = val | USBSW_RUNSTOP;
      printf("usbsw: bus connected (DCTL %08lx -> %08lx)\n",
             (unsigned long)val, (unsigned long)*dctl);
    }
  else
    {
      printf("usage: usbsw on|off\n"
             "  off: soft-detach from the USB host (for serial Ymodem)\n"
             "  on:  reconnect, host re-enumerates\n"
             "state: DCTL=%08lx (%s)\n",
             (unsigned long)val,
             (val & USBSW_RUNSTOP) ? "connected" : "disconnected");
      return 1;
    }

  return 0;
}
