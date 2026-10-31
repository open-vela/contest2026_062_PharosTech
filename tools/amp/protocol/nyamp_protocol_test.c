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
                                 NYAMP_ASR_PARTIAL_RESYNC, "world",
                                 5) == NYAMP_OK);
  CHECK(size == NYAMP_ASR_PARTIAL_HEADER_SIZE + 5);
  CHECK(nyamp_asr_partial_decode(&sequence, &consumed, &flags, &text,
                                 &text_length, payload, size) == NYAMP_OK);
  CHECK(sequence == 9 && consumed == 40000 && text_length == 5);
  CHECK(memcmp(text, "world", 5) == 0);
  CHECK(flags == NYAMP_ASR_PARTIAL_RESYNC);

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

static int test_status_prefix(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  uint8_t *body = NULL;
  const uint8_t *decoded_body = NULL;
  size_t capacity = 0;
  size_t size = 0;
  int32_t status = 0;

  CHECK(nyamp_status_encode(payload, sizeof(payload),
                            NYAMP_MODEL_STALE_GENERATION, &body,
                            &capacity) == NYAMP_OK);
  CHECK(body == payload + NYAMP_STATUS_SIZE);
  CHECK(capacity == sizeof(payload) - NYAMP_STATUS_SIZE);
  CHECK(capacity == NYAMP_BLOB_LIST_BODY_MAX);

  CHECK(nyamp_status_decode(&status, &decoded_body, &size, payload,
                            NYAMP_STATUS_SIZE + 7) == NYAMP_OK);
  CHECK(status == NYAMP_MODEL_STALE_GENERATION && size == 7 &&
        decoded_body == payload + NYAMP_STATUS_SIZE);

  /* A response too short to hold a status is not a success. */
  CHECK(nyamp_status_decode(&status, &decoded_body, &size, payload, 3) ==
        NYAMP_EMSGSIZE);
  CHECK(nyamp_status_encode(payload, 3, 0, &body, &capacity) ==
        NYAMP_EMSGSIZE);
  return 0;
}

static int test_blob_names(void)
{
  static const char *const good[] = {
    "llm/model.rkllm", "asr", "asr/ENC.ONX", "a/b/c/d.bin", "tts/zh_en v2.onnx",
    "..a", "a..", "...",
  };
  static const char *const bad[] = {
    "",       "/abs",      "a//b",   "a/",   ".",         "..",
    "../x",   "a/../b",    "a/./b",  "a/..", "a\\b",      "a/\x01" "b",
    "a\x7f",
  };
  char longest[NYAMP_BLOB_MAX_NAME + 2];
  size_t index;

  for (index = 0; index < sizeof(good) / sizeof(good[0]); index++)
    {
      CHECK(nyamp_blob_name_check(good[index], strlen(good[index])) ==
            NYAMP_OK);
    }

  for (index = 0; index < sizeof(bad) / sizeof(bad[0]); index++)
    {
      CHECK(nyamp_blob_name_check(bad[index], strlen(bad[index])) ==
            NYAMP_EINVAL);
    }

  /* An embedded NUL would truncate the path the responder builds, turning
   * "ok\0/../../etc" into something the component check never saw.
   */
  CHECK(nyamp_blob_name_check("ok\0/x", 5) == NYAMP_EINVAL);
  CHECK(nyamp_blob_name_check(NULL, 3) == NYAMP_EINVAL);

  memset(longest, 'a', sizeof(longest));
  CHECK(nyamp_blob_name_check(longest, NYAMP_BLOB_MAX_NAME) == NYAMP_OK);
  CHECK(nyamp_blob_name_check(longest, NYAMP_BLOB_MAX_NAME + 1) ==
        NYAMP_EINVAL);
  return 0;
}

static int test_blob_open(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  uint32_t flags = 1;
  const char *name = NULL;
  size_t name_length = 0;
  struct nyamp_blob_info_s info;
  struct nyamp_blob_info_s decoded;
  unsigned int index;

  CHECK(nyamp_blob_open_encode(payload, sizeof(payload), &size, 0,
                               "llm/model.rkllm", 15) == NYAMP_OK);
  CHECK(size == NYAMP_BLOB_OPEN_HEADER_SIZE + 15);
  CHECK(nyamp_blob_open_decode(&flags, &name, &name_length, payload, size) ==
        NYAMP_OK);
  CHECK(flags == 0 && name_length == 15 &&
        memcmp(name, "llm/model.rkllm", 15) == 0);

  /* The requester may not even send a traversal, and no flag is defined. */
  CHECK(nyamp_blob_open_encode(payload, sizeof(payload), &size, 0, "../etc",
                               6) == NYAMP_EINVAL);
  CHECK(nyamp_blob_open_encode(payload, sizeof(payload), &size, 1, "asr", 3) ==
        NYAMP_EINVAL);

  /* A responder must refuse one that was forged past the encoder. */
  CHECK(nyamp_blob_open_encode(payload, sizeof(payload), &size, 0, "aa/bb",
                               5) == NYAMP_OK);
  payload[NYAMP_BLOB_OPEN_HEADER_SIZE + 0] = '.';
  payload[NYAMP_BLOB_OPEN_HEADER_SIZE + 1] = '.';
  CHECK(nyamp_blob_open_decode(&flags, &name, &name_length, payload, size) ==
        NYAMP_EPROTO);

  /* The declared length and the frame length must agree exactly. */
  CHECK(nyamp_blob_open_encode(payload, sizeof(payload), &size, 0, "aa/bb",
                               5) == NYAMP_OK);
  CHECK(nyamp_blob_open_decode(&flags, &name, &name_length, payload,
                               size - 1) == NYAMP_EPROTO);
  CHECK(nyamp_blob_open_decode(&flags, &name, &name_length, payload,
                               size + 1) == NYAMP_EPROTO);
  CHECK(nyamp_blob_open_decode(&flags, &name, &name_length, payload, 7) ==
        NYAMP_EMSGSIZE);

  memset(&info, 0, sizeof(info));
  info.blob_id = 0x01020304;
  info.size = 875760324ULL;
  info.mtime = 1789000000ULL;
  for (index = 0; index < NYAMP_BLOB_SHA256_SIZE; index++)
    {
      info.sha256[index] = (uint8_t)(0xa0 + index);
    }

  CHECK(nyamp_blob_info_encode(payload, sizeof(payload), &size, &info) ==
        NYAMP_OK);
  CHECK(size == NYAMP_BLOB_INFO_SIZE);
  CHECK(size + NYAMP_STATUS_SIZE <= NYAMP_INLINE_MAX);
  memset(&decoded, 0, sizeof(decoded));
  CHECK(nyamp_blob_info_decode(&decoded, payload, size) == NYAMP_OK);
  CHECK(decoded.blob_id == info.blob_id && decoded.size == info.size &&
        decoded.mtime == info.mtime &&
        memcmp(decoded.sha256, info.sha256, NYAMP_BLOB_SHA256_SIZE) == 0);

  /* A size above 4 GiB must survive: the offset and size are 64-bit because
   * a model already approaches 1 GiB.
   */
  info.size = 0x123456789aULL;
  CHECK(nyamp_blob_info_encode(payload, sizeof(payload), &size, &info) ==
        NYAMP_OK);
  CHECK(nyamp_blob_info_decode(&decoded, payload, size) == NYAMP_OK);
  CHECK(decoded.size == 0x123456789aULL);

  /* Zero is "no blob" on both sides and can never be granted. */
  info.blob_id = 0;
  CHECK(nyamp_blob_info_encode(payload, sizeof(payload), &size, &info) ==
        NYAMP_EINVAL);
  memset(payload, 0, 4);
  CHECK(nyamp_blob_info_decode(&decoded, payload, NYAMP_BLOB_INFO_SIZE) ==
        NYAMP_EPROTO);
  CHECK(nyamp_blob_info_decode(&decoded, payload, NYAMP_BLOB_INFO_SIZE - 1) ==
        NYAMP_EMSGSIZE);
  return 0;
}

static int test_blob_read_close(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  uint32_t blob_id = 0;
  struct nyamp_blob_read_s read = {
    .blob_id = 7,
    .flags = 0,
    .file_offset = 0x100000000ULL + 4096,
    .buffer = {
      .magic = NYAMP_BUFFER_MAGIC,
      .version = NYAMP_BUFFER_VERSION,
      .flags = NYAMP_BUFFER_IN_SHMEM,
      .offset = 0x1000,
      .length = 0,
      .capacity = 0x100000,
      .format = NYAMP_FORMAT_BYTES,
      .lease = 0x0000002a00000001ULL,
      .generation = 42,
    },
  };
  struct nyamp_blob_read_s decoded;

  CHECK(nyamp_blob_read_encode(payload, sizeof(payload), &size, &read) ==
        NYAMP_OK);
  CHECK(size == NYAMP_BLOB_READ_SIZE);
  CHECK(size + NYAMP_STATUS_SIZE <= NYAMP_INLINE_MAX);
  CHECK(nyamp_blob_read_decode(&decoded, payload, size) == NYAMP_OK);
  CHECK(decoded.blob_id == 7 && decoded.flags == 0 &&
        decoded.file_offset == 0x100000000ULL + 4096);
  CHECK(decoded.buffer.offset == 0x1000 && decoded.buffer.capacity == 0x100000 &&
        decoded.buffer.lease == 0x0000002a00000001ULL &&
        decoded.buffer.generation == 42 &&
        decoded.buffer.format == NYAMP_FORMAT_BYTES);

  /* The response reuses the layout: bytes filled travel in `length`, and the
   * last window is marked.
   */
  read.flags = NYAMP_BLOB_READ_EOF;
  read.buffer.length = 12345;
  read.buffer.flags |= NYAMP_BUFFER_LAST;
  CHECK(nyamp_blob_read_encode(payload, sizeof(payload), &size, &read) ==
        NYAMP_OK);
  CHECK(nyamp_blob_read_decode(&decoded, payload, size) == NYAMP_OK);
  CHECK(decoded.flags == NYAMP_BLOB_READ_EOF &&
        decoded.buffer.length == 12345 &&
        (decoded.buffer.flags & NYAMP_BUFFER_LAST) != 0);

  /* More bytes than the window holds is impossible by construction. */
  read.buffer.length = read.buffer.capacity + 1;
  CHECK(nyamp_blob_read_encode(payload, sizeof(payload), &size, &read) ==
        NYAMP_EINVAL);
  read.buffer.length = 0;

  /* A window outside the shared region cannot carry file bytes. */
  read.buffer.flags = 0;
  CHECK(nyamp_blob_read_encode(payload, sizeof(payload), &size, &read) ==
        NYAMP_EINVAL);
  read.buffer.flags = NYAMP_BUFFER_IN_SHMEM;
  read.buffer.capacity = 0;
  CHECK(nyamp_blob_read_encode(payload, sizeof(payload), &size, &read) ==
        NYAMP_EINVAL);
  read.buffer.capacity = 0x100000;

  read.flags = 2;
  CHECK(nyamp_blob_read_encode(payload, sizeof(payload), &size, &read) ==
        NYAMP_EINVAL);
  read.flags = 0;
  read.blob_id = 0;
  CHECK(nyamp_blob_read_encode(payload, sizeof(payload), &size, &read) ==
        NYAMP_EINVAL);
  read.blob_id = 7;

  /* Forged frames: a corrupted descriptor magic and a cleared shmem flag. */
  CHECK(nyamp_blob_read_encode(payload, sizeof(payload), &size, &read) ==
        NYAMP_OK);
  payload[16] ^= 1;
  CHECK(nyamp_blob_read_decode(&decoded, payload, size) == NYAMP_EPROTO);
  payload[16] ^= 1;
  payload[16 + 6] = 0;
  CHECK(nyamp_blob_read_decode(&decoded, payload, size) == NYAMP_EPROTO);
  CHECK(nyamp_blob_read_decode(&decoded, payload, size - 1) ==
        NYAMP_EMSGSIZE);

  CHECK(nyamp_blob_close_encode(payload, sizeof(payload), &size, 7) ==
        NYAMP_OK);
  CHECK(size == NYAMP_BLOB_CLOSE_SIZE);
  CHECK(nyamp_blob_close_decode(&blob_id, payload, size) == NYAMP_OK);
  CHECK(blob_id == 7);
  CHECK(nyamp_blob_close_encode(payload, sizeof(payload), &size, 0) ==
        NYAMP_EINVAL);
  payload[4] = 1;
  CHECK(nyamp_blob_close_decode(&blob_id, payload, size) == NYAMP_EPROTO);
  return 0;
}

static int test_blob_list(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  uint8_t body[NYAMP_BLOB_LIST_BODY_MAX];
  size_t size = 0;
  size_t position = 0;
  uint32_t cursor = 0;
  uint32_t next = 0;
  uint32_t count = 0;
  const char *prefix = NULL;
  size_t prefix_length = 0;
  struct nyamp_blob_entry_s entry;
  char name[64];
  unsigned int appended = 0;

  CHECK(nyamp_blob_list_encode(payload, sizeof(payload), &size, 9, "asr", 3) ==
        NYAMP_OK);
  CHECK(nyamp_blob_list_decode(&cursor, &prefix, &prefix_length, payload,
                               size) == NYAMP_OK);
  CHECK(cursor == 9 && prefix_length == 3 && memcmp(prefix, "asr", 3) == 0);

  /* The empty prefix is the blob root; a traversal is still refused. */
  CHECK(nyamp_blob_list_encode(payload, sizeof(payload), &size, 0, NULL, 0) ==
        NYAMP_OK);
  CHECK(size == NYAMP_BLOB_LIST_HEADER_SIZE);
  CHECK(nyamp_blob_list_decode(&cursor, &prefix, &prefix_length, payload,
                               size) == NYAMP_OK);
  CHECK(cursor == 0 && prefix_length == 0);
  CHECK(nyamp_blob_list_encode(payload, sizeof(payload), &size, 0, "..", 2) ==
        NYAMP_EINVAL);

  /* Fill a page until the encoder says the next entry does not fit. */
  CHECK(nyamp_blob_list_body_begin(body, sizeof(body), &size) == NYAMP_OK);
  for (;;)
    {
      int result;

      snprintf(name, sizeof(name), "encoder-epoch-99-avg-1.int8.%03u.onnx",
               appended);
      entry.size = 1000000ULL * (appended + 1);
      entry.flags = 0;
      entry.name = name;
      entry.name_length = (uint16_t)strlen(name);
      result = nyamp_blob_list_body_append(body, sizeof(body), &size, &entry);
      if (result == NYAMP_EMSGSIZE)
        {
          break;
        }

      CHECK(result == NYAMP_OK);
      appended++;
    }

  CHECK(appended >= 8 && size <= sizeof(body));
  CHECK(nyamp_blob_list_body_finish(body, size, appended) == NYAMP_OK);
  CHECK(nyamp_blob_list_body_decode(&next, &count, body, size) == NYAMP_OK);
  CHECK(next == appended && count == appended);

  for (cursor = 0; cursor < count; cursor++)
    {
      CHECK(nyamp_blob_list_body_next(&entry, &position, body, size) ==
            NYAMP_OK);
      snprintf(name, sizeof(name), "encoder-epoch-99-avg-1.int8.%03u.onnx",
               (unsigned int)cursor);
      CHECK(entry.size == 1000000ULL * (cursor + 1) &&
            entry.name_length == strlen(name) &&
            memcmp(entry.name, name, entry.name_length) == 0);
    }

  CHECK(nyamp_blob_list_body_next(&entry, &position, body, size) ==
        NYAMP_EMSGSIZE);

  /* Truncation, trailing bytes and a nested name are all refused. */
  CHECK(nyamp_blob_list_body_decode(&next, &count, body, size - 1) ==
        NYAMP_EPROTO);
  CHECK(nyamp_blob_list_body_decode(&next, &count, body, size + 1) ==
        NYAMP_EPROTO);
  body[NYAMP_BLOB_LIST_BODY_HEADER_SIZE + NYAMP_BLOB_LIST_ENTRY_HEADER_SIZE] =
      '/';
  CHECK(nyamp_blob_list_body_decode(&next, &count, body, size) ==
        NYAMP_EPROTO);

  entry.name = "a/b";
  entry.name_length = 3;
  CHECK(nyamp_blob_list_body_begin(body, sizeof(body), &size) == NYAMP_OK);
  CHECK(nyamp_blob_list_body_append(body, sizeof(body), &size, &entry) ==
        NYAMP_EINVAL);

  /* An empty last page is legal; an empty page that promises more is a loop.
   */
  CHECK(nyamp_blob_list_body_finish(body, size, 0) == NYAMP_OK);
  CHECK(nyamp_blob_list_body_decode(&next, &count, body, size) == NYAMP_OK);
  CHECK(next == 0 && count == 0);
  CHECK(nyamp_blob_list_body_finish(body, size, 5) == NYAMP_OK);
  CHECK(nyamp_blob_list_body_decode(&next, &count, body, size) ==
        NYAMP_EPROTO);
  return 0;
}

static int test_blob_bench_and_reports(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  uint32_t rounds = 0;
  uint32_t window = 0;
  struct nyamp_blob_bench_s bench = { .mode = NYAMP_BLOB_BENCH_ECHO,
                                      .seed = 0xdeadbeef };
  struct nyamp_blob_bench_s decoded;
  struct nyamp_blob_bench_report_s report = {
    .rounds = 200,
    .window_bytes = 1048576,
    .rtt_min_us = 180,
    .rtt_avg_us = 240,
    .rtt_max_us = 1900,
    .fill_kib_per_s = 400000,
    .copy_kib_per_s = 600000,
    .pattern_errors = 0,
  };
  struct nyamp_blob_bench_report_s report_out;
  struct nyamp_blob_pull_report_s pull = {
    .bytes = 875760324ULL, .elapsed_ms = 21000, .files = 1, .reused = 0
  };
  struct nyamp_blob_pull_report_s pull_out;
  struct nyamp_blob_progress_s progress = {
    .done = 1048576, .total = 875760324ULL, .bytes_per_second = 41943040
  };
  struct nyamp_blob_progress_s progress_out;

  CHECK(nyamp_blob_bench_encode(payload, sizeof(payload), &size, &bench) ==
        NYAMP_OK);
  CHECK(size == NYAMP_BLOB_BENCH_ECHO_SIZE);
  CHECK(nyamp_blob_bench_decode(&decoded, payload, size) == NYAMP_OK);
  CHECK(decoded.mode == NYAMP_BLOB_BENCH_ECHO && decoded.seed == 0xdeadbeef);

  bench.mode = NYAMP_BLOB_BENCH_FILL;
  CHECK(nyamp_blob_bench_encode(payload, sizeof(payload), &size, &bench) ==
        NYAMP_EINVAL);
  bench.buffer.magic = NYAMP_BUFFER_MAGIC;
  bench.buffer.version = NYAMP_BUFFER_VERSION;
  bench.buffer.flags = NYAMP_BUFFER_IN_SHMEM;
  bench.buffer.offset = 0x1000;
  bench.buffer.capacity = 0x100000;
  bench.buffer.format = NYAMP_FORMAT_BYTES;
  bench.buffer.lease = 5;
  bench.buffer.generation = 3;
  CHECK(nyamp_blob_bench_encode(payload, sizeof(payload), &size, &bench) ==
        NYAMP_OK);
  CHECK(size == NYAMP_BLOB_BENCH_FILL_SIZE);
  CHECK(nyamp_blob_bench_decode(&decoded, payload, size) == NYAMP_OK);
  CHECK(decoded.mode == NYAMP_BLOB_BENCH_FILL &&
        decoded.buffer.capacity == 0x100000 && decoded.buffer.lease == 5);

  /* An echo may not smuggle a descriptor, and a fill may not omit one. */
  CHECK(nyamp_blob_bench_decode(&decoded, payload,
                                NYAMP_BLOB_BENCH_ECHO_SIZE) == NYAMP_EPROTO);
  payload[0] = NYAMP_BLOB_BENCH_ECHO;
  CHECK(nyamp_blob_bench_decode(&decoded, payload, size) == NYAMP_EPROTO);
  payload[0] = 9;
  CHECK(nyamp_blob_bench_decode(&decoded, payload, size) == NYAMP_EPROTO);

  /* The pattern is position dependent and can be produced block by block. */
  {
    uint8_t whole[32];
    uint8_t tail[16];

    nyamp_blob_bench_pattern(whole, 8, 0x12345678, 0);
    nyamp_blob_bench_pattern(tail, 4, 0x12345678, 4);
    CHECK(memcmp(whole + 16, tail, sizeof(tail)) == 0);
    CHECK(whole[0] == 0x78 && whole[1] == 0x56 && whole[2] == 0x34 &&
          whole[3] == 0x12);
    CHECK(memcmp(whole, whole + 4, 4) != 0);
  }

  CHECK(nyamp_blob_bench_run_encode(payload, sizeof(payload), &size, 200,
                                    1048576) == NYAMP_OK);
  CHECK(nyamp_blob_bench_run_decode(&rounds, &window, payload, size) ==
        NYAMP_OK);
  CHECK(rounds == 200 && window == 1048576);
  CHECK(nyamp_blob_bench_run_decode(&rounds, &window, payload, size + 1) ==
        NYAMP_EPROTO);

  CHECK(nyamp_blob_bench_report_encode(payload, sizeof(payload), &size,
                                       &report) == NYAMP_OK);
  CHECK(size == NYAMP_BLOB_BENCH_REPORT_SIZE);
  CHECK(nyamp_blob_bench_report_decode(&report_out, payload, size) ==
        NYAMP_OK);
  CHECK(memcmp(&report, &report_out, sizeof(report)) == 0);

  CHECK(nyamp_blob_pull_report_encode(payload, sizeof(payload), &size,
                                      &pull) == NYAMP_OK);
  CHECK(size == NYAMP_BLOB_PULL_REPORT_SIZE);
  CHECK(nyamp_blob_pull_report_decode(&pull_out, payload, size) == NYAMP_OK);
  CHECK(pull_out.bytes == pull.bytes && pull_out.elapsed_ms == 21000 &&
        pull_out.files == 1 && pull_out.reused == 0);

  CHECK(nyamp_blob_progress_encode(payload, sizeof(payload), &size,
                                   &progress) == NYAMP_OK);
  CHECK(size == NYAMP_BLOB_PROGRESS_SIZE);
  CHECK(nyamp_blob_progress_decode(&progress_out, payload, size) ==
        NYAMP_OK);
  CHECK(progress_out.done == 1048576 &&
        progress_out.total == 875760324ULL &&
        progress_out.bytes_per_second == 41943040);

  /* Progress past the total would render as more than 100 percent. */
  progress.done = progress.total + 1;
  CHECK(nyamp_blob_progress_encode(payload, sizeof(payload), &size,
                                   &progress) == NYAMP_EINVAL);
  return 0;
}

static int test_blob_request_header(void)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  struct nyamp_header_s header = {
    .service = NYAMP_SERVICE_BLOB,
    .opcode = NYAMP_BLOB_READ,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = NYAMP_REQUEST_ID_COMPUTE | 17,
    .deadline_ms = 0,
    .generation = 42,
    .payload_size = NYAMP_BLOB_READ_SIZE,
  };
  struct nyamp_header_s decoded;

  /* The origin bit is part of an ordinary 64-bit id; the header codec must
   * carry it untouched in both directions.
   */
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  CHECK(nyamp_header_decode(&decoded, wire,
                            NYAMP_WIRE_HEADER_SIZE + header.payload_size) ==
        NYAMP_OK);
  CHECK(decoded.service == NYAMP_SERVICE_BLOB &&
        decoded.request_id == (NYAMP_REQUEST_ID_COMPUTE | 17) &&
        (decoded.request_id & NYAMP_REQUEST_ID_COMPUTE) != 0);

  /* Aborting an OPEN is an ordinary payload-free cancel. */
  header.flags = NYAMP_FLAG_CANCEL;
  header.opcode = NYAMP_BLOB_OPEN;
  header.payload_size = 0;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  return 0;
}

static void nyamp_put_test_le32(uint8_t *dest, uint32_t value)
{
  dest[0] = (uint8_t)value;
  dest[1] = (uint8_t)(value >> 8);
  dest[2] = (uint8_t)(value >> 16);
  dest[3] = (uint8_t)(value >> 24);
}

static int test_llm_chat_chunks(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  uint8_t body[NYAMP_LLM_CHAT_MAX_CHUNK];
  size_t size = 0;
  const uint8_t *bytes = NULL;
  struct nyamp_llm_chat_s chunk = {
    .total = 1000,
    .offset = 436,
    .length = 436,
    .max_new_tokens = 256,
    .flags = NYAMP_LLM_CHAT_GUARD_UNTRUSTED | NYAMP_LLM_CHAT_STREAM_TOKENS,
  };
  struct nyamp_llm_chat_s out;
  unsigned int index;

  for (index = 0; index < sizeof(body); index++)
    {
      body[index] = (uint8_t)(index * 7 + 3);
    }

  /* The largest chunk exactly fills one RPMsg payload. */
  CHECK(NYAMP_LLM_CHAT_MAX_CHUNK == 436);
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_OK);
  CHECK(size == NYAMP_INLINE_MAX);
  CHECK(nyamp_llm_chat_decode(&out, &bytes, payload, size) == NYAMP_OK);
  CHECK(out.total == 1000 && out.offset == 436 && out.length == 436 &&
        out.max_new_tokens == 256 && out.flags == chunk.flags);
  CHECK(bytes == payload + NYAMP_LLM_CHAT_HEADER_SIZE &&
        memcmp(bytes, body, 436) == 0);

  /* One byte short or long of the declared length is refused. */
  CHECK(nyamp_llm_chat_decode(&out, &bytes, payload, size - 1) ==
        NYAMP_EMSGSIZE);
  chunk.length = 100;
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_OK);
  CHECK(nyamp_llm_chat_decode(&out, &bytes, payload, size + 1) ==
        NYAMP_EMSGSIZE);
  CHECK(nyamp_llm_chat_decode(&out, &bytes, payload, 19) == NYAMP_EMSGSIZE);

  /* A chunk must lie inside the body it claims to belong to. */
  chunk.offset = 950;
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_EINVAL);
  chunk.offset = 0;
  chunk.length = 0;
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_EINVAL);
  chunk.length = NYAMP_LLM_CHAT_MAX_CHUNK + 1;
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_EINVAL);
  chunk.length = 10;
  chunk.total = NYAMP_LLM_CHAT_MAX_BODY + 1;
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_EINVAL);
  chunk.total = NYAMP_LLM_CHAT_MAX_BODY;
  chunk.offset = NYAMP_LLM_CHAT_MAX_BODY - 10;
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_OK);

  /* An offset near UINT32_MAX must not wrap the bounds check. */
  nyamp_put_test_le32(payload + 0, 100);
  nyamp_put_test_le32(payload + 4, 0xfffffff0U);
  nyamp_put_test_le32(payload + 8, 0x20);
  CHECK(nyamp_llm_chat_decode(&out, &bytes, payload,
                              NYAMP_LLM_CHAT_HEADER_SIZE + 0x20) ==
        NYAMP_EPROTO);

  /* No flag outside the defined set means anything. */
  chunk.offset = 0;
  chunk.total = 10;
  chunk.flags = 4;
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_EINVAL);
  chunk.flags = 0;
  CHECK(nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk, body) ==
        NYAMP_OK);
  payload[16] = 0x80;
  CHECK(nyamp_llm_chat_decode(&out, &bytes, payload, size) == NYAMP_EPROTO);
  return 0;
}

static int test_llm_chat_result_and_finish(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  uint8_t body[NYAMP_LLM_RESULT_MAX_CHUNK];
  size_t size = 0;
  const uint8_t *bytes = NULL;
  struct nyamp_llm_result_s chunk = { .total = 2000, .offset = 1556,
                                      .length = 444 };
  struct nyamp_llm_result_s out;
  struct nyamp_llm_chat_finish_s finish = {
    .status = NYAMP_MODEL_PROMPT_TOO_LONG,
    .sequence = 0,
    .prompt_tokens = 1900,
    .completion_tokens = 0,
    .prefill_ms = 0,
    .decode_ms = 0,
    .context_limit = 2048,
  };
  struct nyamp_llm_chat_finish_s finish_out;
  int32_t generate_status = 0;
  uint32_t sequence = 0;

  memset(body, 0x5a, sizeof(body));
  CHECK(NYAMP_LLM_RESULT_MAX_CHUNK == 444);
  CHECK(nyamp_llm_result_encode(payload, sizeof(payload), &size, &chunk,
                                body) == NYAMP_OK);
  CHECK(size == NYAMP_INLINE_MAX);
  CHECK(nyamp_llm_result_decode(&out, &bytes, payload, size) == NYAMP_OK);
  CHECK(out.total == 2000 && out.offset == 1556 && out.length == 444 &&
        bytes[0] == 0x5a && bytes[443] == 0x5a);

  chunk.offset = 1557;
  CHECK(nyamp_llm_result_encode(payload, sizeof(payload), &size, &chunk,
                                body) == NYAMP_EINVAL);
  CHECK(nyamp_llm_result_decode(&out, &bytes, payload, size - 1) ==
        NYAMP_EMSGSIZE);
  payload[0] = 0;
  payload[1] = 0;
  CHECK(nyamp_llm_result_decode(&out, &bytes, payload, size) == NYAMP_EPROTO);

  /* The overflow status travels with the two numbers the caller needs to
   * trim its history, and stays disjoint from every older status.
   */
  CHECK(NYAMP_MODEL_PROMPT_TOO_LONG == -11);
  CHECK(nyamp_llm_chat_finish_encode(payload, sizeof(payload), &size,
                                     &finish) == NYAMP_OK);
  CHECK(size == NYAMP_LLM_CHAT_FINISH_SIZE);
  CHECK(nyamp_llm_chat_finish_decode(&finish_out, payload, size) == NYAMP_OK);
  CHECK(finish_out.status == NYAMP_MODEL_PROMPT_TOO_LONG &&
        finish_out.prompt_tokens == 1900 && finish_out.context_limit == 2048);

  finish.status = NYAMP_MODEL_OK;
  finish.prompt_tokens = 1093;
  finish.completion_tokens = 17;
  finish.prefill_ms = 4100;
  finish.decode_ms = 1250;
  CHECK(nyamp_llm_chat_finish_encode(payload, sizeof(payload), &size,
                                     &finish) == NYAMP_OK);
  CHECK(nyamp_llm_chat_finish_decode(&finish_out, payload, size) == NYAMP_OK);
  CHECK(memcmp(&finish, &finish_out, sizeof(finish)) == 0);

  /* The two finish layouts must never be taken for each other: a GENERATE
   * client reading a CHAT finish (or the reverse) gets an error, not a
   * status that happens to be zero.
   */
  CHECK(nyamp_llm_finish_decode(&generate_status, &sequence, payload, size) ==
        NYAMP_EPROTO);
  CHECK(nyamp_llm_chat_finish_decode(&finish_out, payload,
                                     NYAMP_LLM_FINISH_SIZE) ==
        NYAMP_EMSGSIZE);
  CHECK(nyamp_llm_chat_finish_decode(&finish_out, payload, size + 1) ==
        NYAMP_EPROTO);
  return 0;
}

static int test_asr_attach_and_end(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  uint32_t sample_rate = 0, max_samples = 0;
  uint16_t channels = 0, flags = 0;
  uint64_t sample = 0;

  /* The encoder sets the attach flag itself and keeps the format flag. */
  CHECK(nyamp_asr_attach_encode(payload, sizeof(payload), &size, 16000, 1,
                                NYAMP_AUDIO_BEGIN_S16, 128000,
                                0x123456789aULL) == NYAMP_OK);
  CHECK(size == NYAMP_ASR_ATTACH_SIZE);
  CHECK(nyamp_asr_attach_decode(&sample_rate, &channels, &flags, &max_samples,
                                &sample, payload, size) == NYAMP_OK);
  CHECK(sample_rate == 16000 && channels == 1 && max_samples == 128000);
  CHECK(flags == (NYAMP_AUDIO_BEGIN_S16 | NYAMP_ASR_BEGIN_ATTACH_KWS));
  CHECK(sample == 0x123456789aULL);

  /* The first 16 bytes are an ordinary BEGIN, so a dispatcher can look at
   * the flags before it knows which of the two layouts it holds.
   */
  CHECK(nyamp_asr_begin_decode(&sample_rate, &channels, &flags, &max_samples,
                               payload, NYAMP_ASR_BEGIN_SIZE) == NYAMP_OK);
  CHECK((flags & NYAMP_ASR_BEGIN_ATTACH_KWS) != 0);

  /* A plain BEGIN is not an attach, whatever its length claims. */
  CHECK(nyamp_asr_attach_decode(&sample_rate, &channels, &flags, &max_samples,
                                &sample, payload,
                                NYAMP_ASR_BEGIN_SIZE) == NYAMP_EMSGSIZE);
  payload[6] = 0;
  CHECK(nyamp_asr_attach_decode(&sample_rate, &channels, &flags, &max_samples,
                                &sample, payload, size) == NYAMP_EPROTO);
  payload[6] = NYAMP_ASR_BEGIN_ATTACH_KWS | 0x80;
  CHECK(nyamp_asr_attach_decode(&sample_rate, &channels, &flags, &max_samples,
                                &sample, payload, size) == NYAMP_EPROTO);
  CHECK(nyamp_asr_attach_encode(payload, sizeof(payload), &size, 16000, 1,
                                0x80, 0, 0) == NYAMP_EINVAL);
  CHECK(nyamp_asr_attach_encode(payload, NYAMP_ASR_ATTACH_SIZE - 1, &size,
                                16000, 1, 0, 0, 0) == NYAMP_EMSGSIZE);

  CHECK(nyamp_asr_end_encode(payload, sizeof(payload), &size,
                             NYAMP_STREAM_SAMPLE_NOW) == NYAMP_OK);
  CHECK(size == NYAMP_ASR_END_SIZE);
  CHECK(nyamp_asr_end_decode(&sample, payload, size) == NYAMP_OK);
  CHECK(sample == NYAMP_STREAM_SAMPLE_NOW);
  CHECK(nyamp_asr_end_encode(payload, sizeof(payload), &size, 48000) ==
        NYAMP_OK);
  CHECK(nyamp_asr_end_decode(&sample, payload, size) == NYAMP_OK);
  CHECK(sample == 48000);
  CHECK(nyamp_asr_end_decode(&sample, payload, size - 1) == NYAMP_EMSGSIZE);
  CHECK(nyamp_asr_end_decode(&sample, payload, size + 1) == NYAMP_EPROTO);

  /* The partial flags are one small set, distinct from the buffer flags. */
  CHECK(NYAMP_ASR_PARTIAL_ALL == 7U);
  return 0;
}

static int test_tts_text_chunks(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  uint8_t text[NYAMP_TTS_TEXT_MAX_CHUNK];
  size_t size = 0;
  const uint8_t *bytes = NULL;
  struct nyamp_tts_text_s chunk = {
    .total = 1000,
    .offset = 0,
    .length = NYAMP_TTS_TEXT_MAX_CHUNK,
    .speaker_id = 1,
    .speed = 1.25f,
    .window_samples = 44100,
    .flags = 0,
  };
  struct nyamp_tts_text_s decoded;

  memset(text, 'x', sizeof(text));
  CHECK(NYAMP_TTS_TEXT_MAX_CHUNK == 428U);
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_OK);
  CHECK(size == NYAMP_INLINE_MAX);
  CHECK(nyamp_tts_text_decode(&decoded, &bytes, payload, size) == NYAMP_OK);
  CHECK(decoded.total == 1000 && decoded.offset == 0 &&
        decoded.length == NYAMP_TTS_TEXT_MAX_CHUNK &&
        decoded.speaker_id == 1 && decoded.speed == 1.25f &&
        decoded.window_samples == 44100 && decoded.flags == 0);
  CHECK(bytes == payload + NYAMP_TTS_TEXT_HEADER_SIZE &&
        memcmp(bytes, text, decoded.length) == 0);

  /* A declared length must match what arrived. */
  CHECK(nyamp_tts_text_decode(&decoded, &bytes, payload, size - 1) ==
        NYAMP_EMSGSIZE);
  CHECK(nyamp_tts_text_decode(&decoded, &bytes, payload,
                              NYAMP_TTS_TEXT_HEADER_SIZE - 1) ==
        NYAMP_EMSGSIZE);

  /* The tail chunk, and the default window. */
  chunk.offset = 856;
  chunk.length = 144;
  chunk.window_samples = 0;
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_OK);
  CHECK(nyamp_tts_text_decode(&decoded, &bytes, payload, size) == NYAMP_OK);
  CHECK(decoded.offset == 856 && decoded.length == 144);

  /* A span outside the body, an oversized body, a bad speed, a window the
   * slot cannot hold, and a reserved flag are all refused by both sides.
   */
  chunk.length = 145;
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_EINVAL);
  chunk.length = 144;
  chunk.total = NYAMP_TTS_TEXT_MAX_BODY + 1;
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_EINVAL);
  chunk.total = 1000;
  chunk.speed = 0.0f;
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_EINVAL);
  chunk.speed = 1.0f;
  chunk.window_samples = NYAMP_TTS_WINDOW_SAMPLES_MAX + 1;
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_EINVAL);
  chunk.window_samples = NYAMP_TTS_WINDOW_SAMPLES_MIN - 1;
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_EINVAL);
  chunk.window_samples = 0;
  chunk.flags = 1;
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_EINVAL);
  chunk.flags = 0;
  CHECK(nyamp_tts_text_encode(payload, sizeof(payload), &size, &chunk,
                              text) == NYAMP_OK);
  payload[24] = 1;
  CHECK(nyamp_tts_text_decode(&decoded, &bytes, payload, size) ==
        NYAMP_EPROTO);
  payload[24] = 0;

  /* A NaN speed (all-ones exponent, non-zero mantissa). */
  payload[16] = 1;
  payload[17] = 0;
  payload[18] = 0xc0;
  payload[19] = 0x7f;
  CHECK(nyamp_tts_text_decode(&decoded, &bytes, payload, size) ==
        NYAMP_EPROTO);

  /* The slot holds exactly the largest window. */
  CHECK(NYAMP_TTS_WINDOW_SAMPLES_MAX * 4U == 0x00100000U);
  return 0;
}

static int test_kws_messages(void)
{
  uint8_t payload[NYAMP_INLINE_MAX];
  size_t size = 0;
  struct nyamp_kws_load_s load = {
    .threshold = 0.1f,
    .score = 2.0f,
    .max_active_paths = 16,
    .num_trailing_blanks = 1,
    .directory_length = 3,
    .keywords_length = 12,
    .directory = "kws",
    .keywords = "keywords.txt",
  };
  struct nyamp_kws_load_s loaded;
  struct nyamp_kws_push_s push = {
    .buffer = {
      .magic = NYAMP_BUFFER_MAGIC,
      .version = NYAMP_BUFFER_VERSION,
      .flags = NYAMP_BUFFER_IN_SHMEM,
      .offset = 0x120000,
      .length = 6400,
      .capacity = 6400,
      .format = NYAMP_FORMAT_F32,
      .lease = 0x0000000980000001ULL,
      .generation = 9,
    },
    .sequence = 41,
    .flags = NYAMP_KWS_PUSH_DISCONTINUITY,
    .stream_sample = 0x100000001ULL,
  };
  struct nyamp_kws_push_s pushed;
  struct nyamp_kws_detected_s detected = {
    .sequence = 3,
    .keyword_id = 0,
    .flags = NYAMP_KWS_DETECTED_HAS_OFFSETS,
    .score = -1.0f,
    .label_length = 14,
    .start_sample = 160000,
    .end_sample = 184320,
    .trigger_sample = 190720,
    .label = "nihao_openvela",
  };
  struct nyamp_kws_detected_s seen;
  uint64_t next = 0;

  CHECK(nyamp_kws_load_encode(payload, sizeof(payload), &size, &load) ==
        NYAMP_OK);
  CHECK(size == NYAMP_KWS_LOAD_HEADER_SIZE + 15);
  CHECK(nyamp_kws_load_decode(&loaded, payload, size) == NYAMP_OK);
  CHECK(loaded.threshold == 0.1f && loaded.score == 2.0f &&
        loaded.max_active_paths == 16 && loaded.num_trailing_blanks == 1);
  CHECK(loaded.directory_length == 3 &&
        memcmp(loaded.directory, "kws", 3) == 0);
  CHECK(loaded.keywords_length == 12 &&
        memcmp(loaded.keywords, "keywords.txt", 12) == 0);
  CHECK(nyamp_kws_load_decode(&loaded, payload, size - 1) == NYAMP_EMSGSIZE);
  CHECK(nyamp_kws_load_decode(&loaded, payload, size + 1) == NYAMP_EMSGSIZE);

  /* The keywords name is optional; the directory is not. */
  load.keywords_length = 0;
  load.keywords = NULL;
  CHECK(nyamp_kws_load_encode(payload, sizeof(payload), &size, &load) ==
        NYAMP_OK);
  CHECK(nyamp_kws_load_decode(&loaded, payload, size) == NYAMP_OK);
  CHECK(loaded.keywords_length == 0);
  load.directory_length = 0;
  CHECK(nyamp_kws_load_encode(payload, sizeof(payload), &size, &load) ==
        NYAMP_EINVAL);
  load.directory_length = 3;
  load.threshold = -0.5f;
  CHECK(nyamp_kws_load_encode(payload, sizeof(payload), &size, &load) ==
        NYAMP_EINVAL);
  load.threshold = 0.0f;
  load.directory_length = NYAMP_INLINE_MAX;
  CHECK(nyamp_kws_load_encode(payload, sizeof(payload), &size, &load) ==
        NYAMP_EMSGSIZE);

  /* BEGIN is the ASR layout under another name. */
  {
    uint32_t sample_rate = 0, window = 0;
    uint16_t channels = 0, flags = 0;

    CHECK(nyamp_kws_begin_encode(payload, sizeof(payload), &size, 16000, 1,
                                 NYAMP_AUDIO_BEGIN_S16, 1600) == NYAMP_OK);
    CHECK(size == NYAMP_KWS_BEGIN_SIZE);
    CHECK(nyamp_kws_begin_decode(&sample_rate, &channels, &flags, &window,
                                 payload, size) == NYAMP_OK);
    CHECK(sample_rate == 16000 && channels == 1 && window == 1600 &&
          flags == NYAMP_AUDIO_BEGIN_S16);
  }

  CHECK(nyamp_kws_push_encode(payload, sizeof(payload), &size, &push) ==
        NYAMP_OK);
  CHECK(size == NYAMP_KWS_PUSH_SIZE);
  CHECK(nyamp_kws_push_decode(&pushed, payload, size) == NYAMP_OK);
  CHECK(pushed.sequence == 41 &&
        pushed.flags == NYAMP_KWS_PUSH_DISCONTINUITY &&
        pushed.stream_sample == 0x100000001ULL);
  CHECK(pushed.buffer.offset == 0x120000 && pushed.buffer.length == 6400 &&
        pushed.buffer.lease == 0x0000000980000001ULL &&
        pushed.buffer.generation == 9);
  CHECK(nyamp_kws_push_decode(&pushed, payload, size - 1) == NYAMP_EMSGSIZE);
  CHECK(nyamp_kws_push_decode(&pushed, payload, size + 1) == NYAMP_EPROTO);
  payload[NYAMP_BUFFER_SIZE + 4] = 0x02;
  CHECK(nyamp_kws_push_decode(&pushed, payload, size) == NYAMP_EPROTO);
  payload[NYAMP_BUFFER_SIZE + 4] = 0;
  payload[NYAMP_BUFFER_SIZE + 6] = 1;
  CHECK(nyamp_kws_push_decode(&pushed, payload, size) == NYAMP_EPROTO);
  push.flags = 0x8000;
  CHECK(nyamp_kws_push_encode(payload, sizeof(payload), &size, &push) ==
        NYAMP_EINVAL);

  CHECK(nyamp_kws_push_ack_encode(payload, sizeof(payload), &size,
                                  0x100001901ULL) == NYAMP_OK);
  CHECK(size == NYAMP_KWS_PUSH_ACK_SIZE);
  CHECK(nyamp_kws_push_ack_decode(&next, payload, size) == NYAMP_OK);
  CHECK(next == 0x100001901ULL);
  CHECK(nyamp_kws_push_ack_decode(&next, payload, size + 1) == NYAMP_EPROTO);

  CHECK(nyamp_kws_detected_encode(payload, sizeof(payload), &size,
                                  &detected) == NYAMP_OK);
  CHECK(size == NYAMP_KWS_DETECTED_HEADER_SIZE + 14);
  CHECK(nyamp_kws_detected_decode(&seen, payload, size) == NYAMP_OK);
  CHECK(seen.sequence == 3 && seen.keyword_id == 0 &&
        seen.flags == NYAMP_KWS_DETECTED_HAS_OFFSETS && seen.score == -1.0f);
  CHECK(seen.start_sample == 160000 && seen.end_sample == 184320 &&
        seen.trigger_sample == 190720);
  CHECK(seen.label_length == 14 &&
        memcmp(seen.label, "nihao_openvela", 14) == 0);
  CHECK(nyamp_kws_detected_decode(&seen, payload, size - 1) ==
        NYAMP_EMSGSIZE);

  /* Offsets that run backwards cannot have come from a decoder. */
  detected.start_sample = detected.end_sample + 1;
  CHECK(nyamp_kws_detected_encode(payload, sizeof(payload), &size,
                                  &detected) == NYAMP_EINVAL);
  detected.flags = 0;
  CHECK(nyamp_kws_detected_encode(payload, sizeof(payload), &size,
                                  &detected) == NYAMP_OK);
  detected.flags = 0x4;
  CHECK(nyamp_kws_detected_encode(payload, sizeof(payload), &size,
                                  &detected) == NYAMP_EINVAL);
  detected.flags = 0;
  detected.label_length = NYAMP_KWS_MAX_LABEL + 1;
  CHECK(nyamp_kws_detected_encode(payload, sizeof(payload), &size,
                                  &detected) == NYAMP_EMSGSIZE);

  /* The service ids the control domain is built against. */
  CHECK(NYAMP_SERVICE_KWS == 10 && NYAMP_SERVICE_SPEAKER == 11);
  return 0;
}

static int test_kws_labels(void)
{
  uint8_t body[64];
  size_t size = 0;
  size_t position = 0;
  uint16_t count = 0;
  uint16_t length = 0;
  const char *label = NULL;

  CHECK(nyamp_kws_labels_begin(body, sizeof(body), &size) == NYAMP_OK);
  CHECK(size == NYAMP_KWS_LABELS_HEADER_SIZE);
  CHECK(nyamp_kws_labels_decode(&count, body, size) == NYAMP_OK);
  CHECK(count == 0);

  CHECK(nyamp_kws_labels_append(body, sizeof(body), &size, "nihao_openvela",
                                14) == NYAMP_OK);
  CHECK(nyamp_kws_labels_append(body, sizeof(body), &size, "hello_openvela",
                                14) == NYAMP_OK);
  CHECK(size == NYAMP_KWS_LABELS_HEADER_SIZE + 2 * 16);
  CHECK(nyamp_kws_labels_decode(&count, body, size) == NYAMP_OK);
  CHECK(count == 2);

  CHECK(nyamp_kws_labels_next(&label, &length, &position, body, size) ==
        NYAMP_OK);
  CHECK(length == 14 && memcmp(label, "nihao_openvela", 14) == 0);
  CHECK(nyamp_kws_labels_next(&label, &length, &position, body, size) ==
        NYAMP_OK);
  CHECK(length == 14 && memcmp(label, "hello_openvela", 14) == 0);
  CHECK(position == size);
  CHECK(nyamp_kws_labels_next(&label, &length, &position, body, size) ==
        NYAMP_EPROTO);

  /* A label that does not fit is reported, and the body stays valid. */
  CHECK(nyamp_kws_labels_append(body, sizeof(body), &size,
                                "a_label_that_is_far_too_long_for_it", 35) ==
        NYAMP_EMSGSIZE);
  CHECK(nyamp_kws_labels_decode(&count, body, size) == NYAMP_OK);
  CHECK(count == 2);

  /* Truncated, padded, and a count that promises more than there is. */
  CHECK(nyamp_kws_labels_decode(&count, body, size - 1) == NYAMP_EPROTO);
  CHECK(nyamp_kws_labels_decode(&count, body, size + 1) == NYAMP_EPROTO);
  body[0] = 3;
  CHECK(nyamp_kws_labels_decode(&count, body, size) == NYAMP_EPROTO);
  body[0] = 2;
  body[2] = 1;
  CHECK(nyamp_kws_labels_decode(&count, body, size) == NYAMP_EPROTO);
  CHECK(nyamp_kws_labels_append(body, sizeof(body), &size, "", 0) ==
        NYAMP_EINVAL);
  return 0;
}

int main(void)
{
  if (test_round_trip() != 0 || test_rejections() != 0 ||
      test_malformed_inputs() != 0 || test_llm_chunk() != 0 ||
      test_llm_token() != 0 || test_llm_finish() != 0 ||
      test_cancel_has_no_payload() != 0 || test_buffer_descriptor() != 0 ||
      test_asr_messages() != 0 || test_tts_messages() != 0 ||
      test_status_prefix() != 0 || test_blob_names() != 0 ||
      test_blob_open() != 0 || test_blob_read_close() != 0 ||
      test_blob_list() != 0 || test_blob_bench_and_reports() != 0 ||
      test_blob_request_header() != 0 || test_llm_chat_chunks() != 0 ||
      test_llm_chat_result_and_finish() != 0 ||
      test_asr_attach_and_end() != 0 || test_tts_text_chunks() != 0 ||
      test_kws_messages() != 0 || test_kws_labels() != 0)
    {
      return 1;
    }
  puts("nyamp protocol tests passed");
  return 0;
}
