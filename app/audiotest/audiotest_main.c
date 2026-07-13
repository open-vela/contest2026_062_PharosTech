/****************************************************************************
 * apps/examples/audiotest/audiotest_main.c
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
 * On-demand ES8388 / SAI1 audio bring-up trigger.  Runs the on-board audio
 * initialisation (see kickpi_k7_audio.c) from the NSH prompt so a stall in
 * SAI/codec clock bring-up only blocks this command, never the boot path.
 * After it reports success, /dev/audio/pcm0 is ready for nxplayer.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>

/* Board bring-up entry (boards/rk3576/kickpi-k7/src/kickpi_k7_audio.c). */

int kickpi_k7_audio_initialize(void);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  int ret;

  printf("audiotest: bringing up ES8388 on SAI1 / I2C3 ...\n");
  ret = kickpi_k7_audio_initialize();
  printf("audiotest: kickpi_k7_audio_initialize returned %d\n", ret);
  return ret < 0 ? 1 : 0;
}
