#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive a whole chat completion between the real client and the real service.

The client is the nyampctl translation unit (its wire-level chat path, the one
a firmware without the Core compute service uses, and the reference for the
CHAT opcode).  The service is nyampd's Dispatch + LlmService.  They talk over
a SOCK_SEQPACKET socket pair, which keeps message boundaries the way RPMsg
does.  Only two things are stood in for, both from nyampd_test_support.h: the
model (a scripted backend) and the vocabulary (a byte codec) -- tokenizer.json
is a third-party file that is never in this repository.  The chat template and
the output parser are the real ones.

So this proves the wire flow end to end -- chunked body, acknowledgements,
chunked result, the terminal finish with its statistics, and the overflow
status -- while claiming no inference.

usage: test_chat_flow.py <repo>
"""

import pathlib
import subprocess
import sys
import tempfile

repo = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(tempfile.mkdtemp(prefix="amp-chat-flow-"))
amp = repo / "tools/amp"
nyampd = amp / "nyampd"
chat = amp / "chat"
models = amp / "models"
protocol = amp / "protocol"
client = repo / "app/nyampctl"

(root / "nuttx").mkdir()
(root / "nuttx/config.h").write_text("")

# The C client, compiled as C, with one entry point the harness can call.
(root / "client.c").write_text(r'''
#include "nyampctl.h"
int chat_flow_client(int fd, const char *path, unsigned int max_new_tokens)
{
  return nyampctl_llm_chat(fd, path, max_new_tokens);
}
''')

(root / "harness.cpp").write_text(r'''
#include "nyampd_core.h"
#include "nyampd_llm.h"
#include "nyampd_test_support.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" int chat_flow_client(int fd, const char *path,
                                unsigned int max_new_tokens);

using namespace nyamp::testing;

namespace
{

constexpr std::uint32_t kFlowGeneration = 4242;

/* nyampd's transport loop, reduced to what a chat needs. */

void Serve(int fd, nyamp::LlmService &llm, std::atomic<bool> &stop)
{
  while (!stop.load())
    {
      struct pollfd pollfd = { fd, POLLIN, 0 };
      if (poll(&pollfd, 1, 5) > 0 && (pollfd.revents & POLLIN) != 0)
        {
          std::uint8_t request[NYAMP_RPMSG_MTU];
          std::uint8_t response[NYAMP_RPMSG_MTU];
          std::size_t response_size = 0;
          const ssize_t got = read(fd, request, sizeof(request));
          if (got > 0 &&
              nyamp::Dispatch(request, static_cast<std::size_t>(got), 0,
                              kFlowGeneration, response, sizeof(response),
                              &response_size, {}, &llm) == NYAMP_OK &&
              response_size != 0)
            {
              (void)!write(fd, response, response_size);
            }
        }

      nyamp::Frame frame;
      while (llm.Poll(&frame))
        {
          (void)!write(fd, frame.data, frame.size);
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2)
    {
      return 2;
    }

  const std::string dir = argv[1];
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) != 0)
    {
      std::perror("socketpair");
      return 1;
    }

  Script script;
  nyamp::LlmService llm(
      kFlowGeneration, [] { return 0; },
      [&] { return std::make_unique<ScriptedBackend>(&script); });
  llm.SetChatCodecFactory([](const std::string &, std::string *) {
    return std::make_unique<ByteCodec>();
  });

  std::ofstream(dir + "/model.rkllm") << "weights";
  std::ofstream(dir + "/tokenizer.json") << "{}";
  if (llm.Load(dir + "/model.rkllm") != nyamp::models::Status::kOk)
    {
      return 1;
    }

  std::atomic<bool> stop{ false };
  std::thread server(Serve, pair[0], std::ref(llm), std::ref(stop));

  /* A tool call whose response JSON needs several result chunks: the long
   * argument is what makes it long.
   */
  const std::string note(900, 'n');
  script.output = "<function name=\"remember\"><param name=\"note\">" + note +
                  "</param><param name=\"priority\">2</param></function>";

  std::ofstream(dir + "/request.json")
      << R"({"messages":[{"role":"system","content":"You are Nyabula."},)"
         R"({"role":"user","content":"记一下这件事，然后告诉我你记了什么。)"
      << std::string(500, 'q')
      << R"("}],"tools":[{"type":"function","function":{"name":"remember",)"
         R"("description":"Store a note","parameters":{"type":"object",)"
         R"("properties":{"note":{"type":"string"},)"
         R"("priority":{"type":"integer"}}}}}]})";

  int failures = 0;
  std::fflush(stdout);
  if (chat_flow_client(pair[1], (dir + "/request.json").c_str(), 1200) != 0)
    {
      std::fprintf(stderr, "tool-call chat failed\n");
      ++failures;
    }

  /* The same request with a budget the window cannot hold is refused with
   * the overflow status, which the client reports as a failure.
   */
  std::fflush(stdout);
  std::printf("--- overflow\n");
  std::fflush(stdout);
  if (chat_flow_client(pair[1], (dir + "/request.json").c_str(), 2040) == 0)
    {
      std::fprintf(stderr, "overflow was not reported\n");
      ++failures;
    }

  std::fflush(stdout);
  stop.store(true);
  server.join();
  close(pair[0]);
  close(pair[1]);
  return failures == 0 ? 0 : 1;
}
''')

objects = []
for source in ("nyampctl_llm.c",):
    target = root / (source + ".o")
    subprocess.run(
        ["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Werror", "-c",
         "-I", str(root), "-I", str(protocol), "-I", str(client),
         str(client / source), "-o", str(target)], check=True)
    objects.append(str(target))

for source in (root / "client.c", protocol / "nyamp_protocol.c"):
    target = root / (source.name + ".o")
    subprocess.run(
        ["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Werror", "-c",
         "-I", str(root), "-I", str(protocol), "-I", str(client),
         str(source), "-o", str(target)], check=True)
    objects.append(str(target))

subprocess.run(
    ["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
     "-I", str(nyampd), "-I", str(models), "-I", str(protocol),
     "-I", str(chat),
     str(root / "harness.cpp"),
     *[str(nyampd / name) for name in (
         "nyampd_core.cpp", "nyampd_llm.cpp", "nyampd_frame.cpp",
         "nyampd_sha256.cpp", "nyampd_blob.cpp", "nyampd_provision.cpp",
         "nyampd_chat.cpp")],
     *[str(chat / name) for name in (
         "nyamp_json.cpp", "nyamp_unicode.cpp", "nyamp_tokenizer.cpp",
         "nyamp_chat_template.cpp", "nyamp_chat_parse.cpp")],
     str(models / "nyamp_models.cpp"), *objects,
     "-o", str(root / "harness"), "-lpthread"],
    check=True)

result = subprocess.run([str(root / "harness"), str(root)],
                        capture_output=True, text=True, timeout=60)
sys.stderr.write(result.stderr)
assert result.returncode == 0, result.stdout

first, overflow = result.stdout.split("--- overflow\n")

# The response the client printed is what an OpenAI client expects.
import json
response = json.loads(first.splitlines()[0])
choice = response["choices"][0]
assert response["object"] == "chat.completion", response
assert choice["finish_reason"] == "tool_calls", choice
assert choice["message"]["role"] == "assistant"
assert choice["message"]["content"] is None
call = choice["message"]["tool_calls"][0]
assert call["type"] == "function" and call["id"].startswith("call_")
assert call["function"]["name"] == "remember"
arguments = json.loads(call["function"]["arguments"])
assert arguments == {"note": "n" * 900, "priority": 2}, arguments
usage = response["usage"]
assert usage["total_tokens"] == usage["prompt_tokens"] + usage["completion_tokens"]
assert len(first.splitlines()[0]) > 1000        # several result chunks
assert "status=0" in first and f"completion_tokens={usage['completion_tokens']}" in first

# NYAMP_MODEL_PROMPT_TOO_LONG, with the numbers needed to trim history.
assert "status=-11" in overflow, overflow
assert f"prompt_tokens={usage['prompt_tokens']}" in overflow, overflow
assert "context=2048" in overflow, overflow

print(f"CHAT_WIRE_FLOW_PASS prompt_tokens={usage['prompt_tokens']} "
      f"completion_tokens={usage['completion_tokens']} "
      f"response_bytes={len(first.splitlines()[0])}")
