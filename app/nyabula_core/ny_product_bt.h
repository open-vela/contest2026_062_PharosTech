/****************************************************************************
 * app/nyabula_core/ny_product_bt.h
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

#ifndef __NYABULA_CORE_NY_PRODUCT_BT_H
#define __NYABULA_CORE_NY_PRODUCT_BT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

/* Bluetooth speaker (A2DP sink + AVRCP) and hands-free unit (HFP) on the
 * ZBlue host.  The request entry point needs cJSON and is declared with the
 * other product topics in ny_product.h; what is here can be included from
 * anywhere.
 *
 * Topics (owner only, except bt.status which family members may read):
 *
 *   bt.status
 *   bt.discoverable  {enabled, seconds?}   opens the pairing window
 *   bt.pair.confirm  {accept, address?}    answers a pending pairing
 *   bt.devices                             bonded and connected devices
 *   bt.forget        {address}
 *   bt.connect       {address?}            default: last connected device
 *   bt.disconnect    {address?}
 *   bt.media.play | pause | next | prev | stop
 *   bt.call.answer | reject | hangup
 */

#ifdef CONFIG_NYABULA_CORE_BT

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_product_bt_tick(void);
void ny_product_bt_shutdown(void);

/****************************************************************************
 * Name: ny_product_bt_speaker_claim / ny_product_bt_speaker_release
 *
 * Description:
 *   For the flash player, the other user of the speaker.  claim is called
 *   before it opens the device: Bluetooth music lets go and stays away (the
 *   phone is asked to pause unless the claim is for the alert chime, after
 *   which the music simply comes back).  release is called once the player
 *   is idle again; it costs nothing when nothing was claimed.
 *
 * Returned Value:
 *   claim: zero, -EBUSY during a phone call (a call is never interrupted:
 *   an alert waits for its end, see ny_product_bt_call_active()), or
 *   -ETIMEDOUT.
 *
 ****************************************************************************/

int ny_product_bt_speaker_claim(bool alert);
void ny_product_bt_speaker_release(void);

/****************************************************************************
 * Name: ny_product_bt_call_active
 *
 * Description:
 *   A call is ringing, being set up or in progress, or its audio link is up.
 *
 ****************************************************************************/

bool ny_product_bt_call_active(void);

/****************************************************************************
 * Name: ny_product_bt_speaker_source
 *
 * Description:
 *   "bluetooth" or "call" while Bluetooth audio is on the speaker, else
 *   NULL.  For music.status.
 *
 ****************************************************************************/

const char *ny_product_bt_speaker_source(void);

#endif /* CONFIG_NYABULA_CORE_BT */
#endif /* __NYABULA_CORE_NY_PRODUCT_BT_H */
