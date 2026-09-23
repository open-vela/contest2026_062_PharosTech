/****************************************************************************
 * tools/amp/nyampd/nyampd_core.cpp
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

#include "nyampd_core.h"

#include "nyamp_protocol.h"
#include "nyampd_asr.h"
#include "nyampd_kws.h"
#include "nyampd_llm.h"
#include "nyampd_provision.h"
#include "nyampd_tts.h"

#include <cstring>
#include <string>

namespace nyamp
{
namespace
{

constexpr std::size_t kStatusPayloadSize = 4;
constexpr std::size_t kHealthPayloadSize = 12;
constexpr std::uint32_t kCapabilityHealth = 1U << 0;
constexpr std::uint32_t kCapabilityLlm = 1U << 1;
constexpr std::uint32_t kCapabilityBlob = 1U << 2;
constexpr std::uint32_t kCapabilityChat = 1U << 3;

/* Unlike the LLM bit these say a backend exists, not merely that the
 * service object does: a daemon built without the speech runtimes leaves
 * them clear and answers the opcodes unsupported.
 */

constexpr std::uint32_t kCapabilityAsr = 1U << 4;
constexpr std::uint32_t kCapabilityTts = 1U << 5;
constexpr std::uint32_t kCapabilityKws = 1U << 6;

void PutLe32(std::uint8_t *dest, std::uint32_t value)
{
  for (unsigned int index = 0; index < 4; ++index)
    {
      dest[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

/****************************************************************************
 * Name: ModelStatusCode
 *
 * Description:
 *   Convert a model-layer status to its wire value.  The C++ enum counts up
 *   from zero while the wire enum counts down from zero, so the sign is the
 *   translation: a positive model status becomes a negative wire status and
 *   stays disjoint from nyamp_result_e.
 *
 ****************************************************************************/

std::int32_t ModelStatus(models::Status status)
{
  return -static_cast<std::int32_t>(status);
}

/****************************************************************************
 * Name: EncodeResponse
 *
 * Description:
 *   Write a response frame.  The payload is a model status followed by an
 *   optional body.  HEALTH keeps its dedicated layout so existing clients stay
 *   compatible; it carries the live capability mask, which is how a client
 *   learns whether the LLM service is actually available.
 *
 ****************************************************************************/

int EncodeResponse(const nyamp_header_s &request, std::int32_t status,
                   std::uint32_t generation, std::uint32_t capabilities,
                   const std::uint8_t *body, std::size_t body_size,
                   std::uint8_t *response, std::size_t response_capacity,
                   std::size_t *response_size)
{
  if (response_size == nullptr)
    {
      return NYAMP_EINVAL;
    }

  const bool health = request.service == NYAMP_SERVICE_HEALTH &&
                      request.opcode == kHealthQuery;
  std::size_t payload_size;
  if (health)
    {
      payload_size = kHealthPayloadSize;
    }
  else
    {
      if (body_size > NYAMP_INLINE_MAX - kStatusPayloadSize)
        {
          return NYAMP_EMSGSIZE;
        }

      payload_size = kStatusPayloadSize + body_size;
    }

  nyamp_header_s header = {
    request.service,
    request.opcode,
    NYAMP_FLAG_RESPONSE | (status == 0 ? 0U : NYAMP_FLAG_ERROR),
    request.request_id,
    request.deadline_ms,
    generation,
    static_cast<std::uint32_t>(payload_size),
  };

  const int result = nyamp_header_encode(response, response_capacity, &header);
  if (result != NYAMP_OK)
    {
      return result;
    }

  std::uint8_t *payload = response + NYAMP_WIRE_HEADER_SIZE;
  PutLe32(payload, static_cast<std::uint32_t>(status));
  if (health)
    {
      PutLe32(payload + 4, generation);
      PutLe32(payload + 8, capabilities);
    }
  else if (body_size != 0)
    {
      std::memcpy(payload + kStatusPayloadSize, body, body_size);
    }

  *response_size = NYAMP_WIRE_HEADER_SIZE + payload_size;
  return NYAMP_OK;
}

/****************************************************************************
 * Name: DispatchLlm
 *
 * Description:
 *   Route an LLM service request.  Every opcode with no implementation behind
 *   it answers unsupported; nothing here reports success it did not achieve.
 *
 ****************************************************************************/

int DispatchLlm(const nyamp_header_s &request, LlmService *llm,
                std::uint32_t generation, std::uint32_t capabilities,
                const uint8_t *payload, std::uint8_t *response,
                std::size_t response_capacity, std::size_t *response_size)
{
  std::int32_t status = NYAMP_MODEL_UNSUPPORTED;
  std::uint8_t body[NYAMP_INLINE_MAX];
  std::size_t body_size = 0;

  if (llm == nullptr)
    {
      return EncodeResponse(request, status, generation, 0, nullptr, 0,
                            response, response_capacity, response_size);
    }

  switch (request.opcode)
    {
      case NYAMP_LLM_LOAD:
        {
          if (request.payload_size == 0 ||
              request.payload_size > NYAMP_LLM_MAX_PATH)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          const std::string directory(reinterpret_cast<const char *>(payload),
                                      request.payload_size);
          bool deferred = false;
          status = ModelStatus(llm->BeginLoad(directory, request, &deferred));
          if (deferred)
            {
              /* The model is being pulled from the control domain; the
               * response follows through the service queue.
               */
              *response_size = 0;
              return NYAMP_OK;
            }

          break;
        }

      case NYAMP_LLM_UNLOAD:
        {
          if (request.payload_size != 0)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          status = ModelStatus(llm->Unload());
          break;
        }

      case NYAMP_LLM_GENERATE:
        {
          nyamp_llm_chunk_s chunk{};
          std::int32_t ids[NYAMP_LLM_MAX_CHUNK_IDS];
          std::size_t count = 0;
          bool started = false;

          if (nyamp_llm_chunk_decode(&chunk, ids, NYAMP_LLM_MAX_CHUNK_IDS,
                                     &count, payload,
                                     request.payload_size) != NYAMP_OK)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          status = ModelStatus(
              llm->BeginGenerate(chunk, ids, count, request.request_id,
                                 request.deadline_ms, &started));

          /* The response only acknowledges the chunk; tokens and the terminal
           * finish arrive later as events tied to request_id.
           */
          break;
        }

      case NYAMP_LLM_CHAT:
        {
          nyamp_llm_chat_s chunk{};
          const std::uint8_t *bytes = nullptr;
          bool started = false;

          if (nyamp_llm_chat_decode(&chunk, &bytes, payload,
                                    request.payload_size) != NYAMP_OK)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          /* Already a wire status: chat can fail in a way the model layer
           * has no name for.  The response acknowledges the chunk; the
           * result and the terminal finish arrive as events.
           */
          status = llm->BeginChat(chunk, bytes, request.request_id,
                                  request.deadline_ms, &started);
          break;
        }

      case NYAMP_LLM_CANCEL:
        {
          if (request.payload_size != 0)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          status = ModelStatus(llm->Cancel(request.request_id));
          break;
        }

      default:
        status = NYAMP_MODEL_UNSUPPORTED;
        break;
    }

  return EncodeResponse(request, status, generation, capabilities, body,
                        body_size, response, response_capacity, response_size);
}

/****************************************************************************
 * Name: DispatchBlob
 *
 * Description:
 *   The BLOB service as seen from this side.  OPEN/READ/CLOSE/LIST/BENCH are
 *   requests this domain SENDS; receiving one means the peer has the
 *   direction backwards, and it is refused rather than served.  Only
 *   BENCH_RUN and PULL arrive here, and both answer later from a worker.
 *
 ****************************************************************************/

int DispatchBlob(const nyamp_header_s &request, BlobService *blob,
                 std::uint32_t generation, std::uint32_t capabilities,
                 const uint8_t *payload, std::uint8_t *response,
                 std::size_t response_capacity, std::size_t *response_size)
{
  std::int32_t status = NYAMP_MODEL_UNSUPPORTED;

  if (blob != nullptr && request.opcode == NYAMP_BLOB_BENCH_RUN)
    {
      std::uint32_t rounds = 0;
      std::uint32_t window_bytes = 0;

      status = nyamp_blob_bench_run_decode(&rounds, &window_bytes, payload,
                                           request.payload_size) == NYAMP_OK
                   ? blob->BeginBench(request, rounds, window_bytes)
                   : NYAMP_MODEL_INVALID;
    }
  else if (blob != nullptr && request.opcode == NYAMP_BLOB_PULL)
    {
      std::uint32_t flags = 0;
      const char *name = nullptr;
      std::size_t name_length = 0;

      status = nyamp_blob_open_decode(&flags, &name, &name_length, payload,
                                      request.payload_size) == NYAMP_OK
                   ? blob->BeginPull(request, std::string(name, name_length))
                   : NYAMP_MODEL_INVALID;
    }

  if (status == NYAMP_MODEL_OK)
    {
      *response_size = 0;
      return NYAMP_OK;
    }

  return EncodeResponse(request, status, generation, capabilities, nullptr, 0,
                        response, response_capacity, response_size);
}

/****************************************************************************
 * Name: DispatchAsr
 *
 * Description:
 *   Route an ASR request.  BEGIN comes in two sizes: the 16-byte form is fed
 *   by PUSH and its response carries the grant; the 24-byte form attaches to
 *   the wake word stream and its response carries nothing.  Every other
 *   opcode names its request through the header's request_id.
 *
 ****************************************************************************/

int DispatchAsr(const nyamp_header_s &request, AsrService *asr,
                std::uint32_t generation, const uint8_t *payload,
                std::uint8_t *response, std::size_t response_capacity,
                std::size_t *response_size)
{
  std::int32_t status = NYAMP_MODEL_UNSUPPORTED;
  std::uint8_t body[NYAMP_INLINE_MAX];
  std::size_t body_size = 0;

  if (asr == nullptr)
    {
      return EncodeResponse(request, status, generation, 0, nullptr, 0,
                            response, response_capacity, response_size);
    }

  switch (request.opcode)
    {
      case NYAMP_ASR_LOAD:
        {
          if (request.payload_size == 0 ||
              request.payload_size > NYAMP_ASR_MAX_PATH)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          bool deferred = false;
          status = ModelStatus(asr->BeginLoad(
              std::string(reinterpret_cast<const char *>(payload),
                          request.payload_size),
              request, &deferred));
          if (deferred)
            {
              *response_size = 0;
              return NYAMP_OK;
            }

          break;
        }

      case NYAMP_ASR_UNLOAD:
        status = request.payload_size != 0 ? NYAMP_MODEL_INVALID
                                           : ModelStatus(asr->Unload());
        break;

      case NYAMP_ASR_BEGIN:
        {
          std::uint32_t sample_rate = 0;
          std::uint16_t channels = 0;
          std::uint16_t flags = 0;
          std::uint32_t max_samples = 0;
          std::uint64_t start_sample = 0;
          const bool attach = request.payload_size == NYAMP_ASR_ATTACH_SIZE;
          nyamp_buffer_s grant{};
          bool has_grant = false;

          const int decoded =
              attach ? nyamp_asr_attach_decode(&sample_rate, &channels, &flags,
                                               &max_samples, &start_sample,
                                               payload, request.payload_size)
                     : nyamp_asr_begin_decode(&sample_rate, &channels, &flags,
                                              &max_samples, payload,
                                              request.payload_size);
          if (decoded != NYAMP_OK)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          status = asr->Begin(request, sample_rate, channels, flags,
                              max_samples, attach, start_sample, &grant,
                              &has_grant);
          if (status == NYAMP_MODEL_OK && has_grant &&
              nyamp_buffer_encode(body, sizeof(body), &body_size, &grant) !=
                  NYAMP_OK)
            {
              status = NYAMP_MODEL_BACKEND_ERROR;
              body_size = 0;
            }

          break;
        }

      case NYAMP_ASR_PUSH:
        {
          nyamp_buffer_s window{};
          std::uint32_t sequence = 0;
          std::uint16_t flags = 0;
          std::uint32_t total = 0;
          std::uint32_t consumed = 0;

          /* total/consumed are the producer's bookkeeping; the service
           * counts for itself.
           */
          status = nyamp_asr_push_decode(&window, &sequence, &flags, &total,
                                         &consumed, payload,
                                         request.payload_size) == NYAMP_OK
                       ? asr->Push(request.request_id, window, sequence)
                       : NYAMP_MODEL_INVALID;
          break;
        }

      case NYAMP_ASR_END:
        {
          std::uint64_t end_sample = 0;

          status = nyamp_asr_end_decode(&end_sample, payload,
                                        request.payload_size) == NYAMP_OK
                       ? asr->End(request.request_id, end_sample)
                       : NYAMP_MODEL_INVALID;
          break;
        }

      case NYAMP_ASR_RELEASE:
        {
          nyamp_buffer_s grant{};

          status = nyamp_buffer_decode(&grant, payload,
                                       request.payload_size) == NYAMP_OK
                       ? asr->Release(request.request_id, grant)
                       : NYAMP_MODEL_INVALID;
          break;
        }

      case NYAMP_ASR_CANCEL:
        status = request.payload_size != 0 ? NYAMP_MODEL_INVALID
                                           : asr->Cancel(request.request_id);
        break;

      default:
        break;
    }

  return EncodeResponse(request, status, generation, 0, body, body_size,
                        response, response_capacity, response_size);
}

/****************************************************************************
 * Name: DispatchTts
 *
 * Description:
 *   Route a TTS request.  SYNTH (ids from the control domain) predates the
 *   text front end and is answered unsupported: nothing on that side can
 *   produce the ids.
 *
 ****************************************************************************/

int DispatchTts(const nyamp_header_s &request, TtsService *tts,
                std::uint32_t generation, const uint8_t *payload,
                std::uint8_t *response, std::size_t response_capacity,
                std::size_t *response_size)
{
  std::int32_t status = NYAMP_MODEL_UNSUPPORTED;

  if (tts == nullptr)
    {
      return EncodeResponse(request, status, generation, 0, nullptr, 0,
                            response, response_capacity, response_size);
    }

  switch (request.opcode)
    {
      case NYAMP_TTS_LOAD:
        {
          if (request.payload_size == 0 ||
              request.payload_size > NYAMP_TTS_MAX_PATH)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          bool deferred = false;
          status = ModelStatus(tts->BeginLoad(
              std::string(reinterpret_cast<const char *>(payload),
                          request.payload_size),
              request, &deferred));
          if (deferred)
            {
              *response_size = 0;
              return NYAMP_OK;
            }

          break;
        }

      case NYAMP_TTS_UNLOAD:
        status = request.payload_size != 0 ? NYAMP_MODEL_INVALID
                                           : ModelStatus(tts->Unload());
        break;

      case NYAMP_TTS_SYNTH_TEXT:
        {
          nyamp_tts_text_s chunk{};
          const std::uint8_t *bytes = nullptr;
          bool started = false;

          status = nyamp_tts_text_decode(&chunk, &bytes, payload,
                                         request.payload_size) == NYAMP_OK
                       ? tts->BeginText(chunk, bytes, request.request_id,
                                        request.deadline_ms, &started)
                       : NYAMP_MODEL_INVALID;
          break;
        }

      case NYAMP_TTS_RELEASE:
        {
          nyamp_buffer_s window{};

          status = nyamp_buffer_decode(&window, payload,
                                       request.payload_size) == NYAMP_OK
                       ? tts->Release(request.request_id, window)
                       : NYAMP_MODEL_INVALID;
          break;
        }

      case NYAMP_TTS_CANCEL:
        status = request.payload_size != 0 ? NYAMP_MODEL_INVALID
                                           : tts->Cancel(request.request_id);
        break;

      default:
        break;
    }

  return EncodeResponse(request, status, generation, 0, nullptr, 0, response,
                        response_capacity, response_size);
}

/****************************************************************************
 * Name: DispatchKws
 ****************************************************************************/

int DispatchKws(const nyamp_header_s &request, KwsService *kws,
                std::uint32_t generation, const uint8_t *payload,
                std::uint8_t *response, std::size_t response_capacity,
                std::size_t *response_size)
{
  std::int32_t status = NYAMP_MODEL_UNSUPPORTED;
  std::uint8_t body[NYAMP_INLINE_MAX];
  std::size_t body_size = 0;

  if (kws == nullptr)
    {
      return EncodeResponse(request, status, generation, 0, nullptr, 0,
                            response, response_capacity, response_size);
    }

  switch (request.opcode)
    {
      case NYAMP_KWS_LOAD:
        {
          nyamp_kws_load_s load{};
          bool deferred = false;

          if (nyamp_kws_load_decode(&load, payload, request.payload_size) !=
              NYAMP_OK)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          status = ModelStatus(kws->BeginLoad(load, request, &deferred));
          if (deferred)
            {
              *response_size = 0;
              return NYAMP_OK;
            }

          break;
        }

      case NYAMP_KWS_UNLOAD:
        status = request.payload_size != 0 ? NYAMP_MODEL_INVALID
                                           : ModelStatus(kws->Unload());
        break;

      case NYAMP_KWS_BEGIN:
        {
          std::uint32_t sample_rate = 0;
          std::uint16_t channels = 0;
          std::uint16_t flags = 0;
          std::uint32_t window_samples = 0;
          nyamp_buffer_s grant{};

          if (nyamp_kws_begin_decode(&sample_rate, &channels, &flags,
                                     &window_samples, payload,
                                     request.payload_size) != NYAMP_OK)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          status = kws->Begin(request, sample_rate, channels, flags,
                              window_samples, &grant);
          if (status == NYAMP_MODEL_OK &&
              nyamp_buffer_encode(body, sizeof(body), &body_size, &grant) !=
                  NYAMP_OK)
            {
              status = NYAMP_MODEL_BACKEND_ERROR;
              body_size = 0;
            }

          break;
        }

      case NYAMP_KWS_PUSH:
        {
          nyamp_kws_push_s push{};
          std::uint64_t next_sample = 0;

          if (nyamp_kws_push_decode(&push, payload, request.payload_size) !=
              NYAMP_OK)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          status = kws->Push(push, &next_sample);
          if (status == NYAMP_MODEL_OK &&
              nyamp_kws_push_ack_encode(body, sizeof(body), &body_size,
                                        next_sample) != NYAMP_OK)
            {
              status = NYAMP_MODEL_BACKEND_ERROR;
              body_size = 0;
            }

          break;
        }

      case NYAMP_KWS_END:
        status = request.payload_size != 0 ? NYAMP_MODEL_INVALID : kws->End();
        break;

      case NYAMP_KWS_LIST:
        {
          if (request.payload_size != 0)
            {
              status = NYAMP_MODEL_INVALID;
              break;
            }

          status = kws->List(body, NYAMP_INLINE_MAX - kStatusPayloadSize,
                             &body_size);
          if (status != NYAMP_MODEL_OK)
            {
              body_size = 0;
            }

          break;
        }

      default:
        break;
    }

  return EncodeResponse(request, status, generation, 0, body, body_size,
                        response, response_capacity, response_size);
}

} // namespace

int Dispatch(const std::uint8_t *request_wire, std::size_t request_size,
             std::uint64_t now_ms, std::uint32_t generation,
             std::uint8_t *response, std::size_t response_capacity,
             std::size_t *response_size, std::string_view diagnostics,
             LlmService *llm, BlobService *blob,
             const SpeechServices *speech)
{
  nyamp_header_s request;
  int result = NYAMP_OK;
  AsrService *asr = speech != nullptr ? speech->asr : nullptr;
  TtsService *tts = speech != nullptr ? speech->tts : nullptr;
  KwsService *kws = speech != nullptr ? speech->kws : nullptr;
  const std::uint32_t capabilities =
      kCapabilityHealth | (llm != nullptr ? kCapabilityLlm : 0U) |
      (blob != nullptr ? kCapabilityBlob : 0U) |
      (llm != nullptr && llm->ChatSupported() ? kCapabilityChat : 0U) |
      (asr != nullptr && asr->Supported() ? kCapabilityAsr : 0U) |
      (tts != nullptr && tts->Supported() ? kCapabilityTts : 0U) |
      (kws != nullptr && kws->Supported() ? kCapabilityKws : 0U);

  if (request_wire == nullptr || response == nullptr ||
      response_size == nullptr)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_header_decode(&request, request_wire, request_size);
  if (result != NYAMP_OK)
    {
      return result;
    }

  /* Never answer a response or an event.  Both domains are responders now,
   * and an error reply to a reply is how two of them would loop.
   */
  const std::uint32_t kind = request.flags & NYAMP_FLAG_KIND_MASK;
  if (kind == NYAMP_FLAG_RESPONSE || kind == NYAMP_FLAG_EVENT)
    {
      *response_size = 0;
      return NYAMP_OK;
    }

  /* A payload-free CANCEL naming a BLOB request aborts that worker.  It has
   * no response of its own; the cancelled request's response reports it.
   */
  if (kind == NYAMP_FLAG_CANCEL && request.service == NYAMP_SERVICE_BLOB)
    {
      if (blob != nullptr)
        {
          blob->Cancel(request.request_id);
        }

      *response_size = 0;
      return NYAMP_OK;
    }

  /* The same for the speech services.  ASR and TTS also have a CANCEL
   * opcode, which is answered; this form is for a requester that gave up
   * and is not going to read an answer.
   */
  if (kind == NYAMP_FLAG_CANCEL &&
      (request.service == NYAMP_SERVICE_ASR ||
       request.service == NYAMP_SERVICE_TTS ||
       request.service == NYAMP_SERVICE_KWS))
    {
      if (request.service == NYAMP_SERVICE_ASR && asr != nullptr)
        {
          asr->Cancel(request.request_id);
        }
      else if (request.service == NYAMP_SERVICE_TTS && tts != nullptr)
        {
          tts->Cancel(request.request_id);
        }
      else if (request.service == NYAMP_SERVICE_KWS && kws != nullptr)
        {
          kws->Cancel(request.request_id);
        }

      *response_size = 0;
      return NYAMP_OK;
    }

  if (request_size != NYAMP_WIRE_HEADER_SIZE + request.payload_size ||
      request.flags != NYAMP_FLAG_REQUEST)
    {
      return EncodeResponse(request, NYAMP_MODEL_INVALID, generation,
                            capabilities, nullptr, 0, response,
                            response_capacity, response_size);
    }

  if (request.deadline_ms != 0 && now_ms > request.deadline_ms)
    {
      return EncodeResponse(request, NYAMP_MODEL_DEADLINE, generation,
                            capabilities, nullptr, 0, response,
                            response_capacity, response_size);
    }

  if (request.generation != 0 && request.generation != generation)
    {
      return EncodeResponse(request, NYAMP_MODEL_STALE_GENERATION, generation,
                            capabilities, nullptr, 0, response,
                            response_capacity, response_size);
    }

  if (request.service == NYAMP_SERVICE_LLM)
    {
      return DispatchLlm(request, llm, generation, capabilities,
                         request_wire + NYAMP_WIRE_HEADER_SIZE, response,
                         response_capacity, response_size);
    }

  if (request.service == NYAMP_SERVICE_BLOB)
    {
      return DispatchBlob(request, blob, generation, capabilities,
                          request_wire + NYAMP_WIRE_HEADER_SIZE, response,
                          response_capacity, response_size);
    }

  if (request.service == NYAMP_SERVICE_ASR)
    {
      return DispatchAsr(request, asr, generation,
                         request_wire + NYAMP_WIRE_HEADER_SIZE, response,
                         response_capacity, response_size);
    }

  if (request.service == NYAMP_SERVICE_TTS)
    {
      return DispatchTts(request, tts, generation,
                         request_wire + NYAMP_WIRE_HEADER_SIZE, response,
                         response_capacity, response_size);
    }

  if (request.service == NYAMP_SERVICE_KWS)
    {
      return DispatchKws(request, kws, generation,
                         request_wire + NYAMP_WIRE_HEADER_SIZE, response,
                         response_capacity, response_size);
    }

  if (request.service == NYAMP_SERVICE_HEALTH &&
      (request.opcode == kHealthQuery || request.opcode == kInfoQuery) &&
      request.payload_size == 0 &&
      (request.opcode == kHealthQuery || !diagnostics.empty()))
    {
      return EncodeResponse(
          request, NYAMP_MODEL_OK, generation, capabilities,
          reinterpret_cast<const std::uint8_t *>(diagnostics.data()),
          request.opcode == kInfoQuery ? diagnostics.size() : 0, response,
          response_capacity, response_size);
    }

  return EncodeResponse(request, NYAMP_MODEL_UNSUPPORTED, generation,
                        capabilities, nullptr, 0, response, response_capacity,
                        response_size);
}

} // namespace nyamp
