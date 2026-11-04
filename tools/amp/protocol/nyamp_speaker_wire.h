/****************************************************************************
 * tools/amp/protocol/nyamp_speaker_wire.h
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


#ifndef __TOOLS_AMP_PROTOCOL_NYAMP_SPEAKER_WIRE_H
#define __TOOLS_AMP_PROTOCOL_NYAMP_SPEAKER_WIRE_H
#include "nyamp_protocol.h"
#include <string.h>
#define NYAMP_SPK_LOAD 1U
#define NYAMP_SPK_UNLOAD 2U
#define NYAMP_SPK_ENROL_BEGIN 3U
#define NYAMP_SPK_ENROL_ADD 4U
#define NYAMP_SPK_ENROL_COMMIT 5U
#define NYAMP_SPK_ENROL_ABORT 6U
#define NYAMP_SPK_TEMPLATE_PUT 7U
#define NYAMP_SPK_DELETE 8U
#define NYAMP_SPK_VERIFY 10U
#define NYAMP_SPK_RANGE_SIZE 24U
#define NYAMP_SPK_TEMPLATE_SIZE 400U
/* Range: KWS BEGIN request id, start sample, end sample (three LE u64s).
 * A stream id is required: a sample offset alone cannot survive a restart.
 * ADD replies with take count; COMMIT with kept count, cohesion and NYVP.
 * VERIFY replies with cosine score and sample count, never authorization.
 */
static inline uint64_t nyamp_spk_get(const uint8_t *p,unsigned int n)
{
  uint64_t v=0;for(unsigned int i=0;i<n;i++)v|=(uint64_t)p[i]<<(i*8);return v;
}
static inline void nyamp_spk_put(uint8_t *p,uint64_t v,unsigned int n)
{
  for(unsigned int i=0;i<n;i++)p[i]=(uint8_t)(v>>(i*8));
}
static inline void nyamp_spk_float_put(uint8_t *p,float f)
{
  uint32_t v;memcpy(&v,&f,4);nyamp_spk_put(p,v,4);
}
static inline float nyamp_spk_float_get(const uint8_t *p)
{
  uint32_t v=(uint32_t)nyamp_spk_get(p,4);float f;memcpy(&f,&v,4);return f;
}
#endif
