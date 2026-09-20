/****************************************************************************
 * tools/amp/protocol/nyamp_protocol.c
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

#include "nyamp_protocol.h"

#include <string.h>

#define NYAMP_MAGIC_OFFSET        0U
#define NYAMP_VERSION_OFFSET      4U
#define NYAMP_HEADER_SIZE_OFFSET  6U
#define NYAMP_SERVICE_OFFSET      8U
#define NYAMP_OPCODE_OFFSET       10U
#define NYAMP_FLAGS_OFFSET        12U
#define NYAMP_REQUEST_ID_OFFSET   16U
#define NYAMP_DEADLINE_OFFSET     24U
#define NYAMP_GENERATION_OFFSET   32U
#define NYAMP_PAYLOAD_SIZE_OFFSET 36U

static void nyamp_put_le16(uint8_t *dest, uint16_t value);
static void nyamp_put_le32(uint8_t *dest, uint32_t value);
static void nyamp_put_le64(uint8_t *dest, uint64_t value);
static uint16_t nyamp_get_le16(const uint8_t *source);
static uint32_t nyamp_get_le32(const uint8_t *source);
static uint64_t nyamp_get_le64(const uint8_t *source);
static int nyamp_header_validate(const struct nyamp_header_s *header);
static int nyamp_payload_ready(const uint8_t *payload, size_t payload_size,
                               size_t minimum);
static int nyamp_llm_span_check(uint32_t total, uint32_t offset,
                                uint32_t length, uint32_t max_total,
                                uint32_t max_length);
static int nyamp_blob_text_encode(uint8_t *payload, size_t payload_capacity,
                                  size_t *payload_size, uint32_t first,
                                  const char *text, size_t text_length);
static int nyamp_blob_text_decode(uint32_t *first, const char **text,
                                  size_t *text_length, const uint8_t *payload,
                                  size_t payload_size);

static void nyamp_put_le16(uint8_t *dest, uint16_t value)
{
  dest[0] = (uint8_t)value;
  dest[1] = (uint8_t)(value >> 8);
}

static void nyamp_put_le32(uint8_t *dest, uint32_t value)
{
  unsigned int index;

  for (index = 0; index < 4; index++)
    {
      dest[index] = (uint8_t)(value >> (index * 8));
    }
}

static void nyamp_put_le64(uint8_t *dest, uint64_t value)
{
  unsigned int index;

  for (index = 0; index < 8; index++)
    {
      dest[index] = (uint8_t)(value >> (index * 8));
    }
}

static uint16_t nyamp_get_le16(const uint8_t *source)
{
  return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static uint32_t nyamp_get_le32(const uint8_t *source)
{
  uint32_t value = 0;
  unsigned int index;

  for (index = 0; index < 4; index++)
    {
      value |= (uint32_t)source[index] << (index * 8);
    }

  return value;
}

static uint64_t nyamp_get_le64(const uint8_t *source)
{
  uint64_t value = 0;
  unsigned int index;

  for (index = 0; index < 8; index++)
    {
      value |= (uint64_t)source[index] << (index * 8);
    }

  return value;
}

static int nyamp_header_validate(const struct nyamp_header_s *header)
{
  uint32_t kind;

  if (header == NULL || header->service == 0 || header->request_id == 0)
    {
      return NYAMP_EINVAL;
    }

  if (header->payload_size > NYAMP_INLINE_MAX)
    {
      return NYAMP_EMSGSIZE;
    }

  if ((header->flags & ~NYAMP_FLAG_ALL) != 0)
    {
      return NYAMP_EPROTO;
    }

  kind = header->flags & NYAMP_FLAG_KIND_MASK;
  if (kind == 0 || (kind & (kind - 1)) != 0)
    {
      return NYAMP_EPROTO;
    }

  if ((header->flags & NYAMP_FLAG_ERROR) != 0 && kind != NYAMP_FLAG_RESPONSE)
    {
      return NYAMP_EPROTO;
    }

  if (kind == NYAMP_FLAG_CANCEL && header->payload_size != 0)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

static int nyamp_payload_ready(const uint8_t *payload, size_t payload_size,
                               size_t minimum)
{
  if (payload == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_size < minimum)
    {
      return NYAMP_EMSGSIZE;
    }

  return NYAMP_OK;
}

int nyamp_header_encode(uint8_t *wire, size_t wire_size,
                        const struct nyamp_header_s *header)
{
  int result = nyamp_header_validate(header);

  if (wire == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (result != NYAMP_OK)
    {
      return result;
    }

  if (wire_size < NYAMP_WIRE_HEADER_SIZE + header->payload_size)
    {
      return NYAMP_EMSGSIZE;
    }

  memset(wire, 0, NYAMP_WIRE_HEADER_SIZE);
  nyamp_put_le32(wire + NYAMP_MAGIC_OFFSET, NYAMP_WIRE_MAGIC);
  nyamp_put_le16(wire + NYAMP_VERSION_OFFSET, NYAMP_WIRE_VERSION);
  nyamp_put_le16(wire + NYAMP_HEADER_SIZE_OFFSET, NYAMP_WIRE_HEADER_SIZE);
  nyamp_put_le16(wire + NYAMP_SERVICE_OFFSET, header->service);
  nyamp_put_le16(wire + NYAMP_OPCODE_OFFSET, header->opcode);
  nyamp_put_le32(wire + NYAMP_FLAGS_OFFSET, header->flags);
  nyamp_put_le64(wire + NYAMP_REQUEST_ID_OFFSET, header->request_id);
  nyamp_put_le64(wire + NYAMP_DEADLINE_OFFSET, header->deadline_ms);
  nyamp_put_le32(wire + NYAMP_GENERATION_OFFSET, header->generation);
  nyamp_put_le32(wire + NYAMP_PAYLOAD_SIZE_OFFSET, header->payload_size);
  return NYAMP_OK;
}

int nyamp_header_decode(struct nyamp_header_s *header, const uint8_t *wire,
                        size_t wire_size)
{
  if (header == NULL || wire == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (wire_size < NYAMP_WIRE_HEADER_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  if (nyamp_get_le32(wire + NYAMP_MAGIC_OFFSET) != NYAMP_WIRE_MAGIC ||
      nyamp_get_le16(wire + NYAMP_VERSION_OFFSET) != NYAMP_WIRE_VERSION ||
      nyamp_get_le16(wire + NYAMP_HEADER_SIZE_OFFSET) !=
          NYAMP_WIRE_HEADER_SIZE)
    {
      return NYAMP_EPROTO;
    }

  header->service = nyamp_get_le16(wire + NYAMP_SERVICE_OFFSET);
  header->opcode = nyamp_get_le16(wire + NYAMP_OPCODE_OFFSET);
  header->flags = nyamp_get_le32(wire + NYAMP_FLAGS_OFFSET);
  header->request_id = nyamp_get_le64(wire + NYAMP_REQUEST_ID_OFFSET);
  header->deadline_ms = nyamp_get_le64(wire + NYAMP_DEADLINE_OFFSET);
  header->generation = nyamp_get_le32(wire + NYAMP_GENERATION_OFFSET);
  header->payload_size = nyamp_get_le32(wire + NYAMP_PAYLOAD_SIZE_OFFSET);

  if (wire_size < NYAMP_WIRE_HEADER_SIZE + header->payload_size)
    {
      return NYAMP_EMSGSIZE;
    }

  return nyamp_header_validate(header);
}

/****************************************************************************
 * Name: nyamp_llm_chunk_encode
 *
 * Description:
 *   Encode one ordered slice of a generate token array.  The chunk header is
 *   four little-endian u32 fields followed by `count` signed 32-bit ids.
 *
 ****************************************************************************/

int nyamp_llm_chunk_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size,
                           const struct nyamp_llm_chunk_s *chunk,
                           const int32_t *ids)
{
  size_t index;
  size_t needed;

  if (payload == NULL || payload_size == NULL || chunk == NULL || ids == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (chunk->count == 0 || chunk->count > NYAMP_LLM_MAX_CHUNK_IDS ||
      chunk->offset > chunk->total ||
      chunk->count > chunk->total - chunk->offset)
    {
      return NYAMP_EINVAL;
    }

  needed = NYAMP_LLM_CHUNK_HEADER_SIZE + (size_t)chunk->count * 4U;
  if (payload_capacity < needed)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, chunk->total);
  nyamp_put_le32(payload + 4, chunk->offset);
  nyamp_put_le32(payload + 8, chunk->count);
  nyamp_put_le32(payload + 12, chunk->max_new_tokens);
  for (index = 0; index < chunk->count; index++)
    {
      nyamp_put_le32(payload + NYAMP_LLM_CHUNK_HEADER_SIZE + index * 4U,
                     (uint32_t)ids[index]);
    }

  *payload_size = needed;
  return NYAMP_OK;
}

/****************************************************************************
 * Name: nyamp_llm_chunk_decode
 *
 * Description:
 *   Decode a generate chunk.  Rejects any shape the encoder could not have
 *   produced so the daemon never has to reason about a truncated array.
 *
 ****************************************************************************/

int nyamp_llm_chunk_decode(struct nyamp_llm_chunk_s *chunk, int32_t *ids,
                           size_t id_capacity, size_t *id_count,
                           const uint8_t *payload, size_t payload_size)
{
  int result;
  size_t index;
  size_t needed;

  if (chunk == NULL || ids == NULL || id_count == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_LLM_CHUNK_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  chunk->total = nyamp_get_le32(payload + 0);
  chunk->offset = nyamp_get_le32(payload + 4);
  chunk->count = nyamp_get_le32(payload + 8);
  chunk->max_new_tokens = nyamp_get_le32(payload + 12);

  if (chunk->count == 0 || chunk->count > NYAMP_LLM_MAX_CHUNK_IDS ||
      chunk->offset > chunk->total ||
      chunk->count > chunk->total - chunk->offset)
    {
      return NYAMP_EPROTO;
    }

  needed = NYAMP_LLM_CHUNK_HEADER_SIZE + (size_t)chunk->count * 4U;
  if (payload_size != needed || id_capacity < chunk->count)
    {
      return NYAMP_EMSGSIZE;
    }

  for (index = 0; index < chunk->count; index++)
    {
      ids[index] = (int32_t)nyamp_get_le32(
          payload + NYAMP_LLM_CHUNK_HEADER_SIZE + index * 4U);
      if (ids[index] < 0)
        {
          return NYAMP_EPROTO;
        }
    }

  *id_count = chunk->count;
  return NYAMP_OK;
}

/****************************************************************************
 * Name: nyamp_llm_token_encode / nyamp_llm_token_decode
 *
 * Description:
 *   Encode a streaming token event: token_id, sequence, text length, then the
 *   UTF-8 bytes.  Text is not stored NUL terminated on the wire.
 *
 ****************************************************************************/

int nyamp_llm_token_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t token_id,
                           uint32_t sequence, const char *text,
                           size_t text_length)
{
  size_t needed;

  if (payload == NULL || payload_size == NULL ||
      (text == NULL && text_length != 0))
    {
      return NYAMP_EINVAL;
    }

  if (text_length > NYAMP_LLM_MAX_TOKEN_TEXT)
    {
      return NYAMP_EMSGSIZE;
    }

  needed = NYAMP_LLM_TOKEN_HEADER_SIZE + text_length;
  if (payload_capacity < needed)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, token_id);
  nyamp_put_le32(payload + 4, sequence);
  nyamp_put_le32(payload + 8, (uint32_t)text_length);
  if (text_length != 0)
    {
      memcpy(payload + NYAMP_LLM_TOKEN_HEADER_SIZE, text, text_length);
    }

  *payload_size = needed;
  return NYAMP_OK;
}

int nyamp_llm_token_decode(uint32_t *token_id, uint32_t *sequence,
                           const char **text, size_t *text_length,
                           const uint8_t *payload, size_t payload_size)
{
  int result;
  size_t length;

  if (token_id == NULL || sequence == NULL || text == NULL ||
      text_length == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_LLM_TOKEN_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  length = nyamp_get_le32(payload + 8);
  if (payload_size != NYAMP_LLM_TOKEN_HEADER_SIZE + length)
    {
      return NYAMP_EPROTO;
    }

  *token_id = nyamp_get_le32(payload + 0);
  *sequence = nyamp_get_le32(payload + 4);
  *text_length = length;
  *text = (const char *)(payload + NYAMP_LLM_TOKEN_HEADER_SIZE);
  return NYAMP_OK;
}

/****************************************************************************
 * Name: nyamp_llm_finish_encode / nyamp_llm_finish_decode
 *
 * Description:
 *   Encode the single terminal event that closes a generate.  `status` is a
 *   nyamp_model_status_e value, so the client can distinguish a completed
 *   generation from a cancelled or failed one.
 *
 ****************************************************************************/

int nyamp_llm_finish_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, int32_t status,
                            uint32_t sequence)
{
  if (payload == NULL || payload_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_LLM_FINISH_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, (uint32_t)status);
  nyamp_put_le32(payload + 4, sequence);
  *payload_size = NYAMP_LLM_FINISH_SIZE;
  return NYAMP_OK;
}

int nyamp_llm_finish_decode(int32_t *status, uint32_t *sequence,
                            const uint8_t *payload, size_t payload_size)
{
  int result;

  if (status == NULL || sequence == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_LLM_FINISH_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_LLM_FINISH_SIZE)
    {
      return NYAMP_EPROTO;
    }

  *status = (int32_t)nyamp_get_le32(payload + 0);
  *sequence = nyamp_get_le32(payload + 4);
  return NYAMP_OK;
}

/****************************************************************************
 * Shared-memory buffer descriptor
 *
 * A descriptor names a region by absolute offset and length rather than a slot
 * index, so the placement policy can change without touching the wire.  The
 * decoder rejects any shape the encoder could not have produced, so a consumer
 * never has to reason about a truncated or self-inconsistent grant.
 *
 ****************************************************************************/

int nyamp_buffer_encode(uint8_t *payload, size_t payload_capacity,
                        size_t *payload_size,
                        const struct nyamp_buffer_s *buffer)
{
  if (payload == NULL || payload_size == NULL || buffer == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_BUFFER_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  /* An empty buffer is legal as a release, but valid bytes can never exceed
   * the granted capacity, and no flag outside the defined set is meaningful.
   */
  if ((buffer->flags & ~NYAMP_BUFFER_ALL) != 0 ||
      buffer->length > buffer->capacity)
    {
      return NYAMP_EINVAL;
    }

  nyamp_put_le32(payload + 0, NYAMP_BUFFER_MAGIC);
  nyamp_put_le16(payload + 4, NYAMP_BUFFER_VERSION);
  nyamp_put_le16(payload + 6, buffer->flags);
  nyamp_put_le32(payload + 8, buffer->offset);
  nyamp_put_le32(payload + 12, buffer->length);
  nyamp_put_le32(payload + 16, buffer->capacity);
  nyamp_put_le32(payload + 20, buffer->format);
  nyamp_put_le64(payload + 24, buffer->lease);
  nyamp_put_le32(payload + 32, buffer->generation);
  nyamp_put_le32(payload + 36, buffer->reserved);

  *payload_size = NYAMP_BUFFER_SIZE;
  return NYAMP_OK;
}

int nyamp_buffer_decode(struct nyamp_buffer_s *buffer, const uint8_t *payload,
                        size_t payload_size)
{
  int result;

  if (buffer == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_BUFFER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_BUFFER_SIZE)
    {
      return NYAMP_EPROTO;
    }

  if (nyamp_get_le32(payload + 0) != NYAMP_BUFFER_MAGIC ||
      nyamp_get_le16(payload + 4) != NYAMP_BUFFER_VERSION)
    {
      return NYAMP_EPROTO;
    }

  buffer->magic = NYAMP_BUFFER_MAGIC;
  buffer->version = NYAMP_BUFFER_VERSION;
  buffer->flags = nyamp_get_le16(payload + 6);
  buffer->offset = nyamp_get_le32(payload + 8);
  buffer->length = nyamp_get_le32(payload + 12);
  buffer->capacity = nyamp_get_le32(payload + 16);
  buffer->format = nyamp_get_le32(payload + 20);
  buffer->lease = nyamp_get_le64(payload + 24);
  buffer->generation = nyamp_get_le32(payload + 32);
  buffer->reserved = nyamp_get_le32(payload + 36);

  if ((buffer->flags & ~NYAMP_BUFFER_ALL) != 0 ||
      buffer->length > buffer->capacity || buffer->reserved != 0)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

/****************************************************************************
 * ASR
 ****************************************************************************/

int nyamp_asr_begin_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t sample_rate,
                           uint16_t channels, uint16_t flags,
                           uint32_t max_samples)
{
  if (payload == NULL || payload_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_ASR_BEGIN_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, sample_rate);
  nyamp_put_le16(payload + 4, channels);
  nyamp_put_le16(payload + 6, flags);
  nyamp_put_le32(payload + 8, max_samples);
  nyamp_put_le32(payload + 12, 0);

  *payload_size = NYAMP_ASR_BEGIN_SIZE;
  return NYAMP_OK;
}

int nyamp_asr_begin_decode(uint32_t *sample_rate, uint16_t *channels,
                           uint16_t *flags, uint32_t *max_samples,
                           const uint8_t *payload, size_t payload_size)
{
  int result;

  if (sample_rate == NULL || channels == NULL || flags == NULL ||
      max_samples == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_ASR_BEGIN_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_ASR_BEGIN_SIZE)
    {
      return NYAMP_EPROTO;
    }

  *sample_rate = nyamp_get_le32(payload + 0);
  *channels = nyamp_get_le16(payload + 4);
  *flags = nyamp_get_le16(payload + 6);
  *max_samples = nyamp_get_le32(payload + 8);
  return NYAMP_OK;
}

int nyamp_asr_push_encode(uint8_t *payload, size_t payload_capacity,
                          size_t *payload_size,
                          const struct nyamp_buffer_s *buffer,
                          uint32_t sequence, uint16_t flags,
                          uint32_t total_samples, uint32_t consumed_samples)
{
  size_t used = 0;
  int result;

  if (payload == NULL || payload_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_ASR_PUSH_HEADER_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  result = nyamp_buffer_encode(payload, payload_capacity, &used, buffer);
  if (result != NYAMP_OK)
    {
      return result;
    }

  nyamp_put_le32(payload + NYAMP_BUFFER_SIZE + 0, sequence);
  nyamp_put_le16(payload + NYAMP_BUFFER_SIZE + 4, flags);
  nyamp_put_le16(payload + NYAMP_BUFFER_SIZE + 6, 0);
  nyamp_put_le32(payload + NYAMP_BUFFER_SIZE + 8, total_samples);
  nyamp_put_le32(payload + NYAMP_BUFFER_SIZE + 12, consumed_samples);

  *payload_size = NYAMP_ASR_PUSH_HEADER_SIZE;
  return NYAMP_OK;
}

int nyamp_asr_push_decode(struct nyamp_buffer_s *buffer, uint32_t *sequence,
                          uint16_t *flags, uint32_t *total_samples,
                          uint32_t *consumed_samples, const uint8_t *payload,
                          size_t payload_size)
{
  int result;

  if (buffer == NULL || sequence == NULL || flags == NULL ||
      total_samples == NULL || consumed_samples == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_ASR_PUSH_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_ASR_PUSH_HEADER_SIZE)
    {
      return NYAMP_EPROTO;
    }

  result = nyamp_buffer_decode(buffer, payload, NYAMP_BUFFER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  *sequence = nyamp_get_le32(payload + NYAMP_BUFFER_SIZE + 0);
  *flags = nyamp_get_le16(payload + NYAMP_BUFFER_SIZE + 4);
  *total_samples = nyamp_get_le32(payload + NYAMP_BUFFER_SIZE + 8);
  *consumed_samples = nyamp_get_le32(payload + NYAMP_BUFFER_SIZE + 12);
  return NYAMP_OK;
}

int nyamp_asr_partial_encode(uint8_t *payload, size_t payload_capacity,
                             size_t *payload_size, uint32_t sequence,
                             uint32_t consumed_samples, uint16_t flags,
                             const char *text, size_t text_length)
{
  size_t needed;

  if (payload == NULL || payload_size == NULL ||
      (text == NULL && text_length != 0))
    {
      return NYAMP_EINVAL;
    }

  if (text_length > NYAMP_ASR_MAX_TEXT)
    {
      return NYAMP_EMSGSIZE;
    }

  needed = NYAMP_ASR_PARTIAL_HEADER_SIZE + text_length;
  if (payload_capacity < needed)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, sequence);
  nyamp_put_le32(payload + 4, consumed_samples);
  nyamp_put_le16(payload + 8, flags);
  nyamp_put_le16(payload + 10, 0);
  if (text_length != 0)
    {
      memcpy(payload + NYAMP_ASR_PARTIAL_HEADER_SIZE, text, text_length);
    }

  *payload_size = needed;
  return NYAMP_OK;
}

int nyamp_asr_partial_decode(uint32_t *sequence, uint32_t *consumed_samples,
                             uint16_t *flags, const char **text,
                             size_t *text_length, const uint8_t *payload,
                             size_t payload_size)
{
  int result;
  size_t length;

  if (sequence == NULL || consumed_samples == NULL || flags == NULL ||
      text == NULL || text_length == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size,
                               NYAMP_ASR_PARTIAL_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  length = payload_size - NYAMP_ASR_PARTIAL_HEADER_SIZE;
  if (length > NYAMP_ASR_MAX_TEXT)
    {
      return NYAMP_EMSGSIZE;
    }

  *sequence = nyamp_get_le32(payload + 0);
  *consumed_samples = nyamp_get_le32(payload + 4);
  *flags = nyamp_get_le16(payload + 8);
  *text_length = length;
  *text = (const char *)(payload + NYAMP_ASR_PARTIAL_HEADER_SIZE);
  return NYAMP_OK;
}

int nyamp_asr_finish_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, int32_t status,
                            uint32_t sequence)
{
  if (payload == NULL || payload_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_ASR_FINISH_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, (uint32_t)status);
  nyamp_put_le32(payload + 4, sequence);
  *payload_size = NYAMP_ASR_FINISH_SIZE;
  return NYAMP_OK;
}

int nyamp_asr_finish_decode(int32_t *status, uint32_t *sequence,
                            const uint8_t *payload, size_t payload_size)
{
  int result;

  if (status == NULL || sequence == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_ASR_FINISH_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_ASR_FINISH_SIZE)
    {
      return NYAMP_EPROTO;
    }

  *status = (int32_t)nyamp_get_le32(payload + 0);
  *sequence = nyamp_get_le32(payload + 4);
  return NYAMP_OK;
}

/****************************************************************************
 * TTS
 ****************************************************************************/

int nyamp_tts_synth_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t phoneme_count,
                           uint32_t speaker_id, float speed,
                           uint32_t bucket_frames)
{
  uint32_t bits;

  if (payload == NULL || payload_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_TTS_SYNTH_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  /* The float travels as its bit pattern so the wire stays integer-only. */
  memcpy(&bits, &speed, sizeof(bits));
  nyamp_put_le32(payload + 0, phoneme_count);
  nyamp_put_le32(payload + 4, speaker_id);
  nyamp_put_le32(payload + 8, bits);
  nyamp_put_le32(payload + 12, bucket_frames);

  *payload_size = NYAMP_TTS_SYNTH_SIZE;
  return NYAMP_OK;
}

int nyamp_tts_synth_decode(uint32_t *phoneme_count, uint32_t *speaker_id,
                           float *speed, uint32_t *bucket_frames,
                           const uint8_t *payload, size_t payload_size)
{
  int result;
  uint32_t bits;

  if (phoneme_count == NULL || speaker_id == NULL || speed == NULL ||
      bucket_frames == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_TTS_SYNTH_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_TTS_SYNTH_SIZE)
    {
      return NYAMP_EPROTO;
    }

  *phoneme_count = nyamp_get_le32(payload + 0);
  *speaker_id = nyamp_get_le32(payload + 4);
  bits = nyamp_get_le32(payload + 8);
  memcpy(speed, &bits, sizeof(*speed));
  *bucket_frames = nyamp_get_le32(payload + 12);
  return NYAMP_OK;
}

int nyamp_tts_pcm_encode(uint8_t *payload, size_t payload_capacity,
                         size_t *payload_size,
                         const struct nyamp_buffer_s *buffer,
                         uint32_t sequence, uint32_t sample_rate,
                         uint32_t channels, uint32_t valid_samples)
{
  size_t used = 0;
  int result;

  if (payload == NULL || payload_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_TTS_PCM_HEADER_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  result = nyamp_buffer_encode(payload, payload_capacity, &used, buffer);
  if (result != NYAMP_OK)
    {
      return result;
    }

  nyamp_put_le32(payload + NYAMP_BUFFER_SIZE + 0, sequence);
  nyamp_put_le32(payload + NYAMP_BUFFER_SIZE + 4, sample_rate);
  nyamp_put_le32(payload + NYAMP_BUFFER_SIZE + 8, channels);
  nyamp_put_le32(payload + NYAMP_BUFFER_SIZE + 12, valid_samples);

  *payload_size = NYAMP_TTS_PCM_HEADER_SIZE;
  return NYAMP_OK;
}

int nyamp_tts_pcm_decode(struct nyamp_buffer_s *buffer, uint32_t *sequence,
                         uint32_t *sample_rate, uint32_t *channels,
                         uint32_t *valid_samples, const uint8_t *payload,
                         size_t payload_size)
{
  int result;

  if (buffer == NULL || sequence == NULL || sample_rate == NULL ||
      channels == NULL || valid_samples == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_TTS_PCM_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_TTS_PCM_HEADER_SIZE)
    {
      return NYAMP_EPROTO;
    }

  result = nyamp_buffer_decode(buffer, payload, NYAMP_BUFFER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  *sequence = nyamp_get_le32(payload + NYAMP_BUFFER_SIZE + 0);
  *sample_rate = nyamp_get_le32(payload + NYAMP_BUFFER_SIZE + 4);
  *channels = nyamp_get_le32(payload + NYAMP_BUFFER_SIZE + 8);
  *valid_samples = nyamp_get_le32(payload + NYAMP_BUFFER_SIZE + 12);
  return NYAMP_OK;
}

int nyamp_tts_finish_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, int32_t status,
                            uint32_t sequence, uint32_t total_samples)
{
  if (payload == NULL || payload_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_TTS_FINISH_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, (uint32_t)status);
  nyamp_put_le32(payload + 4, sequence);
  nyamp_put_le32(payload + 8, total_samples);
  *payload_size = NYAMP_TTS_FINISH_SIZE;
  return NYAMP_OK;
}

int nyamp_tts_finish_decode(int32_t *status, uint32_t *sequence,
                            uint32_t *total_samples, const uint8_t *payload,
                            size_t payload_size)
{
  int result;

  if (status == NULL || sequence == NULL || total_samples == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_TTS_FINISH_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_TTS_FINISH_SIZE)
    {
      return NYAMP_EPROTO;
    }

  *status = (int32_t)nyamp_get_le32(payload + 0);
  *sequence = nyamp_get_le32(payload + 4);
  *total_samples = nyamp_get_le32(payload + 8);
  return NYAMP_OK;
}

/****************************************************************************
 * Response status prefix
 ****************************************************************************/

int nyamp_status_encode(uint8_t *payload, size_t payload_capacity,
                        int32_t status, uint8_t **body, size_t *body_capacity)
{
  if (payload == NULL || body == NULL || body_capacity == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_STATUS_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload, (uint32_t)status);
  *body = payload + NYAMP_STATUS_SIZE;
  *body_capacity = payload_capacity - NYAMP_STATUS_SIZE;
  return NYAMP_OK;
}

int nyamp_status_decode(int32_t *status, const uint8_t **body,
                        size_t *body_size, const uint8_t *payload,
                        size_t payload_size)
{
  int result;

  if (status == NULL || body == NULL || body_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_STATUS_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  *status = (int32_t)nyamp_get_le32(payload);
  *body = payload + NYAMP_STATUS_SIZE;
  *body_size = payload_size - NYAMP_STATUS_SIZE;
  return NYAMP_OK;
}

/****************************************************************************
 * BLOB
 *
 * The name rules live in the codec rather than in the responder alone: the
 * requester refuses to send an illegal name and the responder refuses to act
 * on one, so a traversal attempt has to get past both and neither side's
 * check can be forgotten by a future caller.
 *
 ****************************************************************************/

int nyamp_blob_name_check(const char *name, size_t name_length)
{
  size_t index;
  size_t start = 0;

  if (name == NULL || name_length == 0 || name_length > NYAMP_BLOB_MAX_NAME)
    {
      return NYAMP_EINVAL;
    }

  for (index = 0; index <= name_length; index++)
    {
      size_t length;

      if (index < name_length)
        {
          uint8_t value = (uint8_t)name[index];

          /* A backslash is a separator to FAT, and a control character (NUL
           * included) would truncate the path the responder builds.
           */
          if (value < 0x20 || value == 0x7f || value == '\\')
            {
              return NYAMP_EINVAL;
            }

          if (value != '/')
            {
              continue;
            }
        }

      /* A component ends here.  Empty covers a leading, trailing or doubled
       * separator, so an absolute path is refused by the same test.
       */
      length = index - start;
      if (length == 0 || (length == 1 && name[start] == '.') ||
          (length == 2 && name[start] == '.' && name[start + 1] == '.'))
        {
          return NYAMP_EINVAL;
        }

      start = index + 1;
    }

  return NYAMP_OK;
}

/****************************************************************************
 * Name: nyamp_blob_text_encode / nyamp_blob_text_decode
 *
 * Description:
 *   OPEN, PULL and LIST share one shape: a u32 (flags or cursor), a u32 text
 *   length, then the text.  The explicit length is redundant with the frame
 *   size on purpose -- a frame whose two lengths disagree is refused instead
 *   of being read as a shorter or longer name.
 *
 ****************************************************************************/

static int nyamp_blob_text_encode(uint8_t *payload, size_t payload_capacity,
                                  size_t *payload_size, uint32_t first,
                                  const char *text, size_t text_length)
{
  size_t needed = NYAMP_BLOB_OPEN_HEADER_SIZE + text_length;

  if (payload == NULL || payload_size == NULL ||
      (text == NULL && text_length != 0))
    {
      return NYAMP_EINVAL;
    }

  if (text_length > NYAMP_BLOB_MAX_NAME || payload_capacity < needed)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, first);
  nyamp_put_le32(payload + 4, (uint32_t)text_length);
  if (text_length != 0)
    {
      memcpy(payload + NYAMP_BLOB_OPEN_HEADER_SIZE, text, text_length);
    }

  *payload_size = needed;
  return NYAMP_OK;
}

static int nyamp_blob_text_decode(uint32_t *first, const char **text,
                                  size_t *text_length, const uint8_t *payload,
                                  size_t payload_size)
{
  int result;
  size_t length;

  if (first == NULL || text == NULL || text_length == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_OPEN_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  length = nyamp_get_le32(payload + 4);
  if (length > NYAMP_BLOB_MAX_NAME ||
      payload_size != NYAMP_BLOB_OPEN_HEADER_SIZE + length)
    {
      return NYAMP_EPROTO;
    }

  *first = nyamp_get_le32(payload + 0);
  *text = (const char *)(payload + NYAMP_BLOB_OPEN_HEADER_SIZE);
  *text_length = length;
  return NYAMP_OK;
}

int nyamp_blob_open_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t flags,
                           const char *name, size_t name_length)
{
  if (flags != 0 || nyamp_blob_name_check(name, name_length) != NYAMP_OK)
    {
      return NYAMP_EINVAL;
    }

  return nyamp_blob_text_encode(payload, payload_capacity, payload_size, flags,
                                name, name_length);
}

int nyamp_blob_open_decode(uint32_t *flags, const char **name,
                           size_t *name_length, const uint8_t *payload,
                           size_t payload_size)
{
  int result = nyamp_blob_text_decode(flags, name, name_length, payload,
                                      payload_size);

  if (result != NYAMP_OK)
    {
      return result;
    }

  if (*flags != 0 || nyamp_blob_name_check(*name, *name_length) != NYAMP_OK)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

int nyamp_blob_info_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size,
                           const struct nyamp_blob_info_s *info)
{
  if (payload == NULL || payload_size == NULL || info == NULL ||
      info->blob_id == 0 || info->flags != 0)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_BLOB_INFO_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, info->blob_id);
  nyamp_put_le32(payload + 4, info->flags);
  nyamp_put_le64(payload + 8, info->size);
  nyamp_put_le64(payload + 16, info->mtime);
  memcpy(payload + 24, info->sha256, NYAMP_BLOB_SHA256_SIZE);
  *payload_size = NYAMP_BLOB_INFO_SIZE;
  return NYAMP_OK;
}

int nyamp_blob_info_decode(struct nyamp_blob_info_s *info,
                           const uint8_t *payload, size_t payload_size)
{
  int result;

  if (info == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_INFO_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_BLOB_INFO_SIZE)
    {
      return NYAMP_EPROTO;
    }

  info->blob_id = nyamp_get_le32(payload + 0);
  info->flags = nyamp_get_le32(payload + 4);
  info->size = nyamp_get_le64(payload + 8);
  info->mtime = nyamp_get_le64(payload + 16);
  memcpy(info->sha256, payload + 24, NYAMP_BLOB_SHA256_SIZE);

  /* Zero is the "no blob" value on both sides, so it can never be granted. */
  if (info->blob_id == 0 || info->flags != 0)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

int nyamp_blob_read_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size,
                           const struct nyamp_blob_read_s *read)
{
  size_t used = 0;
  int result;

  if (payload == NULL || payload_size == NULL || read == NULL ||
      read->blob_id == 0 || (read->flags & ~NYAMP_BLOB_READ_EOF) != 0)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_BLOB_READ_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  /* A window that is not in the shared region, or that has no room, cannot
   * carry file bytes; refusing it here keeps the responder's checks from
   * being the only line of defence.
   */
  if ((read->buffer.flags & NYAMP_BUFFER_IN_SHMEM) == 0 ||
      read->buffer.capacity == 0)
    {
      return NYAMP_EINVAL;
    }

  nyamp_put_le32(payload + 0, read->blob_id);
  nyamp_put_le32(payload + 4, read->flags);
  nyamp_put_le64(payload + 8, read->file_offset);
  result = nyamp_buffer_encode(payload + 16, payload_capacity - 16, &used,
                               &read->buffer);
  if (result != NYAMP_OK)
    {
      return result;
    }

  *payload_size = NYAMP_BLOB_READ_SIZE;
  return NYAMP_OK;
}

int nyamp_blob_read_decode(struct nyamp_blob_read_s *read,
                           const uint8_t *payload, size_t payload_size)
{
  int result;

  if (read == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_READ_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_BLOB_READ_SIZE)
    {
      return NYAMP_EPROTO;
    }

  read->blob_id = nyamp_get_le32(payload + 0);
  read->flags = nyamp_get_le32(payload + 4);
  read->file_offset = nyamp_get_le64(payload + 8);
  result = nyamp_buffer_decode(&read->buffer, payload + 16, NYAMP_BUFFER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (read->blob_id == 0 || (read->flags & ~NYAMP_BLOB_READ_EOF) != 0 ||
      (read->buffer.flags & NYAMP_BUFFER_IN_SHMEM) == 0 ||
      read->buffer.capacity == 0)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

int nyamp_blob_close_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, uint32_t blob_id)
{
  if (payload == NULL || payload_size == NULL || blob_id == 0)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_BLOB_CLOSE_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, blob_id);
  nyamp_put_le32(payload + 4, 0);
  *payload_size = NYAMP_BLOB_CLOSE_SIZE;
  return NYAMP_OK;
}

int nyamp_blob_close_decode(uint32_t *blob_id, const uint8_t *payload,
                            size_t payload_size)
{
  int result;

  if (blob_id == NULL)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_CLOSE_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_BLOB_CLOSE_SIZE ||
      nyamp_get_le32(payload + 0) == 0 || nyamp_get_le32(payload + 4) != 0)
    {
      return NYAMP_EPROTO;
    }

  *blob_id = nyamp_get_le32(payload + 0);
  return NYAMP_OK;
}

int nyamp_blob_list_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t cursor,
                           const char *prefix, size_t prefix_length)
{
  /* An empty prefix names the blob root; anything else obeys the name rules.
   */
  if (prefix_length != 0 &&
      nyamp_blob_name_check(prefix, prefix_length) != NYAMP_OK)
    {
      return NYAMP_EINVAL;
    }

  return nyamp_blob_text_encode(payload, payload_capacity, payload_size,
                                cursor, prefix, prefix_length);
}

int nyamp_blob_list_decode(uint32_t *cursor, const char **prefix,
                           size_t *prefix_length, const uint8_t *payload,
                           size_t payload_size)
{
  int result = nyamp_blob_text_decode(cursor, prefix, prefix_length, payload,
                                      payload_size);

  if (result != NYAMP_OK)
    {
      return result;
    }

  if (*prefix_length != 0 &&
      nyamp_blob_name_check(*prefix, *prefix_length) != NYAMP_OK)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

int nyamp_blob_list_body_begin(uint8_t *body, size_t body_capacity,
                               size_t *body_size)
{
  if (body == NULL || body_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (body_capacity < NYAMP_BLOB_LIST_BODY_HEADER_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  memset(body, 0, NYAMP_BLOB_LIST_BODY_HEADER_SIZE);
  *body_size = NYAMP_BLOB_LIST_BODY_HEADER_SIZE;
  return NYAMP_OK;
}

int nyamp_blob_list_body_append(uint8_t *body, size_t body_capacity,
                                size_t *body_size,
                                const struct nyamp_blob_entry_s *entry)
{
  size_t needed;
  uint8_t *dest;

  if (body == NULL || body_size == NULL || entry == NULL ||
      *body_size < NYAMP_BLOB_LIST_BODY_HEADER_SIZE ||
      *body_size > body_capacity ||
      (entry->flags & ~NYAMP_BLOB_ENTRY_DIRECTORY) != 0)
    {
      return NYAMP_EINVAL;
    }

  /* Entries carry one path component, which the name rules already cover. */
  if (nyamp_blob_name_check(entry->name, entry->name_length) != NYAMP_OK ||
      memchr(entry->name, '/', entry->name_length) != NULL)
    {
      return NYAMP_EINVAL;
    }

  needed = NYAMP_BLOB_LIST_ENTRY_HEADER_SIZE + entry->name_length;
  if (body_capacity - *body_size < needed)
    {
      return NYAMP_EMSGSIZE;
    }

  dest = body + *body_size;
  nyamp_put_le64(dest + 0, entry->size);
  nyamp_put_le16(dest + 8, entry->flags);
  nyamp_put_le16(dest + 10, entry->name_length);
  memcpy(dest + NYAMP_BLOB_LIST_ENTRY_HEADER_SIZE, entry->name,
         entry->name_length);

  nyamp_put_le32(body + 4, nyamp_get_le32(body + 4) + 1);
  *body_size += needed;
  return NYAMP_OK;
}

int nyamp_blob_list_body_finish(uint8_t *body, size_t body_size,
                                uint32_t next_cursor)
{
  if (body == NULL || body_size < NYAMP_BLOB_LIST_BODY_HEADER_SIZE)
    {
      return NYAMP_EINVAL;
    }

  nyamp_put_le32(body + 0, next_cursor);
  return NYAMP_OK;
}

int nyamp_blob_list_body_next(struct nyamp_blob_entry_s *entry,
                              size_t *position, const uint8_t *body,
                              size_t body_size)
{
  const uint8_t *source;
  size_t offset;

  if (entry == NULL || position == NULL || body == NULL)
    {
      return NYAMP_EINVAL;
    }

  offset = *position == 0 ? NYAMP_BLOB_LIST_BODY_HEADER_SIZE : *position;
  if (offset > body_size ||
      body_size - offset < NYAMP_BLOB_LIST_ENTRY_HEADER_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  source = body + offset;
  entry->size = nyamp_get_le64(source + 0);
  entry->flags = nyamp_get_le16(source + 8);
  entry->name_length = nyamp_get_le16(source + 10);
  entry->name = (const char *)(source + NYAMP_BLOB_LIST_ENTRY_HEADER_SIZE);

  if (body_size - offset - NYAMP_BLOB_LIST_ENTRY_HEADER_SIZE <
      entry->name_length)
    {
      return NYAMP_EMSGSIZE;
    }

  if ((entry->flags & ~NYAMP_BLOB_ENTRY_DIRECTORY) != 0 ||
      nyamp_blob_name_check(entry->name, entry->name_length) != NYAMP_OK ||
      memchr(entry->name, '/', entry->name_length) != NULL)
    {
      return NYAMP_EPROTO;
    }

  *position = offset + NYAMP_BLOB_LIST_ENTRY_HEADER_SIZE + entry->name_length;
  return NYAMP_OK;
}

int nyamp_blob_list_body_decode(uint32_t *next_cursor, uint32_t *count,
                                const uint8_t *body, size_t body_size)
{
  struct nyamp_blob_entry_s entry;
  size_t position = 0;
  uint32_t index;
  int result;

  if (next_cursor == NULL || count == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(body, body_size, NYAMP_BLOB_LIST_BODY_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  *next_cursor = nyamp_get_le32(body + 0);
  *count = nyamp_get_le32(body + 4);

  /* Walk every entry once so the caller's loop cannot meet a malformed one,
   * and so trailing bytes the count does not account for are refused.
   */
  for (index = 0; index < *count; index++)
    {
      result = nyamp_blob_list_body_next(&entry, &position, body, body_size);
      if (result != NYAMP_OK)
        {
          return NYAMP_EPROTO;
        }
    }

  if ((*count == 0 ? NYAMP_BLOB_LIST_BODY_HEADER_SIZE : position) != body_size)
    {
      return NYAMP_EPROTO;
    }

  /* A page that makes no progress would loop the requester forever. */
  if (*count == 0 && *next_cursor != 0)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

int nyamp_blob_bench_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size,
                            const struct nyamp_blob_bench_s *bench)
{
  size_t used = 0;
  int result;

  if (payload == NULL || payload_size == NULL || bench == NULL ||
      bench->mode > NYAMP_BLOB_BENCH_FILL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < (bench->mode == NYAMP_BLOB_BENCH_FILL
                              ? NYAMP_BLOB_BENCH_FILL_SIZE
                              : NYAMP_BLOB_BENCH_ECHO_SIZE))
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, bench->mode);
  nyamp_put_le32(payload + 4, bench->seed);
  *payload_size = NYAMP_BLOB_BENCH_ECHO_SIZE;
  if (bench->mode == NYAMP_BLOB_BENCH_FILL)
    {
      if ((bench->buffer.flags & NYAMP_BUFFER_IN_SHMEM) == 0 ||
          bench->buffer.capacity == 0)
        {
          return NYAMP_EINVAL;
        }

      result = nyamp_buffer_encode(payload + 8, payload_capacity - 8, &used,
                                   &bench->buffer);
      if (result != NYAMP_OK)
        {
          return result;
        }

      *payload_size = NYAMP_BLOB_BENCH_FILL_SIZE;
    }

  return NYAMP_OK;
}

int nyamp_blob_bench_decode(struct nyamp_blob_bench_s *bench,
                            const uint8_t *payload, size_t payload_size)
{
  int result;

  if (bench == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_BENCH_ECHO_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  memset(bench, 0, sizeof(*bench));
  bench->mode = nyamp_get_le32(payload + 0);
  bench->seed = nyamp_get_le32(payload + 4);

  if (bench->mode == NYAMP_BLOB_BENCH_ECHO)
    {
      return payload_size == NYAMP_BLOB_BENCH_ECHO_SIZE ? NYAMP_OK
                                                        : NYAMP_EPROTO;
    }

  if (bench->mode != NYAMP_BLOB_BENCH_FILL ||
      payload_size != NYAMP_BLOB_BENCH_FILL_SIZE)
    {
      return NYAMP_EPROTO;
    }

  result = nyamp_buffer_decode(&bench->buffer, payload + 8, NYAMP_BUFFER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if ((bench->buffer.flags & NYAMP_BUFFER_IN_SHMEM) == 0 ||
      bench->buffer.capacity == 0)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

void nyamp_blob_bench_pattern(uint8_t *dest, size_t words, uint32_t seed,
                              uint32_t first_word)
{
  size_t index;

  if (dest == NULL)
    {
      return;
    }

  for (index = 0; index < words; index++)
    {
      nyamp_put_le32(dest + index * 4U,
                     seed ^ ((first_word + (uint32_t)index) * 0x9e3779b1U));
    }
}

int nyamp_blob_bench_run_encode(uint8_t *payload, size_t payload_capacity,
                                size_t *payload_size, uint32_t rounds,
                                uint32_t window_bytes)
{
  if (payload == NULL || payload_size == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_BLOB_BENCH_RUN_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, rounds);
  nyamp_put_le32(payload + 4, window_bytes);
  *payload_size = NYAMP_BLOB_BENCH_RUN_SIZE;
  return NYAMP_OK;
}

int nyamp_blob_bench_run_decode(uint32_t *rounds, uint32_t *window_bytes,
                                const uint8_t *payload, size_t payload_size)
{
  int result;

  if (rounds == NULL || window_bytes == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_BENCH_RUN_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_BLOB_BENCH_RUN_SIZE)
    {
      return NYAMP_EPROTO;
    }

  *rounds = nyamp_get_le32(payload + 0);
  *window_bytes = nyamp_get_le32(payload + 4);
  return NYAMP_OK;
}

int nyamp_blob_bench_report_encode(
    uint8_t *payload, size_t payload_capacity, size_t *payload_size,
    const struct nyamp_blob_bench_report_s *report)
{
  if (payload == NULL || payload_size == NULL || report == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_BLOB_BENCH_REPORT_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, report->rounds);
  nyamp_put_le32(payload + 4, report->window_bytes);
  nyamp_put_le32(payload + 8, report->rtt_min_us);
  nyamp_put_le32(payload + 12, report->rtt_avg_us);
  nyamp_put_le32(payload + 16, report->rtt_max_us);
  nyamp_put_le32(payload + 20, report->fill_kib_per_s);
  nyamp_put_le32(payload + 24, report->copy_kib_per_s);
  nyamp_put_le32(payload + 28, report->pattern_errors);
  *payload_size = NYAMP_BLOB_BENCH_REPORT_SIZE;
  return NYAMP_OK;
}

int nyamp_blob_bench_report_decode(struct nyamp_blob_bench_report_s *report,
                                   const uint8_t *payload,
                                   size_t payload_size)
{
  int result;

  if (report == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_BENCH_REPORT_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_BLOB_BENCH_REPORT_SIZE)
    {
      return NYAMP_EPROTO;
    }

  report->rounds = nyamp_get_le32(payload + 0);
  report->window_bytes = nyamp_get_le32(payload + 4);
  report->rtt_min_us = nyamp_get_le32(payload + 8);
  report->rtt_avg_us = nyamp_get_le32(payload + 12);
  report->rtt_max_us = nyamp_get_le32(payload + 16);
  report->fill_kib_per_s = nyamp_get_le32(payload + 20);
  report->copy_kib_per_s = nyamp_get_le32(payload + 24);
  report->pattern_errors = nyamp_get_le32(payload + 28);
  return NYAMP_OK;
}

int nyamp_blob_pull_report_encode(
    uint8_t *payload, size_t payload_capacity, size_t *payload_size,
    const struct nyamp_blob_pull_report_s *report)
{
  if (payload == NULL || payload_size == NULL || report == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_BLOB_PULL_REPORT_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le64(payload + 0, report->bytes);
  nyamp_put_le64(payload + 8, report->elapsed_ms);
  nyamp_put_le32(payload + 16, report->files);
  nyamp_put_le32(payload + 20, report->reused);
  *payload_size = NYAMP_BLOB_PULL_REPORT_SIZE;
  return NYAMP_OK;
}

int nyamp_blob_pull_report_decode(struct nyamp_blob_pull_report_s *report,
                                  const uint8_t *payload, size_t payload_size)
{
  int result;

  if (report == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_PULL_REPORT_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_BLOB_PULL_REPORT_SIZE)
    {
      return NYAMP_EPROTO;
    }

  report->bytes = nyamp_get_le64(payload + 0);
  report->elapsed_ms = nyamp_get_le64(payload + 8);
  report->files = nyamp_get_le32(payload + 16);
  report->reused = nyamp_get_le32(payload + 20);
  return NYAMP_OK;
}

int nyamp_blob_progress_encode(uint8_t *payload, size_t payload_capacity,
                               size_t *payload_size,
                               const struct nyamp_blob_progress_s *progress)
{
  if (payload == NULL || payload_size == NULL || progress == NULL ||
      progress->done > progress->total)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_BLOB_PROGRESS_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le64(payload + 0, progress->done);
  nyamp_put_le64(payload + 8, progress->total);
  nyamp_put_le32(payload + 16, progress->bytes_per_second);
  nyamp_put_le32(payload + 20, 0);
  *payload_size = NYAMP_BLOB_PROGRESS_SIZE;
  return NYAMP_OK;
}

int nyamp_blob_progress_decode(struct nyamp_blob_progress_s *progress,
                               const uint8_t *payload, size_t payload_size)
{
  int result;

  if (progress == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_BLOB_PROGRESS_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_BLOB_PROGRESS_SIZE)
    {
      return NYAMP_EPROTO;
    }

  progress->done = nyamp_get_le64(payload + 0);
  progress->total = nyamp_get_le64(payload + 8);
  progress->bytes_per_second = nyamp_get_le32(payload + 16);
  if (progress->done > progress->total)
    {
      return NYAMP_EPROTO;
    }

  return NYAMP_OK;
}

/****************************************************************************
 * LLM CHAT
 *
 * A request body and a response body are both byte strings far past the
 * inline limit, carried as ordered chunks.  The two chunk layouts share one
 * shape check -- a chunk must lie inside the body it claims to belong to --
 * so a reassembler only has to verify continuity, never bounds.
 *
 ****************************************************************************/

static int nyamp_llm_span_check(uint32_t total, uint32_t offset,
                                uint32_t length, uint32_t max_total,
                                uint32_t max_length)
{
  if (total == 0 || total > max_total || length == 0 || length > max_length ||
      offset > total || length > total - offset)
    {
      return NYAMP_EINVAL;
    }

  return NYAMP_OK;
}

int nyamp_llm_chat_encode(uint8_t *payload, size_t payload_capacity,
                          size_t *payload_size,
                          const struct nyamp_llm_chat_s *chunk,
                          const uint8_t *bytes)
{
  size_t needed;

  if (payload == NULL || payload_size == NULL || chunk == NULL ||
      bytes == NULL)
    {
      return NYAMP_EINVAL;
    }

  if ((chunk->flags & ~NYAMP_LLM_CHAT_FLAGS_ALL) != 0 ||
      nyamp_llm_span_check(chunk->total, chunk->offset, chunk->length,
                           NYAMP_LLM_CHAT_MAX_BODY,
                           NYAMP_LLM_CHAT_MAX_CHUNK) != NYAMP_OK)
    {
      return NYAMP_EINVAL;
    }

  needed = NYAMP_LLM_CHAT_HEADER_SIZE + chunk->length;
  if (payload_capacity < needed)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, chunk->total);
  nyamp_put_le32(payload + 4, chunk->offset);
  nyamp_put_le32(payload + 8, chunk->length);
  nyamp_put_le32(payload + 12, chunk->max_new_tokens);
  nyamp_put_le32(payload + 16, chunk->flags);
  memcpy(payload + NYAMP_LLM_CHAT_HEADER_SIZE, bytes, chunk->length);
  *payload_size = needed;
  return NYAMP_OK;
}

int nyamp_llm_chat_decode(struct nyamp_llm_chat_s *chunk,
                          const uint8_t **bytes, const uint8_t *payload,
                          size_t payload_size)
{
  int result;

  if (chunk == NULL || bytes == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_LLM_CHAT_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  chunk->total = nyamp_get_le32(payload + 0);
  chunk->offset = nyamp_get_le32(payload + 4);
  chunk->length = nyamp_get_le32(payload + 8);
  chunk->max_new_tokens = nyamp_get_le32(payload + 12);
  chunk->flags = nyamp_get_le32(payload + 16);

  if ((chunk->flags & ~NYAMP_LLM_CHAT_FLAGS_ALL) != 0 ||
      nyamp_llm_span_check(chunk->total, chunk->offset, chunk->length,
                           NYAMP_LLM_CHAT_MAX_BODY,
                           NYAMP_LLM_CHAT_MAX_CHUNK) != NYAMP_OK)
    {
      return NYAMP_EPROTO;
    }

  if (payload_size != NYAMP_LLM_CHAT_HEADER_SIZE + (size_t)chunk->length)
    {
      return NYAMP_EMSGSIZE;
    }

  *bytes = payload + NYAMP_LLM_CHAT_HEADER_SIZE;
  return NYAMP_OK;
}

int nyamp_llm_result_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size,
                            const struct nyamp_llm_result_s *chunk,
                            const uint8_t *bytes)
{
  size_t needed;

  if (payload == NULL || payload_size == NULL || chunk == NULL ||
      bytes == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (nyamp_llm_span_check(chunk->total, chunk->offset, chunk->length,
                           NYAMP_LLM_RESULT_MAX_BODY,
                           NYAMP_LLM_RESULT_MAX_CHUNK) != NYAMP_OK)
    {
      return NYAMP_EINVAL;
    }

  needed = NYAMP_LLM_RESULT_HEADER_SIZE + chunk->length;
  if (payload_capacity < needed)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, chunk->total);
  nyamp_put_le32(payload + 4, chunk->offset);
  nyamp_put_le32(payload + 8, chunk->length);
  memcpy(payload + NYAMP_LLM_RESULT_HEADER_SIZE, bytes, chunk->length);
  *payload_size = needed;
  return NYAMP_OK;
}

int nyamp_llm_result_decode(struct nyamp_llm_result_s *chunk,
                            const uint8_t **bytes, const uint8_t *payload,
                            size_t payload_size)
{
  int result;

  if (chunk == NULL || bytes == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_LLM_RESULT_HEADER_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  chunk->total = nyamp_get_le32(payload + 0);
  chunk->offset = nyamp_get_le32(payload + 4);
  chunk->length = nyamp_get_le32(payload + 8);
  if (nyamp_llm_span_check(chunk->total, chunk->offset, chunk->length,
                           NYAMP_LLM_RESULT_MAX_BODY,
                           NYAMP_LLM_RESULT_MAX_CHUNK) != NYAMP_OK)
    {
      return NYAMP_EPROTO;
    }

  if (payload_size != NYAMP_LLM_RESULT_HEADER_SIZE + (size_t)chunk->length)
    {
      return NYAMP_EMSGSIZE;
    }

  *bytes = payload + NYAMP_LLM_RESULT_HEADER_SIZE;
  return NYAMP_OK;
}

int nyamp_llm_chat_finish_encode(uint8_t *payload, size_t payload_capacity,
                                 size_t *payload_size,
                                 const struct nyamp_llm_chat_finish_s *finish)
{
  if (payload == NULL || payload_size == NULL || finish == NULL)
    {
      return NYAMP_EINVAL;
    }

  if (payload_capacity < NYAMP_LLM_CHAT_FINISH_SIZE)
    {
      return NYAMP_EMSGSIZE;
    }

  nyamp_put_le32(payload + 0, (uint32_t)finish->status);
  nyamp_put_le32(payload + 4, finish->sequence);
  nyamp_put_le32(payload + 8, finish->prompt_tokens);
  nyamp_put_le32(payload + 12, finish->completion_tokens);
  nyamp_put_le32(payload + 16, finish->prefill_ms);
  nyamp_put_le32(payload + 20, finish->decode_ms);
  nyamp_put_le32(payload + 24, finish->context_limit);
  *payload_size = NYAMP_LLM_CHAT_FINISH_SIZE;
  return NYAMP_OK;
}

int nyamp_llm_chat_finish_decode(struct nyamp_llm_chat_finish_s *finish,
                                 const uint8_t *payload, size_t payload_size)
{
  int result;

  if (finish == NULL)
    {
      return NYAMP_EINVAL;
    }

  result =
      nyamp_payload_ready(payload, payload_size, NYAMP_LLM_CHAT_FINISH_SIZE);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (payload_size != NYAMP_LLM_CHAT_FINISH_SIZE)
    {
      return NYAMP_EPROTO;
    }

  finish->status = (int32_t)nyamp_get_le32(payload + 0);
  finish->sequence = nyamp_get_le32(payload + 4);
  finish->prompt_tokens = nyamp_get_le32(payload + 8);
  finish->completion_tokens = nyamp_get_le32(payload + 12);
  finish->prefill_ms = nyamp_get_le32(payload + 16);
  finish->decode_ms = nyamp_get_le32(payload + 20);
  finish->context_limit = nyamp_get_le32(payload + 24);
  return NYAMP_OK;
}
