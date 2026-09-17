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
#include "nyampd_llm.h"

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
                std::uint32_t generation, const uint8_t *payload,
                std::uint8_t *response, std::size_t response_capacity,
                std::size_t *response_size)
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
          status = ModelStatus(llm->Load(directory));
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

  return EncodeResponse(request, status, generation, kCapabilityLlm, body,
                        body_size, response, response_capacity, response_size);
}

} // namespace

int Dispatch(const std::uint8_t *request_wire, std::size_t request_size,
             std::uint64_t now_ms, std::uint32_t generation,
             std::uint8_t *response, std::size_t response_capacity,
             std::size_t *response_size, std::string_view diagnostics,
             LlmService *llm)
{
  nyamp_header_s request;
  int result = NYAMP_OK;
  const std::uint32_t capabilities =
      kCapabilityHealth | (llm != nullptr ? kCapabilityLlm : 0U);

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
      return DispatchLlm(request, llm, generation,
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
