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

static int test_buffer_descriptor(void)
{
  uint8_t payload[NYAMP_BUFFER_SIZE];
  size_t size = 0;
  struct nyamp_buffer_s in = {
    .magic = NYAMP_BUFFER_MAGIC,
    .version = NYAMP_BUFFER_VERSION,
    .flags = NYAMP_BUFFER_IN_SHMEM,
    .offset = 0x1000,
    .length = 64000,
    .capacity = 65536,
    .format = NYAMP_FORMAT_F32,
    .lease = 0x1122334455667788ULL,
    .generation = 42,
    .reserved = 0,
  };
  struct nyamp_buffer_s out;

  CHECK(nyamp_buffer_encode(payload, sizeof(payload), &size, &in) == NYAMP_OK);
  CHECK(size == NYAMP_BUFFER_SIZE);
  CHECK(nyamp_buffer_decode(&out, payload, size) == NYAMP_OK);
  CHECK(out.offset == 0x1000 && out.length == 64000 && out.capacity == 65536);
  CHECK(out.lease == 0x1122334455667788ULL && out.generation == 42);
  CHECK(out.format == NYAMP_FORMAT_F32 && out.flags == NYAMP_BUFFER_IN_SHMEM);

  /* The descriptor must be self-contained: a stored copy has to validate
   * without any surrounding message context.
   */
  payload[0] ^= 0xff;
  CHECK(nyamp_buffer_decode(&out, payload, size) == NYAMP_EPROTO);
  payload[0] ^= 0xff;

  payload[4] = 0xff;
  CHECK(nyamp_buffer_decode(&out, payload, size) == NYAMP_EPROTO);
  payload[4] = NYAMP_BUFFER_VERSION;

  /* More valid bytes than the grant holds is impossible by construction. */
  in.length = in.capacity + 1;
  CHECK(nyamp_buffer_encode(payload, sizeof(payload), &size, &in) ==
        NYAMP_EINVAL);
  in.length = 64000;

  /* An undefined flag bit means the two sides disagree on the format. */
  in.flags = 0x8000;
  CHECK(nyamp_buffer_encode(payload, sizeof(payload), &size, &in) ==
        NYAMP_EINVAL);
  in.flags = NYAMP_BUFFER_IN_SHMEM;

  /* A short frame is refused rather than read past. */
  CHECK(nyamp_buffer_decode(&out, payload, NYAMP_BUFFER_SIZE - 1) ==
        NYAMP_EMSGSIZE);
  return 0;
}

static int test_asr_messages(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  uint32_t sample_rate = 0, max_samples = 0, sequence = 0;
  uint32_t total_samples = 0, consumed = 0;
  uint16_t channels = 0, flags = 0;
  const char *text = NULL;
  size_t text_length = 0;
  struct nyamp_buffer_s buffer = {
    .magic = NYAMP_BUFFER_MAGIC,
    .version = NYAMP_BUFFER_VERSION,
    .flags = NYAMP_BUFFER_IN_SHMEM,
    .offset = 0x1000,
    .length = 64000,
    .capacity = 65536,
    .format = NYAMP_FORMAT_F32,
    .lease = 7,
    .generation = 3,
  };
  struct nyamp_buffer_s decoded;

  CHECK(nyamp_asr_begin_encode(payload, sizeof(payload), &size, 16000, 1, 0,
                               480000) == NYAMP_OK);
  CHECK(size == NYAMP_ASR_BEGIN_SIZE);
  CHECK(nyamp_asr_begin_decode(&sample_rate, &channels, &flags, &max_samples,
                               payload, size) == NYAMP_OK);
  CHECK(sample_rate == 16000 && channels == 1 && max_samples == 480000);

  CHECK(nyamp_asr_push_encode(payload, sizeof(payload), &size, &buffer, 5, 0,
                              40000, 40000) == NYAMP_OK);
  CHECK(size == NYAMP_ASR_PUSH_HEADER_SIZE);
  CHECK(nyamp_asr_push_decode(&decoded, &sequence, &flags, &total_samples,
                              &consumed, payload, size) == NYAMP_OK);
  CHECK(decoded.lease == 7 && sequence == 5 && total_samples == 40000);
  CHECK(consumed == 40000 && decoded.length == 64000);

  /* Text travels as a delta with its own length, so it needs no NUL. */
  CHECK(nyamp_asr_partial_encode(payload, sizeof(payload), &size, 9, 40000,
                                 NYAMP_BUFFER_RESYNC, "world", 5) == NYAMP_OK);
  CHECK(size == NYAMP_ASR_PARTIAL_HEADER_SIZE + 5);
  CHECK(nyamp_asr_partial_decode(&sequence, &consumed, &flags, &text,
                                 &text_length, payload, size) == NYAMP_OK);
  CHECK(sequence == 9 && consumed == 40000 && text_length == 5);
  CHECK(memcmp(text, "world", 5) == 0);
  CHECK((flags & NYAMP_BUFFER_RESYNC) != 0);

  /* Empty text is legal: a window may produce no new words. */
  CHECK(nyamp_asr_partial_encode(payload, sizeof(payload), &size, 10, 40000, 0,
                                 NULL, 0) == NYAMP_OK);
  CHECK(size == NYAMP_ASR_PARTIAL_HEADER_SIZE);
  CHECK(nyamp_asr_partial_decode(&sequence, &consumed, &flags, &text,
                                 &text_length, payload, size) == NYAMP_OK);
  CHECK(text_length == 0);

  /* The largest legal delta must fit the inline payload. */
  {
    static char big[NYAMP_ASR_MAX_TEXT + 1];
    CHECK(nyamp_asr_partial_encode(payload, sizeof(payload), &size, 11, 1, 0,
                                   big, NYAMP_ASR_MAX_TEXT) == NYAMP_OK);
    CHECK(size == NYAMP_INLINE_MAX);
    CHECK(nyamp_asr_partial_encode(payload, sizeof(payload), &size, 11, 1, 0,
                                   big,
                                   NYAMP_ASR_MAX_TEXT + 1) == NYAMP_EMSGSIZE);
  }

  CHECK(nyamp_asr_finish_encode(payload, sizeof(payload), &size,
                                NYAMP_MODEL_CANCELLED, 12) == NYAMP_OK);
  {
    int32_t asr_status = 0;
    CHECK(nyamp_asr_finish_decode(&asr_status, &sequence, payload, size) ==
          NYAMP_OK);
    CHECK(asr_status == NYAMP_MODEL_CANCELLED && sequence == 12);
  }
  return 0;
}

static int test_tts_messages(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  uint32_t phoneme_count = 0, speaker_id = 0, bucket_frames = 0;
  uint32_t sequence = 0, sample_rate = 0, channels = 0, valid_samples = 0;
  uint32_t total_samples = 0;
  float speed = 0.0f;
  struct nyamp_buffer_s buffer = {
    .magic = NYAMP_BUFFER_MAGIC,
    .version = NYAMP_BUFFER_VERSION,
    .flags = NYAMP_BUFFER_IN_SHMEM | NYAMP_BUFFER_FROM_COMPUTE,
    .offset = 0x101000,
    .length = 700416, /* 342 frames, not the full 512-frame buffer. */
    .capacity = 1048576,
    .format = NYAMP_FORMAT_F32,
    .lease = 11,
    .generation = 3,
  };
  struct nyamp_buffer_s decoded;

  CHECK(nyamp_tts_synth_encode(payload, sizeof(payload), &size, 95, 1, 1.0f,
                               512) == NYAMP_OK);
  CHECK(size == NYAMP_TTS_SYNTH_SIZE);
  CHECK(nyamp_tts_synth_decode(&phoneme_count, &speaker_id, &speed,
                               &bucket_frames, payload, size) == NYAMP_OK);
  CHECK(phoneme_count == 95 && speaker_id == 1 && bucket_frames == 512);
  CHECK(speed == 1.0f);

  /* The float survives the round trip through its bit pattern. */
  CHECK(nyamp_tts_synth_encode(payload, sizeof(payload), &size, 4, 2, 0.5f,
                               512) == NYAMP_OK);
  CHECK(nyamp_tts_synth_decode(&phoneme_count, &speaker_id, &speed,
                               &bucket_frames, payload, size) == NYAMP_OK);
  CHECK(speed == 0.5f);

  CHECK(nyamp_tts_pcm_encode(payload, sizeof(payload), &size, &buffer, 3,
                             44100, 1, 175104) == NYAMP_OK);
  CHECK(size == NYAMP_TTS_PCM_HEADER_SIZE);
  CHECK(size <= NYAMP_INLINE_MAX);
  CHECK(nyamp_tts_pcm_decode(&decoded, &sequence, &sample_rate, &channels,
                             &valid_samples, payload, size) == NYAMP_OK);
  CHECK(sequence == 3 && sample_rate == 44100 && channels == 1);
  CHECK(valid_samples == 175104);

  /* Valid samples and granted bytes are different quantities: a decoder that
   * conflated them would report roughly 1.5x the real audio.
   */
  CHECK(decoded.length == 700416 && decoded.capacity == 1048576);

  CHECK(nyamp_tts_finish_encode(payload, sizeof(payload), &size,
                                NYAMP_MODEL_OK, 4, 175104) == NYAMP_OK);
  CHECK(size == NYAMP_TTS_FINISH_SIZE);
  {
    int32_t tts_status = 0;
    CHECK(nyamp_tts_finish_decode(&tts_status, &sequence, &total_samples,
                                  payload, size) == NYAMP_OK);
    CHECK(tts_status == NYAMP_MODEL_OK && sequence == 4 &&
          total_samples == 175104);
  }
  return 0;
}

int main(void)
{
  if (test_round_trip() != 0 || test_rejections() != 0 ||
      test_malformed_inputs() != 0 || test_llm_chunk() != 0 ||
      test_llm_token() != 0 || test_llm_finish() != 0 ||
      test_cancel_has_no_payload() != 0 || test_buffer_descriptor() != 0 ||
      test_asr_messages() != 0 || test_tts_messages() != 0)
    {
      return 1;
    }
  puts("nyamp protocol tests passed");
  return 0;
}
