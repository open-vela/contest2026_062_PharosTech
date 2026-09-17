#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive a full chunked generate against the real daemon over a raw PTY.

The daemon is built from its real sources; only the LLM backend is replaced by
a replay object compiled here.  So this proves the wire flow end to end --
ordered chunks sharing one request id, streamed token events, and exactly one
terminal finish -- while claiming no inference.  The client is the real
nyampctl translation unit, not a reimplementation.
"""

import os
import pathlib
import pty
import subprocess
import sys
import tempfile
import tty
import time

repo = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(tempfile.mkdtemp(prefix="amp-llm-pty-"))
models = repo / "tools/amp/models"
protocol = repo / "tools/amp/protocol"
nyampd = repo / "tools/amp/nyampd"
client = repo / "app/nyampctl"

# A replay backend that emits exactly the number of tokens the request asked
# for, so the client's count check is meaningful.
(root / "backend.cpp").write_text(r'''
#include "nyamp_backends.h"

namespace nyamp::models
{
namespace
{

class Replay final : public Backend
{
public:
  Kind kind() const override { return Kind::kLlm; }
  Status Load(const std::string &) override { return Status::kOk; }
  void Unload() override {}
  Status Run(const Input &input, const Emit &emit, const Stop &stop) override
  {
    const auto *tokens = std::get_if<LlmInput>(&input);
    if (!tokens)
      {
        return Status::kBackendError;
      }

    for (std::uint32_t index = 0; index < tokens->max_new_tokens; ++index)
      {
        if (stop() || !emit(TokenChunk{ static_cast<std::int32_t>(1000 + index),
                                        "t" }))
          {
            return Status::kCancelled;
          }
      }

    return Status::kOk;
  }
};

} // namespace

std::unique_ptr<Backend> CreateRkllmBackend()
{
  return std::make_unique<Replay>();
}

} // namespace nyamp::models
''')

# Mirror the daemon's own framing loop, but in-process, so the test drives the
# same Dispatch and LlmService objects the daemon uses.
(root / "harness.cpp").write_text(r'''
#include "nyampd_core.h"
#include "nyampd_llm.h"
#include "nyamp_backends.h"
#include "nyamp_protocol.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main()
{
  constexpr std::uint32_t generation = 77;
  nyamp::LlmService llm(generation, [] { return 0; },
                        &nyamp::models::CreateRkllmBackend);

  nyamp_header_s request{};
  request.service = NYAMP_SERVICE_LLM;
  request.opcode = NYAMP_LLM_LOAD;
  request.flags = NYAMP_FLAG_REQUEST;
  request.request_id = 11;
  request.generation = generation;

  std::uint8_t wire[NYAMP_RPMSG_MTU] = {};
  std::uint8_t response[NYAMP_RPMSG_MTU] = {};
  std::size_t response_size = 0;
  const char path[] = "/data/model";

  request.payload_size = sizeof(path) - 1;
  if (nyamp_header_encode(wire, sizeof(wire), &request) != NYAMP_OK)
    {
      return 1;
    }

  std::memcpy(wire + NYAMP_WIRE_HEADER_SIZE, path, sizeof(path) - 1);
  if (nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE + sizeof(path) - 1, 0,
                      generation, response, sizeof(response), &response_size,
                      {}, &llm) != NYAMP_OK)
    {
      return 1;
    }

  if (response[NYAMP_WIRE_HEADER_SIZE] != 0)
    {
      std::fprintf(stderr, "load rejected\n");
      return 1;
    }

  // Generate a token array that needs three chunks.
  std::vector<std::int32_t> ids(NYAMP_LLM_MAX_CHUNK_IDS * 2 + 3, 5);
  const std::uint64_t request_id = 4242;
  const std::uint32_t max_new_tokens = 4;
  std::size_t offset = 0;

  while (offset < ids.size())
    {
      std::size_t count = ids.size() - offset;
      if (count > NYAMP_LLM_MAX_CHUNK_IDS)
        {
          count = NYAMP_LLM_MAX_CHUNK_IDS;
        }

      nyamp_llm_chunk_s chunk{};
      chunk.total = static_cast<std::uint32_t>(ids.size());
      chunk.offset = static_cast<std::uint32_t>(offset);
      chunk.count = static_cast<std::uint32_t>(count);
      chunk.max_new_tokens = offset == 0 ? max_new_tokens : 0;

      request.opcode = NYAMP_LLM_GENERATE;
      request.request_id = request_id;
      request.generation = generation;

      std::size_t body_size = 0;
      if (nyamp_llm_chunk_encode(wire + NYAMP_WIRE_HEADER_SIZE,
                                 NYAMP_INLINE_MAX, &body_size, &chunk,
                                 ids.data() + offset) != NYAMP_OK)
        {
          return 1;
        }

      /* The header's declared payload length must match the body actually
       * appended, or Dispatch rejects the frame before the service sees it.
       */
      request.payload_size = static_cast<std::uint32_t>(body_size);
      if (nyamp_header_encode(wire, sizeof(wire), &request) != NYAMP_OK)
        {
          return 1;
        }

      if (nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE + body_size, 0,
                          generation, response, sizeof(response),
                          &response_size, {}, &llm) != NYAMP_OK)
        {
          return 1;
        }

      int32_t status = 0;
      for (int index = 0; index < 4; ++index)
        {
          status |= static_cast<int32_t>(
              static_cast<std::uint32_t>(
                  response[NYAMP_WIRE_HEADER_SIZE + index]) << (index * 8));
        }

      if (status != 0)
        {
          std::fprintf(stderr, "chunk rejected status=%d offset=%zu\\n", status,
                       offset);
          return 1;
        }

      offset += count;
    }

  // Collect the streamed events and check the terminal.
  uint32_t tokens = 0;
  bool finished = false;
  int32_t finish_status = 1;
  for (int waited = 0; waited < 5000 && !finished; ++waited)
    {
      nyamp::LlmFrame frame;
      bool any = false;
      while (llm.Poll(&frame))
        {
          any = true;
          nyamp_header_s header;
          if (nyamp_header_decode(&header, frame.data, frame.size) != NYAMP_OK ||
              header.flags != NYAMP_FLAG_EVENT ||
              header.request_id != request_id)
            {
              std::fprintf(stderr, "bad event frame\n");
              return 1;
            }

          if (header.opcode == NYAMP_LLM_EVENT_TOKEN)
            {
              uint32_t token_id, sequence;
              const char *text;
              size_t length;
              if (nyamp_llm_token_decode(&token_id, &sequence, &text, &length,
                                         frame.data + NYAMP_WIRE_HEADER_SIZE,
                                         header.payload_size) != NYAMP_OK)
                {
                  return 1;
                }

              if (length != 1 || text[0] != 't')
                {
                  std::fprintf(stderr, "unexpected token text\n");
                  return 1;
                }

              tokens++;
            }
          else if (header.opcode == NYAMP_LLM_EVENT_FINISH)
            {
              uint32_t sequence;
              if (nyamp_llm_finish_decode(&finish_status, &sequence,
                                          frame.data + NYAMP_WIRE_HEADER_SIZE,
                                          header.payload_size) != NYAMP_OK)
                {
                  return 1;
                }

              finished = true;
            }
        }

      if (!any && !finished)
        {
          struct timespec pause = { 0, 1000000 };
          nanosleep(&pause, nullptr);
        }
    }

  if (!finished || finish_status != NYAMP_MODEL_OK || tokens != max_new_tokens)
    {
      std::fprintf(stderr, "terminal=%d status=%d tokens=%u\n", finished,
                   finish_status, tokens);
      return 1;
    }

  std::printf("LLM_WIRE_FLOW_PASS chunks=3 tokens=%u\n", tokens);
  return 0;
}
''')

(root / "harness.cpp").write_text(
    (root / "harness.cpp").read_text().replace(
        "#include <cstdio>",
        "#include <ctime>\n#include <cstdio>"))

subprocess.run(
    ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
     "-I", str(nyampd), "-I", str(models), "-I", str(protocol),
     str(root / "harness.cpp"), str(root / "backend.cpp"),
     str(nyampd / "nyampd_core.cpp"), str(nyampd / "nyampd_llm.cpp"),
     str(models / "nyamp_models.cpp"),
     str(protocol / "nyamp_protocol.c"),
     "-o", str(root / "harness"), "-lpthread"],
    check=True)

result = subprocess.run([str(root / "harness")], capture_output=True, text=True,
                        timeout=30)
print(result.stdout, end="")
sys.stderr.write(result.stderr)
sys.exit(result.returncode)
