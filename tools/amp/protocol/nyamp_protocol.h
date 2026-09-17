/****************************************************************************
 * tools/amp/protocol/nyamp_protocol.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __TOOLS_AMP_PROTOCOL_NYAMP_PROTOCOL_H
#define __TOOLS_AMP_PROTOCOL_NYAMP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NYAMP_WIRE_MAGIC       0x5041594eU /* "NYAP" in little endian */
#define NYAMP_WIRE_VERSION     1U
#define NYAMP_WIRE_HEADER_SIZE 40U
#define NYAMP_RPMSG_MTU        496U
#define NYAMP_INLINE_MAX       (NYAMP_RPMSG_MTU - NYAMP_WIRE_HEADER_SIZE)

#define NYAMP_FLAG_REQUEST     (1U << 0)
#define NYAMP_FLAG_RESPONSE    (1U << 1)
#define NYAMP_FLAG_EVENT       (1U << 2)
#define NYAMP_FLAG_CANCEL      (1U << 3)
#define NYAMP_FLAG_ERROR       (1U << 4)
#define NYAMP_FLAG_KIND_MASK                                     \
  (NYAMP_FLAG_REQUEST | NYAMP_FLAG_RESPONSE | NYAMP_FLAG_EVENT | \
   NYAMP_FLAG_CANCEL)
#define NYAMP_FLAG_ALL     (NYAMP_FLAG_KIND_MASK | NYAMP_FLAG_ERROR)

#define NYAMP_HEALTH_READY 0U

/* LLM service opcodes.
 *
 * LOAD/UNLOAD/GENERATE/CANCEL are requests; the service answers each with one
 * response.  EVENT_TOKEN and EVENT_FINISH are unsolicited events that follow
 * an accepted GENERATE and are tied to it by request_id.  A rejected GENERATE
 * produces no events, so a client must not wait for a terminal event after a
 * response that carries NYAMP_FLAG_ERROR.
 *
 * GENERATE carries a token array that exceeds the RPMsg inline limit, so it is
 * split into ordered chunks that are sent back to back on one endpoint.  Only
 * one generate may be in flight per endpoint; a chunk whose total/offset/count
 * do not continue the previous chunk is rejected and the partial request is
 * discarded.
 */

#define NYAMP_LLM_LOAD         1U
#define NYAMP_LLM_UNLOAD       2U
#define NYAMP_LLM_GENERATE     3U
#define NYAMP_LLM_CANCEL       4U
#define NYAMP_LLM_EVENT_TOKEN  0x80U
#define NYAMP_LLM_EVENT_FINISH 0x81U

/* Every generate chunk starts with four little-endian u32 fields. */
#define NYAMP_LLM_CHUNK_HEADER_SIZE 16U
#define NYAMP_LLM_MAX_CHUNK_IDS \
  ((NYAMP_INLINE_MAX - NYAMP_LLM_CHUNK_HEADER_SIZE) / 4U)

/* token_id, sequence, text length, then that many UTF-8 bytes. */
#define NYAMP_LLM_TOKEN_HEADER_SIZE 12U
#define NYAMP_LLM_MAX_TOKEN_TEXT \
  (NYAMP_INLINE_MAX - NYAMP_LLM_TOKEN_HEADER_SIZE)

/* status, sequence. */
#define NYAMP_LLM_FINISH_SIZE 8U

/* CANCEL carries no payload.  The target request_id travels in the wire
 * header's request_id field, matching the existing rule that a cancel message
 * has payload_size == 0.
 */

/* Model directory path, UTF-8, not necessarily NUL terminated. */
#define NYAMP_LLM_MAX_PATH NYAMP_INLINE_MAX

enum nyamp_service_e
{
  NYAMP_SERVICE_HEALTH = 1,
  NYAMP_SERVICE_NPU = 2,
  NYAMP_SERVICE_ASR = 3,
  NYAMP_SERVICE_TTS = 4,
  NYAMP_SERVICE_VISION = 5,
  NYAMP_SERVICE_MEDIA = 6,
  NYAMP_SERVICE_HOME = 7,
  NYAMP_SERVICE_LLM = 8,
};

enum nyamp_result_e
{
  NYAMP_OK = 0,
  NYAMP_EINVAL = -1,
  NYAMP_EMSGSIZE = -2,
  NYAMP_EPROTO = -3,
};

/* Application status carried in the payload, distinct from the wire-level
 * nyamp_result_e returned by the codecs.  Values match
 * nyamp::models::Status in tools/amp/models/nyamp_models.h so neither side
 * has to translate, and the negative range stays disjoint from wire errors.
 */

enum nyamp_model_status_e
{
  NYAMP_MODEL_OK = 0,
  NYAMP_MODEL_INVALID = -1,
  NYAMP_MODEL_NOT_READY = -2,
  NYAMP_MODEL_BUSY = -3,
  NYAMP_MODEL_STALE_GENERATION = -4,
  NYAMP_MODEL_DUPLICATE = -5,
  NYAMP_MODEL_CANCELLED = -6,
  NYAMP_MODEL_DEADLINE = -7,
  NYAMP_MODEL_BACKEND_ERROR = -8,
  NYAMP_MODEL_UNSUPPORTED = -9,
  NYAMP_MODEL_CONSUMER_STOPPED = -10,
};

struct nyamp_header_s
{
  uint16_t service;
  uint16_t opcode;
  uint32_t flags;
  uint64_t request_id;
  uint64_t deadline_ms;
  uint32_t generation;
  uint32_t payload_size;
};

struct nyamp_llm_chunk_s
{
  uint32_t total;          /* Total ids in the whole request.        */
  uint32_t offset;         /* Index of ids[0] within that total.     */
  uint32_t count;          /* Ids carried by this chunk.             */
  uint32_t max_new_tokens; /* Meaningful only when offset is zero.   */
};

int nyamp_header_encode(uint8_t *wire, size_t wire_size,
                        const struct nyamp_header_s *header);
int nyamp_header_decode(struct nyamp_header_s *header, const uint8_t *wire,
                        size_t wire_size);

int nyamp_llm_chunk_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size,
                           const struct nyamp_llm_chunk_s *chunk,
                           const int32_t *ids);
int nyamp_llm_chunk_decode(struct nyamp_llm_chunk_s *chunk, int32_t *ids,
                           size_t id_capacity, size_t *id_count,
                           const uint8_t *payload, size_t payload_size);

int nyamp_llm_token_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t token_id,
                           uint32_t sequence, const char *text,
                           size_t text_length);
int nyamp_llm_token_decode(uint32_t *token_id, uint32_t *sequence,
                           const char **text, size_t *text_length,
                           const uint8_t *payload, size_t payload_size);

int nyamp_llm_finish_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, int32_t status,
                            uint32_t sequence);
int nyamp_llm_finish_decode(int32_t *status, uint32_t *sequence,
                            const uint8_t *payload, size_t payload_size);

#ifdef __cplusplus
}
#endif

#endif /* __TOOLS_AMP_PROTOCOL_NYAMP_PROTOCOL_H */
