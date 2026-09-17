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
