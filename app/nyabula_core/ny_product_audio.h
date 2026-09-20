/****************************************************************************
 * app/nyabula_core/ny_product_audio.h
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

#ifndef __NYABULA_CORE_NY_PRODUCT_AUDIO_H
#define __NYABULA_CORE_NY_PRODUCT_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

/* This header is also included by the audioctl command, which has no cJSON
 * include path: the request entry point that needs cJSON is declared in
 * ny_product.h with the other product topics.
 */

#ifdef CONFIG_NYABULA_CORE_AUDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The ES8388 playback and capture nodes the board registers. */

#define NY_PRODUCT_AUDIO_OUTPUT CONFIG_NYABULA_CORE_AUDIO_OUTPUT_DEVICE
#define NY_PRODUCT_AUDIO_INPUT  CONFIG_NYABULA_CORE_AUDIO_INPUT_DEVICE

/* Pass as a level to ny_product_audio_set_levels() to leave it unchanged. */

#define NY_PRODUCT_AUDIO_KEEP (-1)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The codec cannot be asked for these, so the service is their only record:
 * every writer has to go through ny_product_audio_set_levels().
 */

struct ny_product_audio_levels_s
{
  int volume;  /* 0..100, the level restored when unmuted */
  bool muted;  /* Playback mute */
  int gain;    /* 0..100 microphone gain */
  int gain_db; /* What the codec PGA was given for that gain */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ny_product_audio_levels
 *
 * Description:
 *   The levels last given to the codec.  Never touches the product store,
 *   so it is safe on a small command stack.
 *
 * Returned Value:
 *   Zero, or a negated errno value.
 *
 ****************************************************************************/

int ny_product_audio_levels(struct ny_product_audio_levels_s *levels);

/****************************************************************************
 * Name: ny_product_audio_set_levels
 *
 * Description:
 *   Give the codec a new volume (0..100), mute (0 or 1) and/or microphone
 *   gain (0..100); NY_PRODUCT_AUDIO_KEEP leaves one as it is.  Works with or
 *   without a stream open.  The store is written later by the product
 *   worker, never here, so this too is safe on a small command stack.
 *
 * Returned Value:
 *   Zero, -ENODEV while the codec node is not registered, or another
 *   negated errno value.
 *
 ****************************************************************************/

int ny_product_audio_set_levels(int volume, int muted, int gain);

#endif /* CONFIG_NYABULA_CORE_AUDIO */
#endif /* __NYABULA_CORE_NY_PRODUCT_AUDIO_H */
