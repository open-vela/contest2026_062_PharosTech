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

/* Shared-memory buffer descriptor.
 *
 * Audio and image payloads do not fit the RPMsg inline limit: a single second
 * of 16 kHz float32 PCM is 64 KiB, and the TTS vocoder emits 1 MiB in one
 * call.  Sending those as RPMsg chunks would need hundreds of round trips, so
 * a message carries only this descriptor and the bytes live in the reserved
 * shared region at 0x47C00000.
 *
 * All fields are little-endian and encoded field by field; the struct is never
 * memcpy'd onto the wire.
 *
 * offset/length are absolute within the shared region rather than a slot
 * index, so the placement policy can change without a wire change.
 *
 * length is the number of valid bytes and is NOT the slot capacity.  The TTS
 * vocoder validates a full 512-frame output buffer but only `frames` of it are
 * meaningful, so the two values genuinely differ.
 *
 * lease is minted by the compute domain, which is the only allocator, and
 * echoed back unchanged by the control domain.  It carries the generation the
 * grant belongs to, so a stale or foreign lease is rejected without either
 * side maintaining a shared lock or a shared allocator.
 */

#define NYAMP_BUFFER_MAGIC        0x5342594eU /* "NYBS" in little endian */
#define NYAMP_BUFFER_VERSION      1U
#define NYAMP_BUFFER_SIZE         40U

#define NYAMP_BUFFER_IN_SHMEM     (1U << 0)
#define NYAMP_BUFFER_FROM_COMPUTE (1U << 1)
#define NYAMP_BUFFER_LAST         (1U << 2)
#define NYAMP_BUFFER_RESYNC       (1U << 3)
#define NYAMP_BUFFER_ALL                                                   \
  (NYAMP_BUFFER_IN_SHMEM | NYAMP_BUFFER_FROM_COMPUTE | NYAMP_BUFFER_LAST | \
   NYAMP_BUFFER_RESYNC)

enum nyamp_format_e
{
  NYAMP_FORMAT_NONE = 0,
  NYAMP_FORMAT_F32 = 1,  /* Normalized mono PCM, [-1, 1].          */
  NYAMP_FORMAT_S16 = 2,  /* Signed 16-bit mono PCM.                */
  NYAMP_FORMAT_I64 = 3,  /* Phoneme and tone id vectors.           */
  NYAMP_FORMAT_UTF8 = 4, /* Text.                                  */
};

struct nyamp_buffer_s
{
  uint32_t magic;
  uint16_t version;
  uint16_t flags;
  uint32_t offset;
  uint32_t length;
  uint32_t capacity;
  uint32_t format;
  uint64_t lease;
  uint32_t generation;
  uint32_t reserved;
};

/* ASR service opcodes.
 *
 * The control domain owns the audio: it captures a window, writes it into the
 * granted slot, and submits it.  The compute domain accumulates windows into
 * its own memory, which is why the transfer is windowed rather than one large
 * buffer -- the control domain is the memory-constrained side.
 *
 * BEGIN asks for a grant and receives a buffer descriptor back.  PUSH submits
 * one filled window.  RELEASE returns a grant, and no producer may write a
 * published window again until it has been released.
 *
 * EVENT_PARTIAL carries a text delta, not the accumulated transcript.  A
 * transducer decoder may rewrite earlier tokens, so a delta is not always a
 * suffix of the previous text; when it is not, the RESYNC flag says the text
 * is a full replacement instead.
 */

#define NYAMP_ASR_LOAD          1U
#define NYAMP_ASR_UNLOAD        2U
#define NYAMP_ASR_BEGIN         3U
#define NYAMP_ASR_PUSH          4U
#define NYAMP_ASR_RELEASE       5U
#define NYAMP_ASR_CANCEL        6U
#define NYAMP_ASR_EVENT_PARTIAL 0x80U
#define NYAMP_ASR_EVENT_FINISH  0x81U

/* sample_rate, channels, flags, max_samples. */
#define NYAMP_ASR_BEGIN_SIZE 16U

/* buffer descriptor, sequence, flags, total_samples, consumed_samples. */
#define NYAMP_ASR_PUSH_HEADER_SIZE (NYAMP_BUFFER_SIZE + 16U)

/* sequence, consumed_samples, flags, reserved, then the UTF-8 text. */
#define NYAMP_ASR_PARTIAL_HEADER_SIZE 12U
#define NYAMP_ASR_MAX_TEXT            (NYAMP_INLINE_MAX - NYAMP_ASR_PARTIAL_HEADER_SIZE)

/* status, sequence. */
#define NYAMP_ASR_FINISH_SIZE 8U

#define NYAMP_ASR_MAX_PATH    NYAMP_INLINE_MAX

/* TTS service opcodes.
 *
 * SYNTH takes phoneme and tone ids, not text: the model layer has no
 * grapheme-to-phoneme front end, so accepting text here would be pretending to
 * a capability that does not exist.
 *
 * The synthesized PCM always lands in the shared region because the smallest
 * useful result is already far past the inline limit; EVENT_PCM returns the
 * descriptor rather than the samples.
 */

#define NYAMP_TTS_LOAD         1U
#define NYAMP_TTS_UNLOAD       2U
#define NYAMP_TTS_SYNTH        3U
#define NYAMP_TTS_RELEASE      4U
#define NYAMP_TTS_CANCEL       5U
#define NYAMP_TTS_EVENT_PCM    0x80U
#define NYAMP_TTS_EVENT_FINISH 0x81U

/* phoneme_count, speaker_id, speed (float bits), bucket_frames. */
#define NYAMP_TTS_SYNTH_SIZE 16U

/* buffer descriptor, sequence, sample_rate, channels, valid_samples. */
#define NYAMP_TTS_PCM_HEADER_SIZE (NYAMP_BUFFER_SIZE + 16U)

/* status, sequence, total_samples. */
#define NYAMP_TTS_FINISH_SIZE 12U

#define NYAMP_TTS_MAX_PATH    NYAMP_INLINE_MAX

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

int nyamp_buffer_encode(uint8_t *payload, size_t payload_capacity,
                        size_t *payload_size,
                        const struct nyamp_buffer_s *buffer);
int nyamp_buffer_decode(struct nyamp_buffer_s *buffer, const uint8_t *payload,
                        size_t payload_size);

int nyamp_asr_begin_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t sample_rate,
                           uint16_t channels, uint16_t flags,
                           uint32_t max_samples);
int nyamp_asr_begin_decode(uint32_t *sample_rate, uint16_t *channels,
                           uint16_t *flags, uint32_t *max_samples,
                           const uint8_t *payload, size_t payload_size);

int nyamp_asr_push_encode(uint8_t *payload, size_t payload_capacity,
                          size_t *payload_size,
                          const struct nyamp_buffer_s *buffer,
                          uint32_t sequence, uint16_t flags,
                          uint32_t total_samples, uint32_t consumed_samples);
int nyamp_asr_push_decode(struct nyamp_buffer_s *buffer, uint32_t *sequence,
                          uint16_t *flags, uint32_t *total_samples,
                          uint32_t *consumed_samples, const uint8_t *payload,
                          size_t payload_size);

int nyamp_asr_partial_encode(uint8_t *payload, size_t payload_capacity,
                             size_t *payload_size, uint32_t sequence,
                             uint32_t consumed_samples, uint16_t flags,
                             const char *text, size_t text_length);
int nyamp_asr_partial_decode(uint32_t *sequence, uint32_t *consumed_samples,
                             uint16_t *flags, const char **text,
                             size_t *text_length, const uint8_t *payload,
                             size_t payload_size);

int nyamp_asr_finish_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, int32_t status,
                            uint32_t sequence);
int nyamp_asr_finish_decode(int32_t *status, uint32_t *sequence,
                            const uint8_t *payload, size_t payload_size);

int nyamp_tts_synth_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t phoneme_count,
                           uint32_t speaker_id, float speed,
                           uint32_t bucket_frames);
int nyamp_tts_synth_decode(uint32_t *phoneme_count, uint32_t *speaker_id,
                           float *speed, uint32_t *bucket_frames,
                           const uint8_t *payload, size_t payload_size);

int nyamp_tts_pcm_encode(uint8_t *payload, size_t payload_capacity,
                         size_t *payload_size,
                         const struct nyamp_buffer_s *buffer,
                         uint32_t sequence, uint32_t sample_rate,
                         uint32_t channels, uint32_t valid_samples);
int nyamp_tts_pcm_decode(struct nyamp_buffer_s *buffer, uint32_t *sequence,
                         uint32_t *sample_rate, uint32_t *channels,
                         uint32_t *valid_samples, const uint8_t *payload,
                         size_t payload_size);

int nyamp_tts_finish_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, int32_t status,
                            uint32_t sequence, uint32_t total_samples);
int nyamp_tts_finish_decode(int32_t *status, uint32_t *sequence,
                            uint32_t *total_samples, const uint8_t *payload,
                            size_t payload_size);

#ifdef __cplusplus
}
#endif

#endif /* __TOOLS_AMP_PROTOCOL_NYAMP_PROTOCOL_H */
