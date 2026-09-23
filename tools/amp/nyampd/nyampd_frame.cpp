/****************************************************************************
 * tools/amp/nyampd/nyampd_frame.cpp
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

#include "nyampd_frame.h"

#include <cstring>

namespace nyamp
{

bool EncodeFrame(Frame *frame, nyamp_header_s header,
                 const std::uint8_t *payload, std::size_t payload_size)
{
  if (frame == nullptr || payload_size > NYAMP_INLINE_MAX ||
      (payload == nullptr && payload_size != 0))
    {
      return false;
    }

  header.payload_size = static_cast<std::uint32_t>(payload_size);
  if (nyamp_header_encode(frame->data, sizeof(frame->data), &header) !=
      NYAMP_OK)
    {
      return false;
    }

  if (payload_size != 0)
    {
      std::memcpy(frame->data + NYAMP_WIRE_HEADER_SIZE, payload, payload_size);
    }

  frame->size = NYAMP_WIRE_HEADER_SIZE + payload_size;
  return true;
}

bool EncodeStatusResponse(Frame *frame, const nyamp_header_s &request,
                          std::uint32_t generation, std::int32_t status,
                          const std::uint8_t *body, std::size_t body_size)
{
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::uint8_t *destination = nullptr;
  std::size_t capacity = 0;

  if (nyamp_status_encode(payload, sizeof(payload), status, &destination,
                          &capacity) != NYAMP_OK ||
      body_size > capacity || (body == nullptr && body_size != 0))
    {
      return false;
    }

  if (body_size != 0)
    {
      std::memcpy(destination, body, body_size);
    }

  nyamp_header_s header{};
  header.service = request.service;
  header.opcode = request.opcode;
  header.flags = NYAMP_FLAG_RESPONSE | (status == 0 ? 0U : NYAMP_FLAG_ERROR);
  header.request_id = request.request_id;
  header.deadline_ms = request.deadline_ms;
  header.generation = generation;
  return EncodeFrame(frame, header, payload, NYAMP_STATUS_SIZE + body_size);
}

} // namespace nyamp
