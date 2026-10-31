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

/* Model directory or file, UTF-8, not necessarily NUL terminated.
 *
 * An absolute path names storage the compute domain can reach itself and is
 * handed to the backend untouched.  A relative one that satisfies the blob
 * name rules ("llm/model.rkllm", or a directory such as "asr") is a logical
 * name: the compute domain first pulls it from the control domain through
 * the BLOB service below, then loads the local copy.  The LOAD response then
 * arrives only after the pull, preceded by BLOB PROGRESS events that carry
 * the LOAD's request_id.
 */
#define NYAMP_LLM_MAX_PATH NYAMP_INLINE_MAX

/* CHAT: a text-level completion.
 *
 * GENERATE takes token ids, which only works for a caller that owns the
 * tokenizer.  The control domain does not -- the tokenizer needs a 10 MB
 * vocabulary and a bit-exact chat template that live with the model -- so
 * CHAT carries an OpenAI chat-completions request as JSON and the compute
 * domain renders, tokenizes, runs and parses it.
 *
 * The request body uses the same framing idea as GENERATE: ordered chunks
 * that share one request_id, each acknowledged by a response, the last one
 * starting the run.  The parameters travel in every chunk but are taken from
 * the first, so a chunk is self-describing and a mismatched continuation is
 * detectable.
 *
 * After the final chunk is accepted the service emits, under that request_id:
 *
 *   EVENT_TOKEN   zero or more, only when STREAM_TOKENS was requested.  The
 *                 text keeps the model's structural tokens ("<function", ...)
 *                 because it is exactly what the parser will see.
 *   EVENT_RESULT  the chat-completions response JSON, in ordered chunks.
 *                 Sent only when the run produced one (status OK).
 *   EVENT_FINISH  always, exactly once, in the CHAT_FINISH layout below: the
 *                 status plus the token counts and timings.
 *
 * NYAMP_MODEL_PROMPT_TOO_LONG is its own status rather than INVALID because
 * it is the one failure the caller is expected to repair: the finish event
 * carries prompt_tokens and context_limit, so the caller can drop history
 * until prompt_tokens + max_new_tokens fits and retry.
 *
 * CANCEL works as for GENERATE: opcode CANCEL with the CHAT's request_id.
 */

#define NYAMP_LLM_CHAT                 5U
#define NYAMP_LLM_EVENT_RESULT         0x82U

#define NYAMP_LLM_CHAT_MAX_BODY        65536U
#define NYAMP_LLM_RESULT_MAX_BODY      65536U

#define NYAMP_LLM_CHAT_GUARD_UNTRUSTED (1U << 0)
#define NYAMP_LLM_CHAT_STREAM_TOKENS   (1U << 1)
#define NYAMP_LLM_CHAT_FLAGS_ALL \
  (NYAMP_LLM_CHAT_GUARD_UNTRUSTED | NYAMP_LLM_CHAT_STREAM_TOKENS)

/* total, offset, length, max_new_tokens, flags; then `length` bytes. */
#define NYAMP_LLM_CHAT_HEADER_SIZE 20U
#define NYAMP_LLM_CHAT_MAX_CHUNK \
  (NYAMP_INLINE_MAX - NYAMP_LLM_CHAT_HEADER_SIZE)

/* total, offset, length; then `length` bytes. */
#define NYAMP_LLM_RESULT_HEADER_SIZE 12U
#define NYAMP_LLM_RESULT_MAX_CHUNK \
  (NYAMP_INLINE_MAX - NYAMP_LLM_RESULT_HEADER_SIZE)

/* status, sequence, prompt_tokens, completion_tokens, prefill_ms, decode_ms,
 * context_limit.  A GENERATE still finishes with the 8-byte layout above; the
 * two are told apart by the opcode of the request they belong to.
 */
#define NYAMP_LLM_CHAT_FINISH_SIZE 28U

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
  NYAMP_FORMAT_F32 = 1,   /* Normalized mono PCM, [-1, 1].          */
  NYAMP_FORMAT_S16 = 2,   /* Signed 16-bit mono PCM.                */
  NYAMP_FORMAT_I64 = 3,   /* Phoneme and tone id vectors.           */
  NYAMP_FORMAT_UTF8 = 4,  /* Text.                                  */
  NYAMP_FORMAT_BYTES = 5, /* Opaque file bytes (blob windows).     */
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
 *
 * The grant BEGIN returns covers the whole capture slot and lives as long as
 * the request.  A PUSH names a sub-range of it (offset/length inside the
 * grant, the grant's lease) and the range may be rewritten as soon as the
 * response to that PUSH has arrived: the response is sent after the samples
 * were copied out, before they are decoded.  So the control domain is free to
 * use one window or to ping-pong between two, and no per-window RELEASE is
 * needed.  RELEASE returns the grant as a whole and is implied by the end of
 * the request (FINISH, CANCEL, UNLOAD).
 *
 * A request ends when the control domain says so: a PUSH whose descriptor
 * carries NYAMP_BUFFER_LAST (its length may be zero), or ASR_END.  The
 * service then flushes the decoder, sends the final text (PARTIAL_FINAL) and
 * EVENT_FINISH.  Endpointing is the control domain's decision; the decoder's
 * own opinion is only reported (PARTIAL_ENDPOINT).
 *
 * BEGIN with NYAMP_ASR_BEGIN_ATTACH_KWS is the second way to feed a request:
 * instead of being pushed audio it reads the stream the KWS service already
 * receives, from an absolute sample offset (see the KWS service below).  The
 * payload is then NYAMP_ASR_ATTACH_SIZE bytes, no grant is returned, PUSH is
 * refused, and the request ends with ASR_END naming the last sample.
 */

#define NYAMP_ASR_LOAD          1U
#define NYAMP_ASR_UNLOAD        2U
#define NYAMP_ASR_BEGIN         3U
#define NYAMP_ASR_PUSH          4U
#define NYAMP_ASR_RELEASE       5U
#define NYAMP_ASR_CANCEL        6U
#define NYAMP_ASR_END           7U
#define NYAMP_ASR_EVENT_PARTIAL 0x80U
#define NYAMP_ASR_EVENT_FINISH  0x81U

/* sample_rate, channels, flags, max_samples.  KWS_BEGIN has the same shape
 * with the window size in the last field, so one codec serves both.
 */
#define NYAMP_ASR_BEGIN_SIZE 16U

/* BEGIN flags (ASR and KWS).  Windows are float32 unless S16 is asked for;
 * S16 is what the capture device produces and halves the bytes that cross
 * the uncached region.
 */
#define NYAMP_AUDIO_BEGIN_S16      (1U << 0)
#define NYAMP_ASR_BEGIN_ATTACH_KWS (1U << 1)
#define NYAMP_AUDIO_BEGIN_ALL \
  (NYAMP_AUDIO_BEGIN_S16 | NYAMP_ASR_BEGIN_ATTACH_KWS)

/* The 16 bytes above followed by u64 start_sample. */
#define NYAMP_ASR_ATTACH_SIZE 24U

/* u64 end_sample.  NYAMP_STREAM_SAMPLE_NOW means "everything received so
 * far", which is also the only value a pushed (non-attached) request takes.
 */
#define NYAMP_ASR_END_SIZE      8U
#define NYAMP_STREAM_SAMPLE_NOW UINT64_MAX

/* EVENT_PARTIAL flags.  The receiver keeps one string: RESYNC replaces it
 * with the frame's text, anything else appends.  A replacement longer than
 * one frame is a RESYNC frame followed by appending frames.  Partials are
 * advisory and may be shed under back-pressure; the frame after a shed one
 * is always a RESYNC, and the final text is always sent as one.
 */
#define NYAMP_ASR_PARTIAL_RESYNC   (1U << 0)
#define NYAMP_ASR_PARTIAL_ENDPOINT (1U << 1) /* Decoder saw an endpoint. */
#define NYAMP_ASR_PARTIAL_FINAL    (1U << 2) /* Last text frame of request. */
#define NYAMP_ASR_PARTIAL_ALL                              \
  (NYAMP_ASR_PARTIAL_RESYNC | NYAMP_ASR_PARTIAL_ENDPOINT | \
   NYAMP_ASR_PARTIAL_FINAL)

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
 * SYNTH_TEXT takes UTF-8 text.  The compute domain owns the text front end
 * (tools/amp/g2p: normalisation, segmentation, grapheme-to-phoneme, sentence
 * splitting) because it needs the model's 7 MB lexicon, which lives with the
 * model.  The text arrives in ordered chunks that share one request_id, each
 * acknowledged by a response, the last one starting the run -- the framing
 * CHAT uses.  The parameters travel in every chunk and are taken from the
 * first.
 *
 * The vocoder has one fixed 512-frame bucket (about 5.94 s), so the service
 * splits the text into units that fit and synthesizes them one after the
 * other.  Audio is NEVER truncated: a unit the encoder still stretches past
 * the bucket is split again, and text that cannot be split ends the request
 * with NYAMP_MODEL_UNSUPPORTED rather than with a clipped sentence.
 *
 * SYNTH (phoneme and tone ids from the control domain) predates the front
 * end and is answered NYAMP_MODEL_UNSUPPORTED by the current service.
 *
 * The synthesized PCM always lands in the shared region because the smallest
 * useful result is already far past the inline limit; EVENT_PCM returns the
 * descriptor rather than the samples.  PCM is float32, mono, 44100 Hz.  One
 * window is outstanding at a time: the service writes a window, sends
 * EVENT_PCM, and writes the next one only after RELEASE echoed that window's
 * descriptor.  `valid_samples` is exact and `length` is valid_samples * 4;
 * nothing past it is meaningful.  NYAMP_BUFFER_RESYNC on a window marks the
 * first window of a new unit (a sentence boundary), NYAMP_BUFFER_LAST the
 * last window of the request.  EVENT_FINISH follows the RELEASE of the last
 * window, or a CANCEL at once; after a CANCEL the outstanding window is void
 * and must not be read any more.
 */

#define NYAMP_TTS_LOAD         1U
#define NYAMP_TTS_UNLOAD       2U
#define NYAMP_TTS_SYNTH        3U
#define NYAMP_TTS_RELEASE      4U
#define NYAMP_TTS_CANCEL       5U
#define NYAMP_TTS_SYNTH_TEXT   6U
#define NYAMP_TTS_EVENT_PCM    0x80U
#define NYAMP_TTS_EVENT_FINISH 0x81U

/* phoneme_count, speaker_id, speed (float bits), bucket_frames. */
#define NYAMP_TTS_SYNTH_SIZE 16U

/* total, offset, length, speaker_id, speed (float bits), window_samples,
 * flags; then `length` bytes of UTF-8.  window_samples = 0 selects the
 * service default (one second); flags are reserved and zero.
 */
#define NYAMP_TTS_TEXT_MAX_BODY    16384U
#define NYAMP_TTS_TEXT_HEADER_SIZE 28U
#define NYAMP_TTS_TEXT_MAX_CHUNK \
  (NYAMP_INLINE_MAX - NYAMP_TTS_TEXT_HEADER_SIZE)

#define NYAMP_TTS_SAMPLE_RATE        44100U
#define NYAMP_TTS_WINDOW_SAMPLES_MIN 4410U
#define NYAMP_TTS_WINDOW_SAMPLES_MAX 262144U /* The whole 1 MiB slot. */

/* buffer descriptor, sequence, sample_rate, channels, valid_samples. */
#define NYAMP_TTS_PCM_HEADER_SIZE (NYAMP_BUFFER_SIZE + 16U)

/* status, sequence, total_samples. */
#define NYAMP_TTS_FINISH_SIZE 12U

#define NYAMP_TTS_MAX_PATH    NYAMP_INLINE_MAX

/* KWS service opcodes (NYAMP_SERVICE_KWS): the wake word listener.
 *
 * An ASR request is one utterance; a wake word listener has no utterances
 * and must hear everything, including the moments when no ASR request
 * exists.  So the always-on capture stream belongs to this service:
 *
 *  - BEGIN returns the grant for the capture slot and starts the stream.
 *    PUSH submits one window of it under the ASR_PUSH rules (a sub-range of
 *    the grant, reusable once the response has arrived).  END stops it.
 *  - Every sample has an absolute position, `stream_sample`: a u64 count of
 *    samples since BEGIN.  At 16 kHz it does not wrap in the life of the
 *    device.  A PUSH carries the position of its first sample; one that is
 *    not the position the service expects (a window was lost, capture was
 *    paused while the robot spoke) is a DISCONTINUITY, not an error: the
 *    decoder state is dropped and counting continues from the new position.
 *    The PUSH response returns the position the service expects next.
 *  - The compute domain keeps the last NYAMP_KWS_RING_SECONDS of the stream
 *    in its own memory.  An ASR request can attach to it from a sample
 *    offset (ASR_BEGIN_ATTACH_KWS), normally `end_sample` of the DETECTED
 *    event, so the command that follows the wake word is neither lost nor
 *    sent twice.  The ring belongs to the stream: END, UNLOAD, a CANCEL of
 *    the BEGIN, or a new generation close it, and an attached ASR request
 *    then finishes with the audio it got, as if ASR_END had named that point.
 *    A reader that falls more than the ring behind loses the overwritten
 *    samples and carries on from the oldest one still held.
 *
 * LOAD names the model directory (encoder.onnx, decoder.onnx, joiner.onnx,
 * tokens.txt) and the keywords file inside it.  Every keywords line ends in
 * "@label"; the keyword id is the zero-based index of the label among the
 * distinct labels in file order, so pronunciation variants share an id.  A
 * zero parameter selects the evaluated default.
 *
 * Events carry the request_id of the BEGIN.  EVENT_FINISH (the ASR_FINISH
 * layout) is sent once when the stream ends.
 */

#define NYAMP_KWS_LOAD               1U
#define NYAMP_KWS_UNLOAD             2U
#define NYAMP_KWS_BEGIN              3U
#define NYAMP_KWS_PUSH               4U
#define NYAMP_KWS_END                6U
#define NYAMP_KWS_LIST               7U
#define NYAMP_KWS_EVENT_DETECTED     0x80U
#define NYAMP_KWS_EVENT_FINISH       0x81U

#define NYAMP_KWS_RING_SECONDS       10U
#define NYAMP_KWS_WINDOW_SAMPLES_MIN 1600U
#define NYAMP_KWS_WINDOW_SAMPLES_MAX 16000U

/* threshold, score (float bits), max_active_paths, num_trailing_blanks,
 * directory length, keywords file length; then the two names.  An empty
 * keywords name means "keywords.txt".
 */
#define NYAMP_KWS_LOAD_HEADER_SIZE 16U

/* sample_rate, channels, flags, window_samples: the ASR_BEGIN layout. */
#define NYAMP_KWS_BEGIN_SIZE NYAMP_ASR_BEGIN_SIZE

/* buffer descriptor, sequence, flags, reserved, stream_sample. */
#define NYAMP_KWS_PUSH_SIZE          (NYAMP_BUFFER_SIZE + 16U)
#define NYAMP_KWS_PUSH_DISCONTINUITY (1U << 0)

/* PUSH response body: u64 next expected stream_sample. */
#define NYAMP_KWS_PUSH_ACK_SIZE 8U

/* sequence, keyword_id, flags, score (float bits), label length,
 * start_sample, end_sample, trigger_sample; then the label.
 */
#define NYAMP_KWS_DETECTED_HEADER_SIZE 40U
#define NYAMP_KWS_MAX_LABEL            (NYAMP_INLINE_MAX - NYAMP_KWS_DETECTED_HEADER_SIZE)
#define NYAMP_KWS_DETECTED_HAS_OFFSETS (1U << 0)
#define NYAMP_KWS_DETECTED_HAS_SCORE   (1U << 1)
#define NYAMP_KWS_KEYWORD_UNKNOWN      0xffffU

/* LIST response body: count, reserved; then per label u16 length + UTF-8. */
#define NYAMP_KWS_LABELS_HEADER_SIZE 4U

/* Response status prefix.
 *
 * Every response payload starts with one little-endian i32 status (a
 * nyamp_model_status_e value) followed by an opcode-specific body.  The rule
 * predates this helper -- HEALTH and LLM already follow it -- and is named
 * here so both responders encode it the same way.
 */

#define NYAMP_STATUS_SIZE 4U

/* Request-id ownership.
 *
 * A request_id is scoped to the domain that ORIGINATED the request; the
 * responder only echoes it.  Both domains originate requests (the control
 * domain for HEALTH/LLM/ASR/TTS, the compute domain for BLOB) and the two
 * allocators never talk to each other, so the id alone is not unique on the
 * endpoint.  The message kind disambiguates it: a RESPONSE or EVENT a domain
 * receives can only belong to a request that domain sent, and a REQUEST or
 * CANCEL it receives can only come from the peer.
 *
 * Compute-originated ids additionally set the top bit.  That is not needed
 * for correctness; it keeps a captured trace unambiguous and lets a responder
 * refuse a request that claims the wrong origin.  Control-originated ids are
 * (pid << 32 | milliseconds) and never reach the top bit.
 */

#define NYAMP_REQUEST_ID_COMPUTE (1ULL << 63)

/* BLOB service opcodes (NYAMP_SERVICE_BLOB).
 *
 * This is the one service whose REQUESTER is the compute domain and whose
 * RESPONDER is the control domain.  The control domain owns the eMMC and the
 * /data volume; the compute domain has no storage at all and pulls model
 * files on demand.  Everything else keeps its meaning:
 *
 *  - generation is still the compute domain's.  The control domain has no
 *    generation of its own: it learns the current one from the READY event
 *    (or a HEALTH response) and a BLOB request must carry exactly that value.
 *    A request with another generation is answered STALE_GENERATION, and a
 *    new generation voids every open blob, because the compute domain that
 *    held them no longer exists.  Responses echo the request's generation.
 *  - the compute domain remains the only allocator of the shared arena.  READ
 *    carries a window it minted (an NYBS descriptor with its lease); the
 *    control domain fills that window and echoes the descriptor back with
 *    `length` set to the bytes it wrote.  It never picks an offset itself.
 *  - a CANCEL-kind message whose request_id names an outstanding OPEN aborts
 *    it.  OPEN may take many seconds (the digest of a large file is computed
 *    off the receive thread), so a requester that gives up must say so or the
 *    blob would stay open until the next generation.
 *
 * Names are UTF-8, relative to the blob root (/data/models), '/' separated,
 * and must not contain an empty, "." or ".." component, a leading '/', a
 * backslash or a control character.
 *
 * BENCH is answered by the control domain: mode ECHO returns the request
 * body untouched (round-trip time), mode FILL writes a seed-derived pattern
 * into the granted window (shared-memory throughput without the eMMC).
 *
 * BENCH_RUN and PULL travel in the usual direction -- control to compute --
 * and ask the compute domain to run a measurement or a pull and report the
 * outcome.  They exist because the compute domain has no console: without
 * them the delivery path could only be exercised as a side effect of loading
 * a model.  PROGRESS events follow an accepted PULL under its request_id.
 */

#define NYAMP_BLOB_OPEN           1U
#define NYAMP_BLOB_READ           2U
#define NYAMP_BLOB_CLOSE          3U
#define NYAMP_BLOB_LIST           4U
#define NYAMP_BLOB_BENCH          5U
#define NYAMP_BLOB_BENCH_RUN      0x10U
#define NYAMP_BLOB_PULL           0x11U
#define NYAMP_BLOB_EVENT_PROGRESS 0x80U

#define NYAMP_BLOB_MAX_NAME       255U
#define NYAMP_BLOB_SHA256_SIZE    32U

/* flags, name length, then the name. */
#define NYAMP_BLOB_OPEN_HEADER_SIZE 8U

/* blob_id, flags, size, mtime, sha256. */
#define NYAMP_BLOB_INFO_SIZE (24U + NYAMP_BLOB_SHA256_SIZE)

/* blob_id, flags, file_offset, buffer descriptor.  Request and response share
 * the layout: the response echoes the descriptor with `length` = bytes filled.
 */
#define NYAMP_BLOB_READ_SIZE (16U + NYAMP_BUFFER_SIZE)

/* Set in a READ response when the window reaches end of file. */
#define NYAMP_BLOB_READ_EOF (1U << 0)

/* blob_id, reserved. */
#define NYAMP_BLOB_CLOSE_SIZE 8U

/* cursor, prefix length, then the prefix (empty = the blob root). */
#define NYAMP_BLOB_LIST_HEADER_SIZE 8U

/* next_cursor, entry count; then per entry: size u64, flags u16, name length
 * u16, name.  The cursor is opaque to the requester: zero starts a listing
 * and a zero next_cursor ends it.
 */
#define NYAMP_BLOB_LIST_BODY_HEADER_SIZE  8U
#define NYAMP_BLOB_LIST_ENTRY_HEADER_SIZE 12U
#define NYAMP_BLOB_LIST_BODY_MAX          (NYAMP_INLINE_MAX - NYAMP_STATUS_SIZE)

#define NYAMP_BLOB_ENTRY_DIRECTORY        (1U << 0)

/* mode, seed; FILL appends a buffer descriptor. */
#define NYAMP_BLOB_BENCH_ECHO      0U
#define NYAMP_BLOB_BENCH_FILL      1U
#define NYAMP_BLOB_BENCH_ECHO_SIZE 8U
#define NYAMP_BLOB_BENCH_FILL_SIZE (8U + NYAMP_BUFFER_SIZE)

/* rounds, window bytes. */
#define NYAMP_BLOB_BENCH_RUN_SIZE 8U

/* rounds, window bytes, rtt min/avg/max (us), fill KiB/s, copy KiB/s,
 * pattern errors.
 */
#define NYAMP_BLOB_BENCH_REPORT_SIZE 32U

/* bytes, elapsed ms, files, reused files. */
#define NYAMP_BLOB_PULL_REPORT_SIZE 24U

/* done, total, bytes per second, reserved. */
#define NYAMP_BLOB_PROGRESS_SIZE 24U

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
  NYAMP_SERVICE_BLOB = 9, /* Requester: compute.  Responder: control. */
  NYAMP_SERVICE_KWS = 10,

  /* Reserved for the owner-voiceprint service proposed in
   * tools/amp/voice/PROTOCOL.md; nothing answers it yet.
   */
  NYAMP_SERVICE_SPEAKER = 11,
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

  /* CHAT only: the rendered prompt plus max_new_tokens exceeds the context
   * window.  It has no counterpart in nyamp::models::Status because the model
   * layer never sees such a request; the service refuses it first.
   */
  NYAMP_MODEL_PROMPT_TOO_LONG = -11,
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

struct nyamp_llm_chat_s
{
  uint32_t total;          /* Bytes in the whole request body.        */
  uint32_t offset;         /* Position of this chunk within it.       */
  uint32_t length;         /* Bytes carried by this chunk.            */
  uint32_t max_new_tokens; /* Zero selects the service default.       */
  uint32_t flags;          /* NYAMP_LLM_CHAT_*.                       */
};

struct nyamp_llm_result_s
{
  uint32_t total;
  uint32_t offset;
  uint32_t length;
};

struct nyamp_llm_chat_finish_s
{
  int32_t status; /* nyamp_model_status_e. */
  uint32_t sequence;
  uint32_t prompt_tokens;
  uint32_t completion_tokens;
  uint32_t prefill_ms;    /* Request accepted to first token.        */
  uint32_t decode_ms;     /* First token to last token.              */
  uint32_t context_limit; /* What prompt + max_new_tokens must fit.  */
};

struct nyamp_tts_text_s
{
  uint32_t total;  /* Bytes in the whole text.                 */
  uint32_t offset; /* Position of this chunk within it.        */
  uint32_t length; /* Bytes carried by this chunk.             */
  uint32_t speaker_id;
  float speed;             /* 1.0 = the model's own pace.              */
  uint32_t window_samples; /* Zero selects the service default.        */
  uint32_t flags;          /* Reserved, zero.                          */
};

struct nyamp_kws_load_s
{
  float threshold;              /* Zero selects the default.           */
  float score;                  /* Keyword boost; zero = default.      */
  uint16_t max_active_paths;    /* Zero = default.                     */
  uint16_t num_trailing_blanks; /* Zero = default.                     */
  uint16_t directory_length;
  uint16_t keywords_length;
  const char *directory; /* Points into the payload; no NUL.           */
  const char *keywords;  /* May be empty: "keywords.txt".              */
};

struct nyamp_kws_push_s
{
  struct nyamp_buffer_s buffer;
  uint32_t sequence; /* +1 per window of this stream.                  */
  uint16_t flags;    /* NYAMP_KWS_PUSH_*.                              */
  uint64_t stream_sample;
};

struct nyamp_kws_detected_s
{
  uint32_t sequence;   /* +1 per event of this stream.                 */
  uint16_t keyword_id; /* NYAMP_KWS_KEYWORD_UNKNOWN if not listed.     */
  uint16_t flags;      /* NYAMP_KWS_DETECTED_*.                        */
  float score;         /* Valid only with HAS_SCORE.                   */
  uint32_t label_length;
  uint64_t start_sample;   /* First token of the phrase (HAS_OFFSETS). */
  uint64_t end_sample;     /* One frame past the last token.           */
  uint64_t trigger_sample; /* Stream position at the trigger; valid.   */
  const char *label;       /* Points into the payload; no NUL.         */
};

struct nyamp_blob_info_s
{
  uint32_t blob_id; /* Valid until CLOSE or the next generation.   */
  uint32_t flags;   /* Reserved, zero.                              */
  uint64_t size;
  uint64_t mtime; /* Seconds since the Unix epoch, 0 if unknown.  */
  uint8_t sha256[NYAMP_BLOB_SHA256_SIZE];
};

struct nyamp_blob_read_s
{
  uint32_t blob_id;
  uint32_t flags; /* Request: zero.  Response: NYAMP_BLOB_READ_EOF. */
  uint64_t file_offset;
  struct nyamp_buffer_s buffer; /* Response: length = bytes filled.  */
};

struct nyamp_blob_entry_s
{
  uint64_t size;
  uint16_t flags;
  uint16_t name_length;
  const char *name; /* Points into the payload; not NUL terminated. */
};

struct nyamp_blob_bench_s
{
  uint32_t mode;
  uint32_t seed;
  struct nyamp_buffer_s buffer; /* Meaningful only for FILL. */
};

struct nyamp_blob_bench_report_s
{
  uint32_t rounds;
  uint32_t window_bytes;
  uint32_t rtt_min_us;
  uint32_t rtt_avg_us;
  uint32_t rtt_max_us;
  uint32_t fill_kib_per_s; /* Control domain writes the window.   */
  uint32_t copy_kib_per_s; /* Compute domain copies it back out.  */
  uint32_t pattern_errors;
};

struct nyamp_blob_pull_report_s
{
  uint64_t bytes;
  uint64_t elapsed_ms;
  uint32_t files;
  uint32_t reused;
};

struct nyamp_blob_progress_s
{
  uint64_t done;
  uint64_t total;
  uint32_t bytes_per_second;
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

/* The chunk codecs return a pointer into the payload rather than copying:
 * the caller appends the bytes to its own reassembly buffer anyway.
 */

int nyamp_llm_chat_encode(uint8_t *payload, size_t payload_capacity,
                          size_t *payload_size,
                          const struct nyamp_llm_chat_s *chunk,
                          const uint8_t *bytes);
int nyamp_llm_chat_decode(struct nyamp_llm_chat_s *chunk,
                          const uint8_t **bytes, const uint8_t *payload,
                          size_t payload_size);

int nyamp_llm_result_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size,
                            const struct nyamp_llm_result_s *chunk,
                            const uint8_t *bytes);
int nyamp_llm_result_decode(struct nyamp_llm_result_s *chunk,
                            const uint8_t **bytes, const uint8_t *payload,
                            size_t payload_size);

int nyamp_llm_chat_finish_encode(uint8_t *payload, size_t payload_capacity,
                                 size_t *payload_size,
                                 const struct nyamp_llm_chat_finish_s *finish);
int nyamp_llm_chat_finish_decode(struct nyamp_llm_chat_finish_s *finish,
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

/* BEGIN with NYAMP_ASR_BEGIN_ATTACH_KWS: the BEGIN fields plus the stream
 * position recognition starts at.  The encoder sets the flag itself; the
 * decoder refuses a payload without it.
 */

int nyamp_asr_attach_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, uint32_t sample_rate,
                            uint16_t channels, uint16_t flags,
                            uint32_t max_samples, uint64_t start_sample);
int nyamp_asr_attach_decode(uint32_t *sample_rate, uint16_t *channels,
                            uint16_t *flags, uint32_t *max_samples,
                            uint64_t *start_sample, const uint8_t *payload,
                            size_t payload_size);

int nyamp_asr_end_encode(uint8_t *payload, size_t payload_capacity,
                         size_t *payload_size, uint64_t end_sample);
int nyamp_asr_end_decode(uint64_t *end_sample, const uint8_t *payload,
                         size_t payload_size);

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

/* A chunk of SYNTH_TEXT.  The decoder returns a pointer into the payload,
 * as the chat codec does.  A chunk may end inside a UTF-8 sequence; the
 * text is validated once it is whole.
 */

int nyamp_tts_text_encode(uint8_t *payload, size_t payload_capacity,
                          size_t *payload_size,
                          const struct nyamp_tts_text_s *chunk,
                          const uint8_t *bytes);
int nyamp_tts_text_decode(struct nyamp_tts_text_s *chunk,
                          const uint8_t **bytes, const uint8_t *payload,
                          size_t payload_size);

/* KWS.  BEGIN and EVENT_FINISH reuse the ASR layouts. */

#define nyamp_kws_begin_encode  nyamp_asr_begin_encode
#define nyamp_kws_begin_decode  nyamp_asr_begin_decode
#define nyamp_kws_finish_encode nyamp_asr_finish_encode
#define nyamp_kws_finish_decode nyamp_asr_finish_decode

int nyamp_kws_load_encode(uint8_t *payload, size_t payload_capacity,
                          size_t *payload_size,
                          const struct nyamp_kws_load_s *load);
int nyamp_kws_load_decode(struct nyamp_kws_load_s *load,
                          const uint8_t *payload, size_t payload_size);

int nyamp_kws_push_encode(uint8_t *payload, size_t payload_capacity,
                          size_t *payload_size,
                          const struct nyamp_kws_push_s *push);
int nyamp_kws_push_decode(struct nyamp_kws_push_s *push,
                          const uint8_t *payload, size_t payload_size);

int nyamp_kws_push_ack_encode(uint8_t *body, size_t body_capacity,
                              size_t *body_size, uint64_t next_sample);
int nyamp_kws_push_ack_decode(uint64_t *next_sample, const uint8_t *body,
                              size_t body_size);

int nyamp_kws_detected_encode(uint8_t *payload, size_t payload_capacity,
                              size_t *payload_size,
                              const struct nyamp_kws_detected_s *detected);
int nyamp_kws_detected_decode(struct nyamp_kws_detected_s *detected,
                              const uint8_t *payload, size_t payload_size);

/* LIST response body.  begin, append until NYAMP_EMSGSIZE, then the reader
 * validates with _decode and walks with _next from position 0.
 */

int nyamp_kws_labels_begin(uint8_t *body, size_t body_capacity,
                           size_t *body_size);
int nyamp_kws_labels_append(uint8_t *body, size_t body_capacity,
                            size_t *body_size, const char *label,
                            size_t label_length);
int nyamp_kws_labels_decode(uint16_t *count, const uint8_t *body,
                            size_t body_size);
int nyamp_kws_labels_next(const char **label, uint16_t *label_length,
                          size_t *position, const uint8_t *body,
                          size_t body_size);

/* Response payload = status followed by a body.  The encoder returns where
 * the body must be written; the decoder returns where it starts.
 */

int nyamp_status_encode(uint8_t *payload, size_t payload_capacity,
                        int32_t status, uint8_t **body, size_t *body_capacity);
int nyamp_status_decode(int32_t *status, const uint8_t **body,
                        size_t *body_size, const uint8_t *payload,
                        size_t payload_size);

/* NYAMP_OK when `name` is a legal blob name (see the rules above). */

int nyamp_blob_name_check(const char *name, size_t name_length);

int nyamp_blob_open_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t flags,
                           const char *name, size_t name_length);
int nyamp_blob_open_decode(uint32_t *flags, const char **name,
                           size_t *name_length, const uint8_t *payload,
                           size_t payload_size);

int nyamp_blob_info_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size,
                           const struct nyamp_blob_info_s *info);
int nyamp_blob_info_decode(struct nyamp_blob_info_s *info,
                           const uint8_t *payload, size_t payload_size);

int nyamp_blob_read_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size,
                           const struct nyamp_blob_read_s *read);
int nyamp_blob_read_decode(struct nyamp_blob_read_s *read,
                           const uint8_t *payload, size_t payload_size);

int nyamp_blob_close_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size, uint32_t blob_id);
int nyamp_blob_close_decode(uint32_t *blob_id, const uint8_t *payload,
                            size_t payload_size);

int nyamp_blob_list_encode(uint8_t *payload, size_t payload_capacity,
                           size_t *payload_size, uint32_t cursor,
                           const char *prefix, size_t prefix_length);
int nyamp_blob_list_decode(uint32_t *cursor, const char **prefix,
                           size_t *prefix_length, const uint8_t *payload,
                           size_t payload_size);

/* A LIST response body is built incrementally: begin, append until
 * NYAMP_EMSGSIZE says the next entry does not fit, then finish.  The reader
 * validates it with nyamp_blob_list_body_decode and walks it with
 * nyamp_blob_list_body_next, starting from position 0.
 */

int nyamp_blob_list_body_begin(uint8_t *body, size_t body_capacity,
                               size_t *body_size);
int nyamp_blob_list_body_append(uint8_t *body, size_t body_capacity,
                                size_t *body_size,
                                const struct nyamp_blob_entry_s *entry);
int nyamp_blob_list_body_finish(uint8_t *body, size_t body_size,
                                uint32_t next_cursor);
int nyamp_blob_list_body_decode(uint32_t *next_cursor, uint32_t *count,
                                const uint8_t *body, size_t body_size);
int nyamp_blob_list_body_next(struct nyamp_blob_entry_s *entry,
                              size_t *position, const uint8_t *body,
                              size_t body_size);

int nyamp_blob_bench_encode(uint8_t *payload, size_t payload_capacity,
                            size_t *payload_size,
                            const struct nyamp_blob_bench_s *bench);
int nyamp_blob_bench_decode(struct nyamp_blob_bench_s *bench,
                            const uint8_t *payload, size_t payload_size);

/* The FILL pattern: little-endian u32 word k of the window holds
 * seed ^ (k * 0x9e3779b1).  It depends on the position so a window that is
 * merely shifted or stale fails the comparison, and `first_word` lets either
 * side produce it block by block into ordinary cached memory.
 */

void nyamp_blob_bench_pattern(uint8_t *dest, size_t words, uint32_t seed,
                              uint32_t first_word);

int nyamp_blob_bench_run_encode(uint8_t *payload, size_t payload_capacity,
                                size_t *payload_size, uint32_t rounds,
                                uint32_t window_bytes);
int nyamp_blob_bench_run_decode(uint32_t *rounds, uint32_t *window_bytes,
                                const uint8_t *payload, size_t payload_size);

int nyamp_blob_bench_report_encode(
    uint8_t *payload, size_t payload_capacity, size_t *payload_size,
    const struct nyamp_blob_bench_report_s *report);
int nyamp_blob_bench_report_decode(struct nyamp_blob_bench_report_s *report,
                                   const uint8_t *payload,
                                   size_t payload_size);

int nyamp_blob_pull_report_encode(
    uint8_t *payload, size_t payload_capacity, size_t *payload_size,
    const struct nyamp_blob_pull_report_s *report);
int nyamp_blob_pull_report_decode(struct nyamp_blob_pull_report_s *report,
                                  const uint8_t *payload, size_t payload_size);

int nyamp_blob_progress_encode(uint8_t *payload, size_t payload_capacity,
                               size_t *payload_size,
                               const struct nyamp_blob_progress_s *progress);
int nyamp_blob_progress_decode(struct nyamp_blob_progress_s *progress,
                               const uint8_t *payload, size_t payload_size);

#ifdef __cplusplus
}
#endif

#endif /* __TOOLS_AMP_PROTOCOL_NYAMP_PROTOCOL_H */
