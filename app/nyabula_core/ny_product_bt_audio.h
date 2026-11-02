/****************************************************************************
 * app/nyabula_core/ny_product_bt_audio.h
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

#ifndef __NYABULA_CORE_NY_PRODUCT_BT_AUDIO_H
#define __NYABULA_CORE_NY_PRODUCT_BT_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The audio half of the Bluetooth service, kept apart from the stack half
 * (ny_product_bt.c) on purpose: the entry points marked "stack context" are
 * called from Bluetooth host callbacks and only copy bytes and post a
 * semaphore; decoding, encoding and every device call happen on the audio
 * thread this file owns.  It includes no Bluetooth header.
 */

#ifdef CONFIG_NYABULA_CORE_BT

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Who holds the speaker. */

#define NY_BT_AUDIO_OWNER_NONE  0
#define NY_BT_AUDIO_OWNER_MEDIA 1 /* A2DP music */
#define NY_BT_AUDIO_OWNER_CALL  2 /* HFP call */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Sends one SCO payload; returns zero or a negated errno value. */

typedef int (*ny_bt_audio_sco_send_t)(const uint8_t *data, size_t size);

struct ny_bt_audio_status_s
{
  int owner;      /* NY_BT_AUDIO_OWNER_* */
  bool held;      /* Flash music or the alert chime has the speaker */
  bool streaming; /* A2DP media is arriving */
  bool msbc;      /* Call codec: mSBC, else CVSD */
  uint32_t rate;  /* Rate of the open device, 0 when closed */
  uint8_t channels;
  int error; /* Last device error */
  uint32_t media_packets;
  uint32_t media_frames;
  uint32_t media_bad;     /* Frames that did not decode */
  uint32_t media_dropped; /* Packets dropped: ring full or speaker held */
  uint32_t media_underruns;
  uint32_t media_buffer_ms;
  uint32_t sco_rx_packets;
  uint32_t sco_rx_bad; /* Packets the controller flagged, frames lost */
  uint32_t sco_tx_packets;
  uint32_t sco_tx_failed;
  uint32_t gate_closures;
  bool gate_closed;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_bt_audio_start(void);
void ny_bt_audio_shutdown(void);

/* A2DP sink.  configure is stack context and cheap. */

void ny_bt_audio_media_configure(uint32_t rate, uint8_t channels,
                                 uint16_t frame_samples);
void ny_bt_audio_media_start(void);
void ny_bt_audio_media_stop(void);

/****************************************************************************
 * Name: ny_bt_audio_media_packet
 *
 * Description:
 *   Stack context.  One A2DP media payload as it follows the RTP header: a
 *   byte whose low nibble counts the SBC frames, then the frames.
 *
 ****************************************************************************/

void ny_bt_audio_media_packet(const uint8_t *data, size_t size);

/* HFP call audio.  Stack context, all three. */

void ny_bt_audio_call_start(bool msbc, ny_bt_audio_sco_send_t send);
void ny_bt_audio_call_stop(void);
void ny_bt_audio_call_packet(const uint8_t *data, size_t size,
                             uint8_t packet_status);

/****************************************************************************
 * Name: ny_bt_audio_hold / ny_bt_audio_release
 *
 * Description:
 *   The flash player wants the speaker.  hold waits until the audio thread
 *   has let go of the device; Bluetooth music is then discarded until
 *   release.  A call is never interrupted.
 *
 * Returned Value:
 *   Zero, -EBUSY during a call, -ETIMEDOUT when the device was not closed in
 *   time.
 *
 ****************************************************************************/

int ny_bt_audio_hold(int timeout_ms);
void ny_bt_audio_release(void);

void ny_bt_audio_status(struct ny_bt_audio_status_s *status);

#endif /* CONFIG_NYABULA_CORE_BT */
#endif /* __NYABULA_CORE_NY_PRODUCT_BT_AUDIO_H */
