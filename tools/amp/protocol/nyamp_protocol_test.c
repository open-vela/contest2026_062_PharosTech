/****************************************************************************
 * tools/amp/protocol/nyamp_protocol_test.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "nyamp_protocol.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                            \
  do                                                                 \
    {                                                                \
      if (!(expression))                                             \
        {                                                            \
          fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
                  #expression);                                      \
          return 1;                                                  \
        }                                                            \
    }                                                                \
  while (0)

static int test_round_trip(void)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  struct nyamp_header_s input = {
    .service = NYAMP_SERVICE_NPU,
    .opcode = 3,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = 0x1122334455667788ULL,
    .deadline_ms = 1234567,
    .generation = 9,
    .payload_size = 32,
  };
  struct nyamp_header_s output;

  memset(wire, 0xa5, sizeof(wire));
  CHECK(nyamp_header_encode(wire, sizeof(wire), &input) == NYAMP_OK);
  CHECK(nyamp_header_decode(&output, wire,
                            NYAMP_WIRE_HEADER_SIZE + input.payload_size) ==
        NYAMP_OK);
  CHECK(memcmp(&input, &output, sizeof(input)) == 0);
  return 0;
}

static int test_rejections(void)
{
  uint8_t wire[NYAMP_RPMSG_MTU] = { 0 };
  struct nyamp_header_s header = {
    .service = NYAMP_SERVICE_HEALTH,
    .opcode = 1,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = 1,
    .deadline_ms = 100,
    .generation = 1,
    .payload_size = 0,
  };
  struct nyamp_header_s decoded;

  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  CHECK(nyamp_header_decode(&decoded, wire, NYAMP_WIRE_HEADER_SIZE - 1) ==
        NYAMP_EMSGSIZE);

  wire[0] ^= 1;
  CHECK(nyamp_header_decode(&decoded, wire, sizeof(wire)) == NYAMP_EPROTO);
  wire[0] ^= 1;

  header.flags = NYAMP_FLAG_REQUEST | NYAMP_FLAG_RESPONSE;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EPROTO);

  header.flags = NYAMP_FLAG_EVENT | NYAMP_FLAG_ERROR;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EPROTO);

  header.flags = NYAMP_FLAG_CANCEL;
  header.payload_size = 1;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EPROTO);

  header.flags = NYAMP_FLAG_REQUEST;
  header.payload_size = NYAMP_INLINE_MAX + 1;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EMSGSIZE);
  return 0;
}

static int test_malformed_inputs(void)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  struct nyamp_header_s decoded;
  uint32_t state = 0x9e3779b9U;
  unsigned int iteration;
  unsigned int index;

  for (iteration = 0; iteration < 10000; iteration++)
    {
      for (index = 0; index < sizeof(wire); index++)
        {
          state ^= state << 13;
          state ^= state >> 17;
          state ^= state << 5;
          wire[index] = (uint8_t)state;
        }

      CHECK(nyamp_header_decode(&decoded, wire, sizeof(wire)) != NYAMP_OK);
    }

  return 0;
}

static int test_llm_chunk(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  struct nyamp_llm_chunk_s chunk = {
    .total = 300, .offset = 110, .count = 110, .max_new_tokens = 64
  };
  int32_t ids[NYAMP_LLM_MAX_CHUNK_IDS];
  int32_t decoded[NYAMP_LLM_MAX_CHUNK_IDS];
  struct nyamp_llm_chunk_s out;
  size_t count = 0;
  size_t index;

  for (index = 0; index < 110; index++)
    {
      ids[index] = (int32_t)(1000 + index);
    }

  CHECK(nyamp_llm_chunk_encode(payload, sizeof(payload), &size, &chunk, ids) ==
        NYAMP_OK);
  CHECK(size == NYAMP_LLM_CHUNK_HEADER_SIZE + 110 * 4);
  CHECK(nyamp_llm_chunk_decode(&out, decoded, NYAMP_LLM_MAX_CHUNK_IDS, &count,
                               payload, size) == NYAMP_OK);
  CHECK(count == 110 && out.total == 300 && out.offset == 110 &&
        out.max_new_tokens == 64 && decoded[0] == 1000 &&
        decoded[109] == 1109);

  /* A negative id can never come from the encoder. */
  decoded[0] = 0;
  payload[NYAMP_LLM_CHUNK_HEADER_SIZE + 3] = 0x80;
  CHECK(nyamp_llm_chunk_decode(&out, decoded, NYAMP_LLM_MAX_CHUNK_IDS, &count,
                               payload, size) == NYAMP_EPROTO);
  payload[NYAMP_LLM_CHUNK_HEADER_SIZE + 3] = 0;

  /* Shape violations are rejected rather than silently truncated. */
  chunk.count = 0;
  CHECK(nyamp_llm_chunk_encode(payload, sizeof(payload), &size, &chunk, ids) ==
        NYAMP_EINVAL);
  chunk.count = 111;
  CHECK(nyamp_llm_chunk_encode(payload, sizeof(payload), &size, &chunk, ids) ==
        NYAMP_EINVAL);
  chunk.offset = 300;
  chunk.count = 1;
  CHECK(nyamp_llm_chunk_encode(payload, sizeof(payload), &size, &chunk, ids) ==
        NYAMP_EINVAL);

  /* One byte short of the encoded length must be refused. */
  chunk.offset = 0;
  chunk.count = (uint32_t)NYAMP_LLM_MAX_CHUNK_IDS;
  CHECK(nyamp_llm_chunk_encode(payload, NYAMP_INLINE_MAX - 1, &size, &chunk,
                               ids) == NYAMP_EMSGSIZE);
  CHECK(nyamp_llm_chunk_encode(payload, NYAMP_INLINE_MAX, &size, &chunk,
                               ids) == NYAMP_OK);
  return 0;
}

static int test_llm_token(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  uint32_t token_id = 0, sequence = 0;
  const char *text = NULL;
  size_t text_length = 0;
  const char body[] = "hello";

  CHECK(nyamp_llm_token_encode(payload, sizeof(payload), &size, 42, 7, body,
                               sizeof(body) - 1) == NYAMP_OK);
  CHECK(size == NYAMP_LLM_TOKEN_HEADER_SIZE + 5);
  CHECK(payload[NYAMP_LLM_TOKEN_HEADER_SIZE] == 'h');
  CHECK(nyamp_llm_token_decode(&token_id, &sequence, &text, &text_length,
                               payload, size) == NYAMP_OK);
  CHECK(token_id == 42 && sequence == 7 && text_length == 5 &&
        memcmp(text, "hello", 5) == 0);

  /* Empty text is legal; a length that does not match the frame is not. */
  CHECK(nyamp_llm_token_encode(payload, sizeof(payload), &size, 1, 0, NULL,
                               0) == NYAMP_OK);
  CHECK(size == NYAMP_LLM_TOKEN_HEADER_SIZE);
  CHECK(nyamp_llm_token_decode(&token_id, &sequence, &text, &text_length,
                               payload, size) == NYAMP_OK);
  CHECK(text_length == 0);

  /* A frame longer than its declared text length is a protocol error. */
  payload[NYAMP_LLM_TOKEN_HEADER_SIZE] = 'x';
  CHECK(nyamp_llm_token_decode(&token_id, &sequence, &text, &text_length,
                               payload, size + 1) == NYAMP_EPROTO);

  /* A frame shorter than the fixed header cannot be decoded at all. */
  CHECK(nyamp_llm_token_decode(&token_id, &sequence, &text, &text_length,
                               payload, NYAMP_LLM_TOKEN_HEADER_SIZE - 1) ==
        NYAMP_EMSGSIZE);
  return 0;
}

static int test_llm_finish(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  int32_t status = 0;
  uint32_t sequence = 0;

  CHECK(nyamp_llm_finish_encode(payload, sizeof(payload), &size,
                                NYAMP_MODEL_CANCELLED, 3) == NYAMP_OK);
  CHECK(size == NYAMP_LLM_FINISH_SIZE);
  CHECK(nyamp_llm_finish_decode(&status, &sequence, payload, size) ==
        NYAMP_OK);
  CHECK(status == NYAMP_MODEL_CANCELLED && sequence == 3);
  CHECK(nyamp_llm_finish_decode(&status, &sequence, payload, size - 1) ==
        NYAMP_EMSGSIZE);
  return 0;
}

static int test_cancel_has_no_payload(void)
{
  uint8_t wire[NYAMP_RPMSG_MTU] = { 0 };
  struct nyamp_header_s header = {
    .service = NYAMP_SERVICE_LLM,
    .opcode = 4,
    .flags = NYAMP_FLAG_CANCEL,
    .request_id = 99,
    .deadline_ms = 0,
    .generation = 1,
    .payload_size = 1,
  };
  struct nyamp_header_s decoded;

  /* The existing rule that a cancel carries no payload is what lets the
   * target request_id travel in the header's own request_id field.
   */
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EPROTO);
  header.payload_size = 0;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  CHECK(nyamp_header_decode(&decoded, wire, NYAMP_WIRE_HEADER_SIZE) ==
        NYAMP_OK);
  CHECK(decoded.request_id == 99);
  return 0;
}

int main(void)
{
  if (test_round_trip() != 0 || test_rejections() != 0 ||
      test_malformed_inputs() != 0 || test_llm_chunk() != 0 ||
      test_llm_token() != 0 || test_llm_finish() != 0 ||
      test_cancel_has_no_payload() != 0)
    {
      return 1;
    }
  puts("nyamp protocol tests passed");
  return 0;
}
