/****************************************************************************
 * tools/amp/nyampd/nyampd_test_support.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_TEST_SUPPORT_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_TEST_SUPPORT_H

/* Shared by the blob and chat tests and by tools/amp/test_chat_flow.py: an
 * in-process control domain with a rig that wires one blob client to it, and
 * the two stand-ins a chat test needs -- a byte codec for the vocabulary and
 * a scripted backend for the model.  Test code only; nothing here is linked
 * into the daemon.
 */

#include "nyampd_blob.h"
#include "nyampd_chat.h"
#include "nyampd_sha256.h"

#include "nyamp_chat_template.h"
#include "nyamp_json.h"
#include "nyamp_models.h"
#include "nyamp_protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nyamp::testing
{

inline constexpr std::uint32_t kGeneration = 0x51a7e001U;
inline constexpr std::uint32_t kWindowOffset = 0x1000;

/****************************************************************************
 * Name: Responder
 *
 * Description:
 *   The control domain, in process.  It is both ends of the fake link: the
 *   transport the client sends into and the shared window the client reads
 *   back, so a test sees exactly the bytes a real responder would have put
 *   in the arena.  It answers from inside Send, which is the harshest timing
 *   a client can meet -- the response exists before Send has returned.
 *
 *   It applies the checks the real responder applies (generation, window
 *   placement, lease freshness, name rules) so a client that only works
 *   against a lenient peer fails here.  The fault knobs are the ways a real
 *   link goes wrong.
 *
 ****************************************************************************/

class Responder final : public nyamp::BlobTransport, public nyamp::BlobWindow
{
public:
  explicit Responder(std::uint32_t capacity) : window_(capacity, 0xee) {}

  void Attach(nyamp::BlobClient *client) { client_ = client; }

  /* BlobWindow */

  const std::uint8_t *data() const override { return window_.data(); }
  std::uint32_t offset() const override { return kWindowOffset; }
  std::uint32_t capacity() const override
  {
    return static_cast<std::uint32_t>(window_.size());
  }

  /* BlobTransport */

  bool Send(const nyamp::Frame &frame) override
  {
    nyamp_header_s header;

    if (link_down)
      {
        return false;
      }

    if (nyamp_header_decode(&header, frame.data, frame.size) != NYAMP_OK ||
        frame.size != NYAMP_WIRE_HEADER_SIZE + header.payload_size)
      {
        ++malformed;
        return true;
      }

    /* Every id this domain originates carries the origin bit. */
    if ((header.request_id & NYAMP_REQUEST_ID_COMPUTE) == 0)
      {
        ++malformed;
      }

    if (header.flags == NYAMP_FLAG_CANCEL)
      {
        cancelled_ids.push_back(header.request_id);
        return true;
      }

    if (header.flags != NYAMP_FLAG_REQUEST ||
        header.service != NYAMP_SERVICE_BLOB)
      {
        ++malformed;
        return true;
      }

    Handle(header, frame.data + NYAMP_WIRE_HEADER_SIZE);
    return true;
  }

  /* Files and fault injection. */

  std::map<std::string, std::vector<std::uint8_t> > files;
  std::uint32_t generation = kGeneration;
  std::uint32_t max_read = 0; /* Non-zero: cap every READ.         */
  int corrupt_read = -1;      /* Flip a byte in this READ index.   */
  int stale_after_reads = -1; /* "Restart" after this many READs.  */
  int eof_after_reads = -1;   /* File "shrinks" after this many.   */
  bool lie_about_digest = false;
  bool hold_open = false;    /* Never answer OPEN.                */
  bool stale_window = false; /* BENCH FILL leaves old bytes.      */
  bool link_down = false;

  /* Observations. */

  int malformed = 0;
  int opens = 0;
  int reads = 0;
  int closes = 0;
  int lists = 0;
  std::set<std::uint32_t> open_blobs;
  std::vector<std::uint64_t> cancelled_ids;
  std::uint64_t held_open_id = 0;

private:
  void Reply(const nyamp_header_s &request, std::int32_t status,
             const std::uint8_t *body, std::size_t body_size)
  {
    nyamp::Frame frame;
    nyamp_header_s header;

    /* Responses echo the request's generation: it is the only one there is. */
    if (!nyamp::EncodeStatusResponse(&frame, request, request.generation,
                                     status, body, body_size) ||
        nyamp_header_decode(&header, frame.data, frame.size) != NYAMP_OK)
      {
        ++malformed;
        return;
      }

    client_->OnFrame(header, frame.data + NYAMP_WIRE_HEADER_SIZE);
  }

  bool WindowOk(const nyamp_buffer_s &buffer, std::uint32_t request_generation)
  {
    const bool fresh = leases_.insert(buffer.lease).second;

    return fresh && buffer.offset == kWindowOffset &&
           buffer.capacity <= window_.size() && buffer.capacity != 0 &&
           buffer.generation == request_generation &&
           (buffer.lease >> 32) == request_generation && buffer.length == 0;
  }

  bool IsDirectory(const std::string &name) const
  {
    const std::string prefix = name + "/";
    for (const auto &file : files)
      {
        if (file.first.compare(0, prefix.size(), prefix) == 0)
          {
            return true;
          }
      }

    return false;
  }

  void Handle(const nyamp_header_s &request, const std::uint8_t *payload)
  {
    if (request.generation != generation)
      {
        Reply(request, NYAMP_MODEL_STALE_GENERATION, nullptr, 0);
        return;
      }

    switch (request.opcode)
      {
        case NYAMP_BLOB_OPEN:
          {
            std::uint32_t flags = 0;
            const char *name = nullptr;
            std::size_t length = 0;

            ++opens;
            if (nyamp_blob_open_decode(&flags, &name, &length, payload,
                                       request.payload_size) != NYAMP_OK)
              {
                Reply(request, NYAMP_MODEL_INVALID, nullptr, 0);
                return;
              }

            if (hold_open)
              {
                held_open_id = request.request_id;
                return;
              }

            const std::string key(name, length);
            const auto found = files.find(key);
            if (found == files.end())
              {
                Reply(request,
                      IsDirectory(key) ? NYAMP_MODEL_UNSUPPORTED
                                       : NYAMP_MODEL_NOT_READY,
                      nullptr, 0);
                return;
              }

            nyamp_blob_info_s info{};
            nyamp::Sha256 hash;
            std::uint8_t body[NYAMP_BLOB_INFO_SIZE];
            std::size_t size = 0;

            info.blob_id = ++next_blob_;
            info.size = found->second.size();
            info.mtime = 1789000000;
            hash.Update(found->second.data(), found->second.size());
            hash.Final(info.sha256);
            if (lie_about_digest)
              {
                info.sha256[5] ^= 0x40;
              }

            open_blobs.insert(info.blob_id);
            blob_names_[info.blob_id] = key;
            nyamp_blob_info_encode(body, sizeof(body), &size, &info);
            Reply(request, NYAMP_MODEL_OK, body, size);
            return;
          }

        case NYAMP_BLOB_READ:
          {
            nyamp_blob_read_s read{};
            std::uint8_t body[NYAMP_BLOB_READ_SIZE];
            std::size_t size = 0;

            if (nyamp_blob_read_decode(&read, payload, request.payload_size) !=
                    NYAMP_OK ||
                open_blobs.count(read.blob_id) == 0 ||
                !WindowOk(read.buffer, request.generation))
              {
                Reply(request, NYAMP_MODEL_INVALID, nullptr, 0);
                return;
              }

            if (stale_after_reads >= 0 && reads >= stale_after_reads)
              {
                /* The compute domain "restarted": every blob is gone and the
                 * old generation is refused from here on.
                 */
                generation ^= 0x5a5a5a5aU;
                open_blobs.clear();
                Reply(request, NYAMP_MODEL_STALE_GENERATION, nullptr, 0);
                return;
              }

            const std::vector<std::uint8_t> &file =
                files[blob_names_[read.blob_id]];
            std::uint64_t end = file.size();
            if (eof_after_reads >= 0 && reads >= eof_after_reads)
              {
                end = read.file_offset + 1;
              }

            std::uint64_t count =
                read.file_offset >= end ? 0 : end - read.file_offset;
            count = std::min<std::uint64_t>(count, read.buffer.capacity);
            if (max_read != 0)
              {
                count = std::min<std::uint64_t>(count, max_read);
              }

            std::memcpy(window_.data(), file.data() + read.file_offset, count);
            if (corrupt_read == reads && count != 0)
              {
                window_[count / 2] ^= 0x01;
              }

            ++reads;
            read.buffer.length = static_cast<std::uint32_t>(count);
            if (read.file_offset + count >= end)
              {
                read.flags = NYAMP_BLOB_READ_EOF;
                read.buffer.flags |= NYAMP_BUFFER_LAST;
              }

            nyamp_blob_read_encode(body, sizeof(body), &size, &read);
            Reply(request, NYAMP_MODEL_OK, body, size);
            return;
          }

        case NYAMP_BLOB_CLOSE:
          {
            std::uint32_t blob_id = 0;

            ++closes;
            if (nyamp_blob_close_decode(&blob_id, payload,
                                        request.payload_size) != NYAMP_OK ||
                open_blobs.erase(blob_id) == 0)
              {
                Reply(request, NYAMP_MODEL_INVALID, nullptr, 0);
                return;
              }

            Reply(request, NYAMP_MODEL_OK, nullptr, 0);
            return;
          }

        case NYAMP_BLOB_LIST:
          {
            std::uint32_t cursor = 0;
            const char *prefix = nullptr;
            std::size_t length = 0;
            std::uint8_t body[NYAMP_BLOB_LIST_BODY_MAX];
            std::size_t size = 0;

            ++lists;
            if (nyamp_blob_list_decode(&cursor, &prefix, &length, payload,
                                       request.payload_size) != NYAMP_OK)
              {
                Reply(request, NYAMP_MODEL_INVALID, nullptr, 0);
                return;
              }

            /* Direct children of the prefix, files and directories alike. */
            const std::string base = length == 0
                                         ? std::string()
                                         : std::string(prefix, length) + "/";
            std::map<std::string, std::pair<std::uint64_t, bool> > children;
            for (const auto &file : files)
              {
                if (file.first.compare(0, base.size(), base) != 0)
                  {
                    continue;
                  }

                const std::string rest = file.first.substr(base.size());
                const std::size_t slash = rest.find('/');
                if (slash == std::string::npos)
                  {
                    children[rest] = { file.second.size(), false };
                  }
                else
                  {
                    children[rest.substr(0, slash)] = { 0, true };
                  }
              }

            if (children.empty())
              {
                Reply(request, NYAMP_MODEL_NOT_READY, nullptr, 0);
                return;
              }

            nyamp_blob_list_body_begin(body, sizeof(body), &size);
            std::uint32_t index = 0;
            std::uint32_t next = 0;
            for (const auto &child : children)
              {
                if (index++ < cursor)
                  {
                    continue;
                  }

                nyamp_blob_entry_s entry{};
                entry.size = child.second.first;
                entry.flags =
                    child.second.second ? NYAMP_BLOB_ENTRY_DIRECTORY : 0;
                entry.name = child.first.data();
                entry.name_length =
                    static_cast<std::uint16_t>(child.first.size());
                if (nyamp_blob_list_body_append(body, sizeof(body), &size,
                                                &entry) == NYAMP_EMSGSIZE)
                  {
                    next = index - 1;
                    break;
                  }
              }

            nyamp_blob_list_body_finish(body, size, next);
            Reply(request, NYAMP_MODEL_OK, body, size);
            return;
          }

        case NYAMP_BLOB_BENCH:
          {
            nyamp_blob_bench_s bench{};
            std::uint8_t body[NYAMP_BLOB_BENCH_FILL_SIZE];
            std::size_t size = 0;

            if (nyamp_blob_bench_decode(&bench, payload,
                                        request.payload_size) != NYAMP_OK ||
                (bench.mode == NYAMP_BLOB_BENCH_FILL &&
                 (!WindowOk(bench.buffer, request.generation) ||
                  bench.buffer.capacity % 4096 != 0)))
              {
                Reply(request, NYAMP_MODEL_INVALID, nullptr, 0);
                return;
              }

            if (bench.mode == NYAMP_BLOB_BENCH_FILL)
              {
                if (!stale_window || !filled_once_)
                  {
                    nyamp_blob_bench_pattern(window_.data(),
                                             bench.buffer.capacity / 4,
                                             bench.seed, 0);
                    filled_once_ = true;
                  }

                bench.buffer.length = bench.buffer.capacity;
              }

            nyamp_blob_bench_encode(body, sizeof(body), &size, &bench);
            Reply(request, NYAMP_MODEL_OK, body, size);
            return;
          }

        default:
          Reply(request, NYAMP_MODEL_UNSUPPORTED, nullptr, 0);
          return;
      }
  }

  nyamp::BlobClient *client_ = nullptr;
  std::vector<std::uint8_t> window_;
  std::set<std::uint64_t> leases_;
  std::map<std::uint32_t, std::string> blob_names_;
  std::uint32_t next_blob_ = 100;
  bool filled_once_ = false;
};

/* One client wired to one responder over a private scratch root. */

struct Rig
{
  explicit Rig(std::uint32_t window, std::uint32_t generation = kGeneration)
      : responder(window)
  {
    char pattern[] = "/tmp/nyamp-blob-test-XXXXXX";
    root = mkdtemp(pattern);

    nyamp::BlobClient::Options options;
    options.root = root;
    options.open_timeout_ms = 2000;
    options.io_timeout_ms = 2000;
    client = std::make_unique<nyamp::BlobClient>(generation, &responder,
                                                 &responder, options);
    responder.Attach(client.get());
  }

  ~Rig()
  {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  bool Exists(const std::string &relative) const
  {
    return std::filesystem::exists(root + "/" + relative);
  }

  std::vector<std::uint8_t> Read(const std::string &relative) const
  {
    std::ifstream file(root + "/" + relative, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>());
  }

  Responder responder;
  std::string root;
  std::unique_ptr<nyamp::BlobClient> client;
};

inline std::vector<std::uint8_t> Noise(std::size_t size, std::uint32_t seed)
{
  std::vector<std::uint8_t> bytes(size);
  std::uint32_t state = seed | 1;

  for (std::uint8_t &byte : bytes)
    {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      byte = static_cast<std::uint8_t>(state >> 11);
    }

  return bytes;
}

/* Generated ids are bytes shifted clear of the real stop ids, so the byte
 * 0x01 can never be taken for </s>.
 */

inline constexpr std::int32_t kByteBase = 1000;
inline constexpr std::int32_t kStopImEnd = 130073;
inline constexpr std::int32_t kStopImStart = 130072;

/****************************************************************************
 * Name: ByteCodec
 *
 * Description:
 *   Stands in for the vocabulary only.  The request still goes through the
 *   real chat template and the output through the real parser; what is
 *   replaced is the mapping between text and ids, which needs a 10 MB
 *   third-party file this repository does not carry.  The prompt is counted
 *   at roughly four bytes a token, which keeps a request with a tool list
 *   inside the 2048-token window the way the real tokenizer does.
 *
 ****************************************************************************/

class ByteCodec final : public nyamp::ChatCodec
{
public:
  bool Encode(std::string_view request_json, bool guard_untrusted,
              std::vector<std::int32_t> *ids, std::string *error) override
  {
    nyamp::Json request;
    nyamp::RenderedPrompt prompt;

    last_guard = guard_untrusted;
    if (!nyamp::json_parse(request_json, &request, error) ||
        !nyamp::render_chat_prompt(request, nyamp::ChatTemplateOptions(),
                                   &prompt, error))
      {
        return false;
      }

    last_prompt = prompt.text;
    ids->assign((prompt.text.size() + 3) / 4, 7);
    return true;
  }

  std::string Decode(const std::vector<std::int32_t> &ids) override
  {
    std::string text;
    for (std::int32_t id : ids)
      {
        text.push_back(static_cast<char>(id - kByteBase));
      }

    return text;
  }

  /* Emits whole UTF-8 characters only, like the real stream decoder. */

  void StreamReset() override { pending_.clear(); }

  std::string StreamPush(std::int32_t id) override
  {
    pending_.push_back(static_cast<char>(id - kByteBase));

    const unsigned char lead = static_cast<unsigned char>(pending_[0]);
    const std::size_t need = lead < 0x80   ? 1
                             : lead < 0xe0 ? 2
                             : lead < 0xf0 ? 3
                                           : 4;
    if (pending_.size() < need)
      {
        return std::string();
      }

    std::string done;
    done.swap(pending_);
    return done;
  }

  std::string StreamFlush() override
  {
    std::string done;
    done.swap(pending_);
    return done;
  }

  static inline std::string last_prompt;
  static inline bool last_guard = false;

private:
  std::string pending_;
};

/* What the scripted backend does on its next Run. */

struct Script
{
  std::string output; /* Spelled as byte ids.                     */
  std::int32_t stop_id = kStopImEnd; /* 0: end without reporting a stop.   */
  bool endless = false; /* Keep emitting 'a' until stopped.         */
  int delay_ms = 0;     /* Per token, to leave room for a cancel.   */

  std::vector<std::int32_t> seen_prompt;
  std::uint32_t seen_max_new_tokens = 0;
  std::atomic<int> emitted{ 0 };
  int runs = 0;
};

class ScriptedBackend final : public nyamp::models::Backend
{
public:
  explicit ScriptedBackend(Script *script) : script_(script) {}

  nyamp::models::Kind kind() const override
  {
    return nyamp::models::Kind::kLlm;
  }

  nyamp::models::Status Load(const std::string &) override
  {
    return nyamp::models::Status::kOk;
  }

  void Unload() override {}

  nyamp::models::Status Run(const nyamp::models::Input &input,
                            const nyamp::models::Emit &emit,
                            const nyamp::models::Stop &stop) override
  {
    const auto *tokens = std::get_if<nyamp::models::LlmInput>(&input);
    if (tokens == nullptr)
      {
        return nyamp::models::Status::kBackendError;
      }

    script_->seen_prompt = tokens->token_ids;
    script_->seen_max_new_tokens = tokens->max_new_tokens;
    script_->emitted = 0;
    ++script_->runs;

    auto send = [&](std::int32_t id) {
      if (script_->delay_ms != 0)
        {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(script_->delay_ms));
        }

      if (stop() || !emit(nyamp::models::TokenChunk{ id, "?" }))
        {
          return false;
        }

      ++script_->emitted;
      return true;
    };

    if (script_->endless)
      {
        for (;;)
          {
            if (!send(kByteBase + 'a'))
              {
                return nyamp::models::Status::kCancelled;
              }
          }
      }

    for (unsigned char byte : script_->output)
      {
        if (!send(kByteBase + byte))
          {
            return nyamp::models::Status::kCancelled;
          }
      }

    if (script_->stop_id != 0 && !send(script_->stop_id))
      {
        return nyamp::models::Status::kCancelled;
      }

    return nyamp::models::Status::kOk;
  }

private:
  Script *script_;
};

} // namespace nyamp::testing

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_TEST_SUPPORT_H */
