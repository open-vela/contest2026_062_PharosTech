/****************************************************************************
 * tools/amp/protocol/nyamp_vision_wire.h
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

#ifndef __TOOLS_AMP_PROTOCOL_NYAMP_VISION_WIRE_H
#define __TOOLS_AMP_PROTOCOL_NYAMP_VISION_WIRE_H
#include "nyamp_protocol.h"
#include <string.h>

#define NYAMP_VISION_LOAD              1U
#define NYAMP_VISION_BEGIN             2U
#define NYAMP_VISION_FRAME             3U
#define NYAMP_VISION_END               4U
#define NYAMP_VISION_UNLOAD            5U
#define NYAMP_VISION_IDENTITY_LOAD     6U
#define NYAMP_VISION_EMBED             7U
#define NYAMP_VISION_TRACK_LOAD        8U
#define NYAMP_VISION_TRACK_INIT        9U
#define NYAMP_VISION_TRACK_FRAME       10U
#define NYAMP_VISION_TRACK_INIT_SIZE   (NYAMP_VISION_FRAME_SIZE + 16U)
#define NYAMP_VISION_TRACK_RESULT_SIZE 48U

/* TRACK_INIT appends normalized x/y/width/height float32 to FRAME.
 * TRACK_RESULT: sequence u64, stamp u64, phase/progress/views u32,
 * normalized x/y/width/height and confidence float32. Phase 1=learning,
 * 2=tracking, 3=lost. Only phase 2 may drive gaze.
 */
#define NYAMP_VISION_EMBED_SIZE   (NYAMP_VISION_FRAME_SIZE + 4U)
#define NYAMP_VISION_EMBED_HEADER 24U
#define NYAMP_VISION_EMBED_DIM    128U

/* EMBED adds the selected face index to FRAME's request. Its response is
 * sequence, timestamp, total detected faces, selected index (UINT32_MAX when
 * no face meets capture quality), one ordinary face record and 128 signed
 * Q15 feature components. The existing FRAME layout remains unchanged.
 */
#define NYAMP_VISION_WIDTH         640U
#define NYAMP_VISION_HEIGHT        480U
#define NYAMP_VISION_BYTES         (NYAMP_VISION_WIDTH * NYAMP_VISION_HEIGHT * 3U / 2U)
#define NYAMP_VISION_FRAME_SIZE    (NYAMP_BUFFER_SIZE + 16U)
#define NYAMP_VISION_RESULT_HEADER 20U
#define NYAMP_VISION_FACE_SIZE     60U
#define NYAMP_VISION_MAX_FACES     5U

/* FRAME: buffer descriptor, sequence u64, monotonic capture milliseconds u64.
 * Bytes are tightly packed, unmirrored VGA NV12 in limited-range BT.601.
 * One frame may be in flight. The producer cannot overwrite its grant until
 * the deferred FRAME response arrives or END returns successfully.
 * Result: sequence u64, timestamp u64, count u32, then 15 float32 per face:
 * normalized x/y/width/height, confidence, and five normalized x/y landmarks.
 */
static inline uint64_t nyamp_vision_get(const uint8_t *data, unsigned int size)
{
  uint64_t value = 0;
  for (unsigned int i = 0; i < size; ++i)
    value |= (uint64_t)data[i] << (i * 8);
  return value;
}
static inline void nyamp_vision_put(uint8_t *data, uint64_t value,
                                    unsigned int size)
{
  for (unsigned int i = 0; i < size; ++i)
    data[i] = (uint8_t)(value >> (i * 8));
}
static inline void nyamp_vision_put_float(uint8_t *data, float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  nyamp_vision_put(data, bits, 4);
}
static inline float nyamp_vision_get_float(const uint8_t *data)
{
  uint32_t bits = (uint32_t)nyamp_vision_get(data, 4);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}
#endif
