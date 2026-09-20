/****************************************************************************
 * tools/amp/nyampd/nyampd_frame.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_FRAME_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_FRAME_H

#include <cstddef>
#include <cstdint>

#include "nyamp_protocol.h"

namespace nyamp
{

/* One encoded RPMsg message.  Services hand these to the transport loop
 * instead of writing the endpoint themselves, so the loop stays the only
 * owner of the file descriptor.
 */

struct Frame
{
  std::uint8_t data[NYAMP_RPMSG_MTU];
  std::size_t size;
};

/****************************************************************************
 * Name: EncodeFrame
 *
 * Description:
 *   Build a complete frame from a header template and a payload.  Returns
 *   false when the header is illegal or the payload does not fit.
 *
 ****************************************************************************/

bool EncodeFrame(Frame *frame, nyamp_header_s header,
                 const std::uint8_t *payload, std::size_t payload_size);

/****************************************************************************
 * Name: EncodeStatusResponse
 *
 * Description:
 *   Build the response to `request`: a status followed by an optional body.
 *   A service that answers later than Dispatch returns uses this so a
 *   deferred response is byte-identical to an immediate one.
 *
 ****************************************************************************/

bool EncodeStatusResponse(Frame *frame, const nyamp_header_s &request,
                          std::uint32_t generation, std::int32_t status,
                          const std::uint8_t *body, std::size_t body_size);

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_FRAME_H */
