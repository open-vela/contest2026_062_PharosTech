/****************************************************************************
 * tools/amp/nyampd/nyampd_core.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_CORE_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_CORE_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nyamp
{

class LlmService;
class BlobService;
class AsrService;
class TtsService;
class KwsService;

/* The speech services.  Any of them may be null; its opcodes then answer
 * unsupported, exactly as a null `llm` does.
 */

struct SpeechServices
{
  AsrService *asr = nullptr;
  TtsService *tts = nullptr;
  KwsService *kws = nullptr;
};

constexpr std::uint16_t kHealthQuery = 1;
constexpr std::uint16_t kInfoQuery = 2;

enum class Status : std::int32_t
{
  kOk = 0,
  kProtocol = -1,
  kDeadline = -2,
  kGeneration = -3,
  kUnsupported = -4,
  kInvalid = -5,
  kNotReady = -6,
  kBusy = -7,
};

/****************************************************************************
 * Name: Dispatch
 *
 * Description:
 *   Handle one decoded request frame.  `llm` may be null, in which case every
 *   LLM opcode is answered with unsupported rather than a fabricated success.
 *   A GENERATE that completes its chunk sequence is accepted here and its
 *   events are drained later by TakeEvent, not returned from this call.
 *
 *   NYAMP_OK with *response_size == 0 means "nothing to send now".  That is
 *   the result for a request whose response is deferred to a service queue
 *   (a LOAD that must first pull its model, a BLOB bench or pull), and for a
 *   RESPONSE or EVENT frame.  The latter matters now that both domains answer
 *   requests: replying to a response would let two responders bounce error
 *   frames off each other forever, so such a frame is dropped, never answered.
 *
 *   blob may be null; the control-originated BLOB opcodes then answer
 *   unsupported.  So may speech, or any service inside it.
 *
 *   A CANCEL-kind frame (no payload, request_id naming the target) is
 *   accepted for BLOB, ASR, TTS and KWS.  It has no response of its own; the
 *   cancelled request's terminal event or response reports it.
 *
 ****************************************************************************/

int Dispatch(const std::uint8_t *request, std::size_t request_size,
             std::uint64_t now_ms, std::uint32_t generation,
             std::uint8_t *response, std::size_t response_capacity,
             std::size_t *response_size, std::string_view diagnostics = {},
             LlmService *llm = nullptr, BlobService *blob = nullptr,
             const SpeechServices *speech = nullptr);

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_CORE_H */
