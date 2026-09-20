/****************************************************************************
 * tools/amp/nyampd/nyampd_blob_test.cpp
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

#include "nyampd_blob.h"
#include "nyampd_core.h"
#include "nyampd_llm.h"
#include "nyampd_provision.h"
#include "nyampd_sha256.h"
#include "nyampd_test_support.h"

#include "nyamp_protocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
#include <vector>

#define CHECK(expression)                                                 \
  do                                                                      \
    {                                                                     \
      if (!(expression))                                                  \
        {                                                                 \
          std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
                       #expression);                                      \
          return 1;                                                       \
        }                                                                 \
    }                                                                     \
  while (0)

namespace
{

using namespace nyamp::testing;

int TestSha256Vectors()
{
  std::uint8_t digest[nyamp::kSha256Size];
  nyamp::Sha256 empty;
  nyamp::Sha256 abc;
  nyamp::Sha256 million;

  empty.Final(digest);
  CHECK(nyamp::Sha256Hex(digest) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  abc.Update("abc", 3);
  abc.Final(digest);
  CHECK(nyamp::Sha256Hex(digest) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  /* Fed in ragged pieces so every buffering branch is crossed, including
   * the padding that spills into a second block.
   */
  const std::string piece(1000, 'a');
  for (int index = 0; index < 1000; ++index)
    {
      million.Update(piece.data(), 1 + (index % 7));
      million.Update(piece.data(), piece.size() - 1 - (index % 7));
    }

  million.Final(digest);
  CHECK(nyamp::Sha256Hex(digest) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  /* The 56-byte FIPS 180 message: its padding does not fit the first block,
   * which is the branch a length-handling mistake hides in.
   */
  nyamp::Sha256 boundary;
  const std::string fifty_six =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(fifty_six.size() == 56);
  boundary.Update(fifty_six.data(), fifty_six.size());
  boundary.Final(digest);
  CHECK(nyamp::Sha256Hex(digest) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  return 0;
}

int TestExactReassembly()
{
  /* Three full 1 MiB windows and a ragged tail, the production geometry. */
  Rig rig(1U << 20);
  const std::vector<std::uint8_t> model = Noise((3U << 20) + 12345, 7);
  nyamp::BlobStats stats;
  std::string path;
  std::uint64_t last_done = 0;
  int reports = 0;

  rig.responder.files["llm/model.rkllm"] = model;
  rig.responder.files["llm/empty.bin"] = {};

  auto progress = [&](const nyamp::BlobProgress &update) {
    ++reports;
    last_done = update.done;
  };

  CHECK(rig.client->Fetch("llm/model.rkllm", &path, progress, &stats) ==
        nyamp::BlobResult::kOk);
  CHECK(path == rig.root + "/llm/model.rkllm");
  CHECK(rig.Read("llm/model.rkllm") == model);
  CHECK(rig.Exists("llm/model.rkllm.sha256"));
  CHECK(!rig.Exists("llm/model.rkllm.part"));
  CHECK(rig.responder.reads == 4 && reports == 4 && last_done == model.size());
  CHECK(stats.files == 1 && stats.reused == 0 && stats.bytes == model.size());
  CHECK(rig.responder.open_blobs.empty() && rig.responder.closes == 1);
  CHECK(rig.responder.malformed == 0);

  /* A second request for the same digest moves no bytes at all. */
  CHECK(rig.client->Fetch("llm/model.rkllm", &path, progress, &stats) ==
        nyamp::BlobResult::kOk);
  CHECK(rig.responder.reads == 4 && stats.reused == 1 && stats.files == 2);
  CHECK(rig.responder.open_blobs.empty());

  /* A changed file on the control domain is a different digest, so the
   * local copy is refreshed rather than trusted.
   */
  std::vector<std::uint8_t> updated = model;
  updated[1000] ^= 0xff;
  rig.responder.files["llm/model.rkllm"] = updated;
  CHECK(rig.client->Fetch("llm/model.rkllm", &path, nullptr, &stats) ==
        nyamp::BlobResult::kOk);
  CHECK(rig.responder.reads == 8 && rig.Read("llm/model.rkllm") == updated);

  /* A marker beside the wrong bytes must not be believed: size gates it. */
  {
    std::ofstream truncate(rig.root + "/llm/model.rkllm",
                           std::ios::binary | std::ios::trunc);
    truncate << "short";
  }
  CHECK(rig.client->Fetch("llm/model.rkllm", &path, nullptr, &stats) ==
        nyamp::BlobResult::kOk);
  CHECK(rig.responder.reads == 12 && rig.Read("llm/model.rkllm") == updated);

  /* An empty file is a legal blob and needs no window at all. */
  CHECK(rig.client->Fetch("llm/empty.bin", &path, nullptr, &stats) ==
        nyamp::BlobResult::kOk);
  CHECK(rig.Exists("llm/empty.bin") && rig.Read("llm/empty.bin").empty());
  CHECK(rig.responder.reads == 12 && rig.responder.open_blobs.empty());
  return 0;
}

int TestDigestMismatch()
{
  /* One flipped bit in one window, as a stale or torn mapping would give. */
  {
    Rig rig(4096);
    std::string path = "untouched";

    rig.responder.files["tts/voice.onnx"] = Noise(4096 * 5 + 17, 11);
    rig.responder.corrupt_read = 2;
    CHECK(rig.client->Fetch("tts/voice.onnx", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kDigest);
    CHECK(path == "untouched");
    CHECK(!rig.Exists("tts/voice.onnx"));
    CHECK(!rig.Exists("tts/voice.onnx.part"));
    CHECK(!rig.Exists("tts/voice.onnx.sha256"));
    CHECK(rig.responder.open_blobs.empty());

    /* The failure is not sticky: a clean retry succeeds. */
    rig.responder.corrupt_read = -1;
    CHECK(rig.client->Fetch("tts/voice.onnx", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kOk);
  }

  /* The bytes are intact but OPEN announced another digest. */
  {
    Rig rig(4096);
    std::string path;

    rig.responder.files["tts/voice.onnx"] = Noise(9000, 12);
    rig.responder.lie_about_digest = true;
    CHECK(rig.client->Fetch("tts/voice.onnx", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kDigest);
    CHECK(!rig.Exists("tts/voice.onnx"));
  }

  /* A refresh that fails must not leave the old marker vouching for it. */
  {
    Rig rig(4096);
    std::string path;

    rig.responder.files["a.bin"] = Noise(10000, 13);
    CHECK(rig.client->Fetch("a.bin", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kOk);
    rig.responder.files["a.bin"] = Noise(10000, 14);
    rig.responder.corrupt_read = rig.responder.reads + 1;
    CHECK(rig.client->Fetch("a.bin", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kDigest);
    CHECK(!rig.Exists("a.bin.sha256"));
  }

  return 0;
}

int TestShortReads()
{
  Rig rig(4096);
  const std::vector<std::uint8_t> model = Noise(4096 * 3 + 100, 21);
  std::string path;
  std::uint64_t previous = 0;
  bool monotonic = true;

  /* The responder may fill less than the window (a FAT cluster boundary, a
   * busy bus).  The client must continue from where the bytes stopped, not
   * from where the window would have ended.
   */
  rig.responder.files["asr/ENC.ONX"] = model;
  rig.responder.max_read = 1000;

  auto progress = [&](const nyamp::BlobProgress &update) {
    monotonic = monotonic && update.done > previous &&
                update.done - previous <= 1000 && update.total == model.size();
    previous = update.done;
  };

  CHECK(rig.client->Fetch("asr/ENC.ONX", &path, progress, nullptr) ==
        nyamp::BlobResult::kOk);
  CHECK(monotonic && previous == model.size());
  CHECK(rig.Read("asr/ENC.ONX") == model);
  CHECK(rig.responder.reads == 13);

  /* End of file before the announced size means the file changed under the
   * transfer; hashing what arrived would only report it less clearly.
   */
  Rig shrunk(4096);
  shrunk.responder.files["asr/ENC.ONX"] = model;
  shrunk.responder.eof_after_reads = 1;
  CHECK(shrunk.client->Fetch("asr/ENC.ONX", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kSize);
  CHECK(!shrunk.Exists("asr/ENC.ONX") && !shrunk.Exists("asr/ENC.ONX.part"));
  CHECK(shrunk.responder.open_blobs.empty());
  return 0;
}

int TestStaleGeneration()
{
  std::string path;

  /* The responder learned a newer generation mid-transfer: this daemon is
   * the stale one and must stop, keeping nothing.
   */
  {
    Rig rig(4096);

    rig.responder.files["llm/model.rkllm"] = Noise(4096 * 6, 31);
    rig.responder.stale_after_reads = 2;
    CHECK(rig.client->Fetch("llm/model.rkllm", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kStale);
    CHECK(rig.responder.reads == 2);
    CHECK(!rig.Exists("llm/model.rkllm") &&
          !rig.Exists("llm/model.rkllm.part"));

    /* No CLOSE is owed: the responder already dropped every blob. */
    CHECK(rig.responder.closes == 0);
  }

  /* A client from an older generation is refused at OPEN. */
  {
    Rig rig(4096, kGeneration - 1);

    rig.responder.files["llm/model.rkllm"] = Noise(100, 32);
    CHECK(rig.client->Fetch("llm/model.rkllm", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kStale);
    CHECK(rig.responder.reads == 0 && rig.responder.open_blobs.empty());
  }

  /* A response nobody is waiting for -- the late answer to an abandoned
   * request -- must not be taken for the next one.
   */
  {
    Rig rig(4096);
    nyamp_header_s forged{};
    std::uint8_t status[NYAMP_STATUS_SIZE] = {};

    forged.service = NYAMP_SERVICE_BLOB;
    forged.opcode = NYAMP_BLOB_OPEN;
    forged.flags = NYAMP_FLAG_RESPONSE;
    forged.request_id = NYAMP_REQUEST_ID_COMPUTE | 1;
    forged.generation = kGeneration;
    forged.payload_size = sizeof(status);

    /* Nothing is outstanding, so this is dropped without effect. */
    rig.client->OnFrame(forged, status);
    rig.responder.files["x.bin"] = Noise(10, 33);
    CHECK(rig.client->Fetch("x.bin", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kOk);
  }

  return 0;
}

int TestCancel()
{
  std::string path;

  /* Cancel lands between two windows. */
  {
    Rig rig(4096);
    int windows = 0;

    rig.responder.files["llm/model.rkllm"] = Noise(4096 * 20, 41);
    auto progress = [&](const nyamp::BlobProgress &) {
      if (++windows == 3)
        {
          rig.client->Cancel();
        }
    };

    CHECK(rig.client->Fetch("llm/model.rkllm", &path, progress, nullptr) ==
          nyamp::BlobResult::kCancelled);
    CHECK(rig.responder.reads == 3);
    CHECK(!rig.Exists("llm/model.rkllm") &&
          !rig.Exists("llm/model.rkllm.part") &&
          !rig.Exists("llm/model.rkllm.sha256"));

    /* The blob is still closed: a cancelled transfer must not leave a file
     * open on the control domain until the next generation.
     */
    CHECK(rig.responder.closes == 1 && rig.responder.open_blobs.empty());

    /* The flag is sticky until its owner clears it. */
    CHECK(rig.client->Fetch("llm/model.rkllm", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kCancelled);
    rig.client->ClearCancel();
    CHECK(rig.client->Fetch("llm/model.rkllm", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kOk);
  }

  /* Cancel lands while OPEN is still hashing on the other side.  The
   * requester must tell the responder, or the blob would open later with
   * nobody left to close it.
   */
  {
    Rig rig(4096);

    rig.responder.files["llm/model.rkllm"] = Noise(100, 42);
    rig.responder.hold_open = true;

    std::thread canceller([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      rig.client->Cancel();
    });

    const nyamp::BlobResult result =
        rig.client->Fetch("llm/model.rkllm", &path, nullptr, nullptr);
    canceller.join();
    CHECK(result == nyamp::BlobResult::kCancelled);
    CHECK(rig.responder.cancelled_ids.size() == 1 &&
          rig.responder.cancelled_ids[0] == rig.responder.held_open_id &&
          rig.responder.held_open_id != 0);
  }

  /* A responder that never answers is a timeout, and is told the same. */
  {
    Rig rig(4096);

    rig.responder.files["llm/model.rkllm"] = Noise(100, 43);
    rig.responder.hold_open = true;

    nyamp::BlobClient::Options options;
    options.root = rig.root;
    options.open_timeout_ms = 80;
    nyamp::BlobClient impatient(kGeneration, &rig.responder, &rig.responder,
                                options);
    rig.responder.Attach(&impatient);
    CHECK(impatient.Fetch("llm/model.rkllm", &path, nullptr, nullptr) ==
          nyamp::BlobResult::kTimeout);
    CHECK(rig.responder.cancelled_ids.size() == 1);
  }

  return 0;
}

int TestRefusals()
{
  Rig rig(4096);
  std::string path;

  /* An illegal name never reaches the wire. */
  CHECK(rig.client->Fetch("../etc/passwd", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kInvalid);
  CHECK(rig.client->Fetch("/data/models/x", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kInvalid);
  CHECK(rig.client->Fetch("a//b", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kInvalid);
  CHECK(rig.responder.opens == 0);

  CHECK(rig.client->Fetch("llm/missing.rkllm", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kNotFound);

  rig.responder.link_down = true;
  CHECK(rig.client->Fetch("llm/missing.rkllm", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kTransport);
  rig.responder.link_down = false;

  /* Ownership is exclusive and non-blocking. */
  CHECK(rig.client->Acquire());
  CHECK(!rig.client->Acquire());
  rig.client->Release();
  CHECK(rig.client->Acquire());
  rig.client->Release();
  return 0;
}

int TestDirectoryProvisioning()
{
  Rig rig(4096);
  nyamp::ModelProvisioner provisioner(rig.client.get());
  nyamp::BlobStats stats;
  std::string path;

  /* Enough long names that the listing cannot fit one RPMsg payload. */
  for (int index = 0; index < 24; ++index)
    {
      char name[96];
      std::snprintf(name, sizeof(name),
                    "asr/encoder-epoch-99-avg-1.int8.part-%02d.onnx", index);
      rig.responder.files[name] = Noise(5000 + index, 50 + index);
    }

  rig.responder.files["asr/tokens.txt"] = Noise(321, 90);
  rig.responder.files["asr/lang/zh/lexicon.txt"] = Noise(7777, 91);

  CHECK(nyamp::ModelProvisioner::IsLogicalName("asr"));
  CHECK(nyamp::ModelProvisioner::IsLogicalName("llm/model.rkllm"));
  CHECK(!nyamp::ModelProvisioner::IsLogicalName("/data/MEDIA912/MCP5W4.RKL"));
  CHECK(!nyamp::ModelProvisioner::IsLogicalName("../x"));

  CHECK(provisioner.Provide("asr", &path, nullptr, &stats) ==
        nyamp::BlobResult::kOk);
  CHECK(path == rig.root + "/asr");
  CHECK(stats.files == 26 && stats.reused == 0);
  CHECK(rig.responder.lists > 3);
  CHECK(rig.Read("asr/tokens.txt") == rig.responder.files["asr/tokens.txt"]);
  CHECK(rig.Read("asr/lang/zh/lexicon.txt") ==
        rig.responder.files["asr/lang/zh/lexicon.txt"]);
  CHECK(rig.Read("asr/encoder-epoch-99-avg-1.int8.part-23.onnx") ==
        rig.responder.files["asr/encoder-epoch-99-avg-1.int8.part-23.onnx"]);
  CHECK(rig.responder.open_blobs.empty());

  /* "Fetch what is missing": one changed file is the only one that moves. */
  const int reads = rig.responder.reads;
  rig.responder.files["asr/tokens.txt"] = Noise(321, 92);
  stats = nyamp::BlobStats();
  CHECK(provisioner.Provide("asr", &path, nullptr, &stats) ==
        nyamp::BlobResult::kOk);
  CHECK(stats.files == 26 && stats.reused == 25);
  CHECK(rig.responder.reads == reads + 1);

  /* A single file resolves to the file itself. */
  rig.responder.files["llm/model.rkllm"] = Noise(9999, 93);
  CHECK(provisioner.Provide("llm/model.rkllm", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kOk);
  CHECK(path == rig.root + "/llm/model.rkllm");

  CHECK(provisioner.Provide("nothing-here", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kNotFound);

  /* While someone else holds the client the provisioner reports busy
   * instead of queueing behind a transfer that may take minutes.
   */
  CHECK(rig.client->Acquire());
  CHECK(provisioner.Provide("asr", &path, nullptr, nullptr) ==
        nyamp::BlobResult::kBusy);
  rig.client->Release();
  return 0;
}

int TestBench()
{
  Rig rig(1U << 20);
  nyamp_blob_bench_report_s report{};

  CHECK(rig.client->Bench(8, 0, &report) == nyamp::BlobResult::kOk);
  CHECK(report.rounds == 8 && report.window_bytes == (1U << 20));
  CHECK(report.rtt_min_us <= report.rtt_avg_us &&
        report.rtt_avg_us <= report.rtt_max_us);
  CHECK(report.pattern_errors == 0);

  /* A smaller window is honoured, rounded down to whole 4 KiB blocks. */
  CHECK(rig.client->Bench(2, 70000, &report) == nyamp::BlobResult::kOk);
  CHECK(report.window_bytes == 69632 && report.pattern_errors == 0);

  /* A window that keeps showing the previous fill -- what a cacheable
   * mapping of memory another CPU cluster writes uncached would do -- is
   * the fault this benchmark exists to expose.
   */
  Rig stale(1U << 16);
  stale.responder.stale_window = true;
  CHECK(stale.client->Bench(4, 0, &report) == nyamp::BlobResult::kOk);
  CHECK(report.pattern_errors != 0);
  return 0;
}

/* A backend that only records the path it was asked to load. */

class RecordingBackend final : public nyamp::models::Backend
{
public:
  explicit RecordingBackend(std::string *loaded) : loaded_(loaded) {}

  nyamp::models::Kind kind() const override
  {
    return nyamp::models::Kind::kLlm;
  }

  nyamp::models::Status Load(const std::string &path) override
  {
    *loaded_ = path;
    return std::filesystem::exists(path) || path == "/sd/model"
               ? nyamp::models::Status::kOk
               : nyamp::models::Status::kBackendError;
  }

  void Unload() override {}

  nyamp::models::Status Run(const nyamp::models::Input &,
                            const nyamp::models::Emit &,
                            const nyamp::models::Stop &) override
  {
    return nyamp::models::Status::kOk;
  }

private:
  std::string *loaded_;
};

bool WaitForResponse(nyamp::LlmService &llm, std::uint64_t request_id,
                     std::int32_t *status, int *progress_events)
{
  for (int waited = 0; waited < 5000; waited += 2)
    {
      nyamp::Frame frame;
      while (llm.Poll(&frame))
        {
          nyamp_header_s header;
          if (nyamp_header_decode(&header, frame.data, frame.size) !=
                  NYAMP_OK ||
              header.request_id != request_id)
            {
              return false;
            }

          if (header.flags == NYAMP_FLAG_EVENT &&
              header.service == NYAMP_SERVICE_BLOB &&
              header.opcode == NYAMP_BLOB_EVENT_PROGRESS)
            {
              ++*progress_events;
              continue;
            }

          if ((header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_RESPONSE ||
              header.service != NYAMP_SERVICE_LLM ||
              header.opcode != NYAMP_LLM_LOAD)
            {
              return false;
            }

          const std::uint8_t *body = nullptr;
          std::size_t body_size = 0;
          return nyamp_status_decode(status, &body, &body_size,
                                     frame.data + NYAMP_WIRE_HEADER_SIZE,
                                     header.payload_size) == NYAMP_OK &&
                 ((header.flags & NYAMP_FLAG_ERROR) != 0) == (*status != 0);
        }

      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

  return false;
}

int TestLlmLoadProvisioning()
{
  Rig rig(4096);
  nyamp::ModelProvisioner provisioner(rig.client.get());
  std::string loaded;
  nyamp::LlmService llm(
      kGeneration, [] { return 0; },
      [&] { return std::make_unique<RecordingBackend>(&loaded); });
  nyamp_header_s request{};
  bool deferred = true;
  std::int32_t status = 1;
  int progress = 0;

  request.service = NYAMP_SERVICE_LLM;
  request.opcode = NYAMP_LLM_LOAD;
  request.flags = NYAMP_FLAG_REQUEST;
  request.request_id = 0x0000002a00001234ULL;
  request.generation = kGeneration;

  /* Without a provisioner nothing changes, logical name or not. */
  CHECK(llm.BeginLoad("llm/model.rkllm", request, &deferred) ==
        nyamp::models::Status::kBackendError);
  CHECK(!deferred && loaded == "llm/model.rkllm");

  llm.SetProvisioner(&provisioner);

  /* Chat has its own test; without a codec factory a logical-name load is
   * the model alone, which is what this test is about.
   */
  llm.SetChatCodecFactory(nyamp::ChatCodecFactory());

  /* An absolute path is local storage: untouched, synchronous, no pull. */
  CHECK(llm.BeginLoad("/sd/model", request, &deferred) ==
        nyamp::models::Status::kOk);
  CHECK(!deferred && loaded == "/sd/model" && rig.responder.opens == 0);

  /* A logical name is pulled first, then the backend gets the tmpfs path.
   * The session refuses a load on top of a loaded model, as it always has.
   */
  CHECK(llm.Unload() == nyamp::models::Status::kOk);
  rig.responder.files["llm/model.rkllm"] = Noise(4096 * 4 + 5, 61);
  CHECK(llm.BeginLoad("llm/model.rkllm", request, &deferred) ==
        nyamp::models::Status::kOk);
  CHECK(deferred);
  CHECK(WaitForResponse(llm, request.request_id, &status, &progress));
  CHECK(status == NYAMP_MODEL_OK && progress >= 1);
  CHECK(loaded == rig.root + "/llm/model.rkllm");
  CHECK(rig.Read("llm/model.rkllm") == rig.responder.files["llm/model.rkllm"]);
  CHECK(llm.LastLoadStatus() == nyamp::models::Status::kOk);

  /* A model the control domain does not have is reported, not loaded. */
  CHECK(llm.Unload() == nyamp::models::Status::kOk);
  loaded.clear();
  request.request_id += 1;
  CHECK(llm.BeginLoad("llm/absent.rkllm", request, &deferred) ==
        nyamp::models::Status::kOk);
  CHECK(deferred);
  CHECK(WaitForResponse(llm, request.request_id, &status, &progress));
  CHECK(status == NYAMP_MODEL_NOT_READY && loaded.empty());

  /* A corrupted transfer never reaches the backend. */
  rig.responder.files["llm/bad.rkllm"] = Noise(4096 * 3, 62);
  rig.responder.corrupt_read = rig.responder.reads + 1;
  request.request_id += 1;
  CHECK(llm.BeginLoad("llm/bad.rkllm", request, &deferred) ==
        nyamp::models::Status::kOk);
  CHECK(WaitForResponse(llm, request.request_id, &status, &progress));
  CHECK(status == NYAMP_MODEL_BACKEND_ERROR && loaded.empty());
  CHECK(!rig.Exists("llm/bad.rkllm"));
  return 0;
}

int TestDispatchDirection()
{
  Rig rig(4096);
  nyamp::ModelProvisioner provisioner(rig.client.get());
  nyamp::BlobService blob(kGeneration, &provisioner);
  std::uint8_t wire[NYAMP_RPMSG_MTU] = {};
  std::uint8_t response[NYAMP_RPMSG_MTU] = {};
  std::size_t response_size = 99;
  std::size_t size = 0;
  nyamp_header_s header{};
  nyamp_header_s answer;

  /* A response is never answered, even a malformed-looking one: with a
   * responder on each side that would be a loop.
   */
  header.service = NYAMP_SERVICE_BLOB;
  header.opcode = NYAMP_BLOB_READ;
  header.flags = NYAMP_FLAG_RESPONSE | NYAMP_FLAG_ERROR;
  header.request_id = NYAMP_REQUEST_ID_COMPUTE | 9;
  header.generation = kGeneration;
  header.payload_size = 4;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  CHECK(nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE + 4, 0, kGeneration,
                        response, sizeof(response), &response_size, {},
                        nullptr, &blob) == NYAMP_OK);
  CHECK(response_size == 0);

  /* OPEN arriving here has the direction backwards and is refused. */
  header.flags = NYAMP_FLAG_REQUEST;
  header.opcode = NYAMP_BLOB_OPEN;
  header.request_id = 77;
  CHECK(nyamp_blob_open_encode(wire + NYAMP_WIRE_HEADER_SIZE, NYAMP_INLINE_MAX,
                               &size, 0, "a.bin", 5) == NYAMP_OK);
  header.payload_size = static_cast<std::uint32_t>(size);
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  CHECK(nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE + size, 0, kGeneration,
                        response, sizeof(response), &response_size, {},
                        nullptr, &blob) == NYAMP_OK);
  CHECK(nyamp_header_decode(&answer, response, response_size) == NYAMP_OK);
  CHECK((answer.flags & NYAMP_FLAG_ERROR) != 0 && answer.request_id == 77);

  /* PULL is accepted, deferred, and answered from the worker. */
  rig.responder.files["a.bin"] = Noise(4096 * 2 + 1, 71);
  header.opcode = NYAMP_BLOB_PULL;
  header.request_id = 78;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  CHECK(nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE + size, 0, kGeneration,
                        response, sizeof(response), &response_size, {},
                        nullptr, &blob) == NYAMP_OK);
  CHECK(response_size == 0);

  bool answered = false;
  for (int waited = 0; waited < 5000 && !answered; waited += 2)
    {
      nyamp::Frame frame;
      while (blob.Poll(&frame))
        {
          CHECK(nyamp_header_decode(&answer, frame.data, frame.size) ==
                NYAMP_OK);
          CHECK(answer.request_id == 78 &&
                answer.service == NYAMP_SERVICE_BLOB);
          if (answer.flags == NYAMP_FLAG_EVENT)
            {
              CHECK(answer.opcode == NYAMP_BLOB_EVENT_PROGRESS);
              continue;
            }

          std::int32_t status = 1;
          const std::uint8_t *body = nullptr;
          std::size_t body_size = 0;
          nyamp_blob_pull_report_s report{};

          CHECK(answer.flags == NYAMP_FLAG_RESPONSE &&
                answer.opcode == NYAMP_BLOB_PULL);
          CHECK(nyamp_status_decode(&status, &body, &body_size,
                                    frame.data + NYAMP_WIRE_HEADER_SIZE,
                                    answer.payload_size) == NYAMP_OK);
          CHECK(status == NYAMP_MODEL_OK);
          CHECK(nyamp_blob_pull_report_decode(&report, body, body_size) ==
                NYAMP_OK);
          CHECK(report.bytes == 4096 * 2 + 1 && report.files == 1 &&
                report.reused == 0);
          answered = true;
        }

      if (!answered)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

  CHECK(answered);
  CHECK(rig.Read("a.bin") == rig.responder.files["a.bin"]);

  /* Health advertises the capability only when the service is attached. */
  header.service = NYAMP_SERVICE_HEALTH;
  header.opcode = nyamp::kHealthQuery;
  header.payload_size = 0;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  CHECK(nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE, 0, kGeneration, response,
                        sizeof(response), &response_size, {}, nullptr,
                        &blob) == NYAMP_OK);
  CHECK((response[NYAMP_WIRE_HEADER_SIZE + 8] & 0x4) != 0);
  CHECK(nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE, 0, kGeneration, response,
                        sizeof(response), &response_size, {}, nullptr,
                        nullptr) == NYAMP_OK);
  CHECK((response[NYAMP_WIRE_HEADER_SIZE + 8] & 0x4) == 0);
  return 0;
}

} // namespace

int main()
{
  struct
  {
    const char *name;
    int (*run)();
  } const tests[] = {
    { "sha256 vectors", TestSha256Vectors },
    { "exact reassembly and reuse", TestExactReassembly },
    { "digest mismatch", TestDigestMismatch },
    { "short reads and shrinking file", TestShortReads },
    { "stale generation", TestStaleGeneration },
    { "cancel", TestCancel },
    { "refusals", TestRefusals },
    { "directory provisioning", TestDirectoryProvisioning },
    { "bench", TestBench },
    { "llm load provisioning", TestLlmLoadProvisioning },
    { "dispatch direction", TestDispatchDirection },
  };

  int passed = 0;
  for (const auto &test : tests)
    {
      if (test.run() != 0)
        {
          std::fprintf(stderr, "FAILED: %s\n", test.name);
          return 1;
        }

      std::printf("ok: %s\n", test.name);
      ++passed;
    }

  std::printf("nyampd blob tests passed (%d groups)\n", passed);
  return 0;
}
