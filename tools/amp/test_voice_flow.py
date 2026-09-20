#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive ASR, TTS and the wake word over the wire, from outside the process.

The service side is nyampd's Dispatch with the real AsrService, TtsService and
KwsService, served over a SOCK_SEQPACKET socket pair (message boundaries, as
RPMsg has them).  The shared region is a 4 MiB file both sides map, laid out
by chips/rk3576/include/rk3576_shmem_layout.h, so descriptors carry the
offsets they carry on the board.

The client is this script, and it shares no code with the service: every
frame is packed and unpacked here with `struct`, from the layouts documented
in tools/amp/protocol/README.md.  A field that the C codec and that document
disagree on fails here, which is the point -- the control-domain client has
to be written from the same document.

Only the models are stood in for (nyampd_speech_test_support.h): a scripted
recognizer, a scripted spotter that fires on a marker sample, a scripted text
front end and vocoder.  So this proves the wire flow -- grants and leases,
windows through shared memory, incremental text, PCM windows with exact
valid_samples, one window outstanding, cancel -- and claims no inference.

usage: test_voice_flow.py <repo>
"""

import mmap
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import tempfile

repo = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(tempfile.mkdtemp(prefix="amp-voice-flow-"))
amp = repo / "tools/amp"
nyampd = amp / "nyampd"
layout = repo / "chips/rk3576/include/rk3576_shmem_layout.h"

# ---------------------------------------------------------------------------
# The arena layout, read from the header the firmware uses.
# ---------------------------------------------------------------------------

defines = dict(re.findall(r"#define\s+(NYAMP_\w+)\s+(0x[0-9a-fA-F]+)U",
                          layout.read_text(encoding="utf-8")))
SLOT_SHARED = int(defines["NYAMP_SLOT_SHARED"], 16)
SLOT_SHARED_SIZE = int(defines["NYAMP_SLOT_SHARED_SIZE"], 16)
SLOT_CAPTURE = int(defines["NYAMP_SLOT_CAPTURE"], 16)
SLOT_CAPTURE_SIZE = int(defines["NYAMP_SLOT_CAPTURE_SIZE"], 16)
ARENA_SIZE = 4 * 1024 * 1024
assert SLOT_CAPTURE >= SLOT_SHARED + SLOT_SHARED_SIZE
assert SLOT_CAPTURE + SLOT_CAPTURE_SIZE <= ARENA_SIZE

GENERATION = 0x5A17F10E

(root / "harness.cpp").write_text(r'''
#include "nyampd_speech_test_support.h"

#include <cstdlib>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace nyamp::testing;

namespace
{

class FileSlot final : public nyamp::SharedSlot
{
public:
  FileSlot(std::uint8_t *arena, std::uint32_t offset, std::uint32_t capacity)
      : arena_(arena), offset_(offset), capacity_(capacity)
  {
  }

  std::uint8_t *data() override { return arena_ + offset_; }
  std::uint32_t offset() const override { return offset_; }
  std::uint32_t capacity() const override { return capacity_; }

private:
  std::uint8_t *arena_;
  std::uint32_t offset_;
  std::uint32_t capacity_;
};

} // namespace

int main(int argc, char **argv)
{
  if (argc != 8)
    {
      return 2;
    }

  const int fd = std::atoi(argv[1]);
  const int arena_fd = open(argv[2], O_RDWR);
  const std::uint32_t generation =
      static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 0));
  const std::uint32_t shared = std::strtoul(argv[4], nullptr, 0);
  const std::uint32_t shared_size = std::strtoul(argv[5], nullptr, 0);
  const std::uint32_t capture = std::strtoul(argv[6], nullptr, 0);
  const std::uint32_t capture_size = std::strtoul(argv[7], nullptr, 0);
  void *mapping = mmap(nullptr, 4 * 1024 * 1024, PROT_READ | PROT_WRITE,
                       MAP_SHARED, arena_fd, 0);
  if (arena_fd < 0 || mapping == MAP_FAILED)
    {
      return 3;
    }

  std::uint8_t *arena = static_cast<std::uint8_t *>(mapping);
  FileSlot capture_slot(arena, capture, capture_size);
  FileSlot speech_slot(arena, shared, shared_size);
  nyamp::AtomicGate capture_gate;
  nyamp::AtomicGate speech_gate;
  nyamp::LeaseMint mint(generation);

  AsrScript asr_script;
  asr_script.steps = { { 16000, "打开", false },
                       { 32000, "打开客厅", false },
                       { 40000, "打开客厅的灯", true } };
  asr_script.final_suffix = "。";
  KwsScript kws_script;
  TtsScript tts_script;

  nyamp::KwsService kws(
      generation,
      [&] { return std::make_unique<ScriptedKwsBackend>(&kws_script); },
      &capture_slot, &capture_gate, &mint);
  nyamp::AsrService asr(
      generation, [] { return std::uint64_t(0); },
      [&] { return std::make_unique<ScriptedAsrBackend>(&asr_script); },
      &capture_slot, &capture_gate, &mint);
  nyamp::TtsService tts(
      generation, [] { return std::uint64_t(0); },
      [&] { return std::make_unique<ScriptedVocoder>(&tts_script); },
      [&] { return std::make_unique<ScriptedFrontend>(&tts_script); },
      &speech_slot, &speech_gate, &mint);
  asr.SetCaptureSource(&kws);
  const nyamp::SpeechServices services{ &asr, &tts, &kws };

  /* nyampd's transport loop, reduced to what the speech services need. */
  for (;;)
    {
      struct pollfd pollfd = { fd, POLLIN, 0 };
      if (poll(&pollfd, 1, 2) > 0)
        {
          if ((pollfd.revents & POLLIN) == 0)
            {
              break; /* The client hung up. */
            }

          std::uint8_t request[NYAMP_RPMSG_MTU];
          std::uint8_t response[NYAMP_RPMSG_MTU];
          std::size_t response_size = 0;
          const ssize_t got = read(fd, request, sizeof(request));
          if (got <= 0)
            {
              break;
            }

          if (nyamp::Dispatch(request, static_cast<std::size_t>(got), 0,
                              generation, response, sizeof(response),
                              &response_size, {}, nullptr, nullptr,
                              &services) == NYAMP_OK &&
              response_size != 0)
            {
              (void)!write(fd, response, response_size);
            }
        }

      nyamp::Frame frame;
      while (kws.Poll(&frame) || tts.Poll(&frame) || asr.Poll(&frame))
        {
          (void)!write(fd, frame.data, frame.size);
        }
    }

  return 0;
}
''', encoding="utf-8")

sources = ["nyampd_core.cpp", "nyampd_llm.cpp", "nyampd_frame.cpp",
           "nyampd_sha256.cpp", "nyampd_blob.cpp", "nyampd_provision.cpp",
           "nyampd_chat.cpp", "nyampd_audio.cpp", "nyampd_loader.cpp",
           "nyampd_asr.cpp", "nyampd_tts.cpp", "nyampd_kws.cpp"]
chat_sources = ["nyamp_json.cpp", "nyamp_unicode.cpp", "nyamp_tokenizer.cpp",
                "nyamp_chat_template.cpp", "nyamp_chat_parse.cpp"]

subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-c",
                str(amp / "protocol/nyamp_protocol.c"),
                "-o", str(root / "protocol.o")], check=True)
subprocess.run(
    ["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
     "-I", str(nyampd), "-I", str(amp / "models"), "-I", str(amp / "protocol"),
     "-I", str(amp / "chat"), "-I", str(amp / "g2p"),
     str(root / "harness.cpp"),
     *[str(nyampd / name) for name in sources],
     *[str(amp / "chat" / name) for name in chat_sources],
     str(amp / "models/nyamp_models.cpp"), str(amp / "g2p/nyamp_g2p.cpp"),
     str(root / "protocol.o"),
     "-o", str(root / "harness"), "-lpthread"],
    check=True)

# ---------------------------------------------------------------------------
# The wire, as tools/amp/protocol/README.md documents it.
# ---------------------------------------------------------------------------

HEADER = struct.Struct("<IHHHHIQQII")          # 40 bytes
NYBS = struct.Struct("<IHHIIIIQII")            # 40 bytes
WIRE_MAGIC = 0x5041594E                        # "NYAP"
NYBS_MAGIC = 0x5342594E                        # "NYBS"
REQUEST, RESPONSE, EVENT, CANCEL, ERROR = 1, 2, 4, 8, 16
ASR, TTS, KWS = 3, 4, 10
BUF_IN_SHMEM, BUF_FROM_COMPUTE, BUF_LAST, BUF_RESYNC = 1, 2, 4, 8
FORMAT_F32 = 1
OK, INVALID, NOT_READY, BUSY, CANCELLED = 0, -1, -2, -3, -6
NOW = 0xFFFFFFFFFFFFFFFF
assert HEADER.size == 40 and NYBS.size == 40

arena_path = root / "arena"
with open(arena_path, "wb") as handle:
    handle.truncate(ARENA_SIZE)
arena_file = open(arena_path, "r+b")
arena = mmap.mmap(arena_file.fileno(), ARENA_SIZE)

ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
ours.settimeout(20)
harness = subprocess.Popen(
    [str(root / "harness"), str(theirs.fileno()), str(arena_path),
     str(GENERATION), str(SLOT_SHARED), str(SLOT_SHARED_SIZE),
     str(SLOT_CAPTURE), str(SLOT_CAPTURE_SIZE)],
    pass_fds=[theirs.fileno()])
theirs.close()

events = []


def send(service, opcode, request_id, payload=b"", flags=REQUEST):
    ours.send(HEADER.pack(WIRE_MAGIC, 1, 40, service, opcode, flags,
                          request_id, 0, GENERATION, len(payload)) + payload)


def receive():
    frame = ours.recv(496)
    (magic, version, size, service, opcode, flags, request_id, _deadline,
     generation, payload_size) = HEADER.unpack(frame[:40])
    assert magic == WIRE_MAGIC and version == 1 and size == 40
    assert generation == GENERATION and len(frame) == 40 + payload_size
    return service, opcode, flags, request_id, frame[40:]


def call(service, opcode, request_id, payload=b""):
    """One request, its response; events that arrive meanwhile are kept."""
    send(service, opcode, request_id, payload)
    while True:
        got = receive()
        if got[2] & RESPONSE and got[:2] == (service, opcode) \
                and got[3] == request_id:
            status = struct.unpack("<i", got[4][:4])[0]
            assert bool(got[2] & ERROR) == (status != 0)
            return status, got[4][4:]
        assert got[2] & EVENT, got
        events.append(got)


def next_event():
    if events:
        return events.pop(0)
    got = receive()
    assert got[2] & EVENT, got
    return got


def unpack_nybs(data):
    (magic, version, flags, offset, length, capacity, fmt, lease, generation,
     reserved) = NYBS.unpack(data[:40])
    assert magic == NYBS_MAGIC and version == 1 and reserved == 0
    assert generation == GENERATION and flags & BUF_IN_SHMEM
    return dict(flags=flags, offset=offset, length=length, capacity=capacity,
                format=fmt, lease=lease)


def pack_nybs(grant, offset, length, flags=BUF_IN_SHMEM):
    return NYBS.pack(NYBS_MAGIC, 1, flags, offset, length, grant["capacity"],
                     grant["format"], grant["lease"], GENERATION, 0)


def audio(count, level, marker_at=None):
    samples = [level] * count
    if marker_at is not None:
        samples[marker_at] = 0.75              # The scripted wake phrase.
    return struct.pack(f"<{count}f", *samples)


# ---------------------------------------------------------------------------
# 1. ASR, pushed: BEGIN -> grant, windows through shared memory, partial
#    text as it grows, END, final text, FINISH.
# ---------------------------------------------------------------------------

assert call(ASR, 3, 100, struct.pack("<IHHII", 16000, 1, 0, 0, 0))[0] \
    == NOT_READY                               # BEGIN before LOAD.
assert call(ASR, 1, 101, b"/models/asr") == (OK, b"")
status, body = call(ASR, 3, 102, struct.pack("<IHHII", 16000, 1, 0, 0, 0))
assert status == OK and len(body) == 40
grant = unpack_nybs(body)
assert grant["offset"] == SLOT_CAPTURE and grant["capacity"] == SLOT_CAPTURE_SIZE
assert grant["format"] == FORMAT_F32 and grant["length"] == 0
assert grant["lease"] >> 32 == GENERATION

text = ""
frames = 0


def take_asr(event, request_id):
    """Apply one PARTIAL frame; return the FINISH status once it arrives."""
    global text, frames
    service, opcode, _flags, got_id, payload = event
    assert service == ASR and got_id == request_id
    if opcode == 0x81:
        status, sequence = struct.unpack("<iI", payload)
        assert sequence == frames
        return status
    assert opcode == 0x80
    sequence, consumed, flags, reserved = struct.unpack("<IIHH", payload[:12])
    assert reserved == 0 and sequence == frames
    frames += 1
    text = payload[12:].decode() if flags & 1 else text + payload[12:].decode()
    take_asr.endpoint |= bool(flags & 2)
    take_asr.final |= bool(flags & 4)
    take_asr.consumed = consumed
    return None


take_asr.endpoint = take_asr.final = False
take_asr.consumed = 0

# 2.5 s in one-second windows, alternating between two ranges of the grant.
for sequence, count in enumerate((16000, 16000, 8000)):
    offset = SLOT_CAPTURE + (sequence % 2) * 64000
    arena[offset:offset + count * 4] = audio(count, 0.125)
    push = pack_nybs(grant, offset, count * 4) + \
        struct.pack("<IHHII", sequence, 0, 0, 0, 0)
    assert call(ASR, 4, 102, push) == (OK, b"")
    arena[offset:offset + count * 4] = b"\xa5" * (count * 4)  # Ours again.

# A window under another lease is refused and changes nothing.
forged = dict(grant, lease=grant["lease"] ^ 1)
assert call(ASR, 4, 102, pack_nybs(forged, SLOT_CAPTURE, 64) +
            struct.pack("<IHHII", 3, 0, 0, 0, 0))[0] == INVALID

assert call(ASR, 7, 102, struct.pack("<Q", NOW)) == (OK, b"")
finish = None
while finish is None:
    finish = take_asr(next_event(), 102)
assert finish == OK and take_asr.final and take_asr.endpoint
assert text == "打开客厅的灯。", text
assert take_asr.consumed == 40000
asr_frames = frames

# ---------------------------------------------------------------------------
# 2. Wake word, then ASR attached to the same stream.
# ---------------------------------------------------------------------------

load = struct.pack("<ffHHHH", 0.0, 0.0, 0, 0, len(b"/models/kws"), 0) + \
    b"/models/kws"
assert call(KWS, 1, 200, load) == (OK, b"")
status, body = call(KWS, 7, 201)               # LIST
count, reserved = struct.unpack("<HH", body[:4])
labels, at = [], 4
for _ in range(count):
    length = struct.unpack("<H", body[at:at + 2])[0]
    labels.append(body[at + 2:at + 2 + length].decode())
    at += 2 + length
assert status == OK and labels == ["nihao_openvela", "hello_openvela"]

status, body = call(KWS, 3, 202, struct.pack("<IHHII", 16000, 1, 0, 16000, 0))
assert status == OK
stream = unpack_nybs(body)
assert stream["offset"] == SLOT_CAPTURE and stream["lease"] != grant["lease"]

# The capture slot has one owner: a pushed ASR request is BUSY meanwhile.
assert call(ASR, 3, 203, struct.pack("<IHHII", 16000, 1, 0, 0, 0))[0] == BUSY

position = 0


def push_stream(data, count, flags=0):
    global position
    index = push_stream.sequence
    offset = SLOT_CAPTURE + (index % 2) * 64000
    arena[offset:offset + count * 4] = data
    status, ack = call(KWS, 4, 202, pack_nybs(stream, offset, count * 4) +
                       struct.pack("<IHHQ", index, flags, 0, position))
    assert status == OK
    position += count
    assert struct.unpack("<Q", ack)[0] == position
    push_stream.sequence += 1


push_stream.sequence = 0
push_stream(audio(16000, 0.01), 16000)
push_stream(audio(16000, 0.01, marker_at=7999), 16000)   # Ends at 24000.

service, opcode, _flags, got_id, payload = next_event()
assert (service, opcode, got_id) == (KWS, 0x80, 202)
(sequence, keyword_id, flags, _score, label_length, start, end,
 trigger) = struct.unpack("<IHHIIQQQ", payload[:40])
assert sequence == 0 and keyword_id == 0 and flags == 1   # HAS_OFFSETS only.
assert payload[40:40 + label_length] == b"nihao_openvela"
assert (start, end) == (16000, 24000) and end <= trigger <= 32000

# The command keeps coming while the control domain reacts...
push_stream(audio(16000, 0.125), 16000)
# ...and recognition starts where the wake phrase ended, from the ring.
text, frames = "", 0
take_asr.endpoint = take_asr.final = False
status, body = call(ASR, 3, 204,
                    struct.pack("<IHHIIQ", 16000, 1, 2, 0, 0, end))  # ATTACH_KWS
assert (status, body) == (OK, b"")
push_stream(audio(16000, 0.125), 16000)
push_stream(audio(16000, 0.125), 16000)

while not take_asr.endpoint:                   # The recognizer's opinion...
    assert take_asr(next_event(), 204) is None
assert call(ASR, 7, 204, struct.pack("<Q", position)) == (OK, b"")  # ...ours.
finish = None
while finish is None:
    finish = take_asr(next_event(), 204)
assert finish == OK and text == "打开客厅的灯。"
assert take_asr.consumed == position - end     # Nothing lost, nothing twice.

# A position that goes back is refused; END stops the stream.
assert call(KWS, 4, 202, pack_nybs(stream, SLOT_CAPTURE, 6400) +
            struct.pack("<IHHQ", push_stream.sequence, 0, 0, 0))[0] == INVALID
assert call(KWS, 6, 205) == (OK, b"")
service, opcode, _flags, got_id, payload = next_event()
assert (service, opcode, got_id) == (KWS, 0x81, 202)
assert struct.unpack("<iI", payload) == (OK, 1)

# ---------------------------------------------------------------------------
# 3. TTS: chunked UTF-8 text in, PCM windows out, one at a time.
# ---------------------------------------------------------------------------

assert call(TTS, 1, 300, b"/models/tts") == (OK, b"")
sentences = ["the first sentence is short.",
             "the second one is a little bit longer than that.",
             "and a third."]
speech = " ".join(sentences).encode()
speech += b" " + b" ".join(b"pad pad pad pad pad pad pad pad pad pad pad." for _ in range(9))
assert len(speech) > 428                        # More than one chunk.


def say(request_id, data, speed=1.0, window=0):
    for offset in range(0, len(data), 428):
        part = data[offset:offset + 428]
        header = struct.pack("<IIIIfII", len(data), offset, len(part), 1,
                             speed, window, 0)
        status, body = call(TTS, 6, request_id, header + part)
        assert body == b""
        if status != OK:
            return status
    return OK


def listen(request_id, on_window=None):
    windows, total = [], 0
    while True:
        service, opcode, _flags, got_id, payload = next_event()
        assert service == TTS and got_id == request_id
        if opcode == 0x81:
            status, sequence, total_samples = struct.unpack("<iII", payload)
            assert sequence == len(windows)
            return status, windows, total_samples
        assert opcode == 0x80 and len(payload) == 56
        window = unpack_nybs(payload)
        sequence, rate, channels, valid = struct.unpack("<IIII", payload[40:])
        assert sequence == len(windows) and rate == 44100 and channels == 1
        assert window["format"] == FORMAT_F32 and window["offset"] == SLOT_SHARED
        assert window["length"] == valid * 4 <= SLOT_SHARED_SIZE
        assert window["flags"] & BUF_FROM_COMPUTE
        samples = struct.unpack_from(f"<{valid}f", arena, window["offset"])
        windows.append(dict(window, valid=valid, first=samples[0],
                            second=samples[1] if valid > 1 else None))
        if on_window is None or on_window(window):
            assert call(TTS, 4, request_id,
                        payload[:40]) == (OK, b"")  # RELEASE echoes it.


assert say(301, speech) == OK
status, windows, total = listen(301)
assert status == OK and total == sum(w["valid"] for w in windows)

# One unit per sentence (8 frames per byte, 512 samples per frame); RESYNC
# opens each unit, LAST closes the request and nothing else.
units = sentences + ["pad pad pad pad pad pad pad pad pad pad pad."] * 9
assert total == sum(len(u) * 8 * 512 for u in units), total
starts = [w for w in windows if w["flags"] & BUF_RESYNC]
assert len(starts) == len(units)
for unit, window in zip(units, starts):
    assert window["first"] == (ord(unit[0]) % 256) / 256.0   # This unit's audio.
assert [bool(w["flags"] & BUF_LAST) for w in windows] == \
    [False] * (len(windows) - 1) + [True]
assert all(w["valid"] <= 44100 for w in windows)
assert len({w["lease"] for w in windows}) == len(windows)
longest = max(len(u) for u in units) * 8
assert longest <= 512                           # No unit over the bucket.

# A second request while one runs is BUSY; a stale lease frees nothing;
# CANCEL voids the outstanding window and ends the request.
seen = {}


def hold_first(window):
    seen["window"] = window
    assert say(303, b"another one.") == BUSY
    stale = dict(window, lease=window["lease"] - 1)
    assert call(TTS, 4, 302, pack_nybs(stale, window["offset"], window["length"],
                                       window["flags"]))[0] == INVALID
    assert call(TTS, 5, 302) == (OK, b"")       # CANCEL
    return False


assert say(302, b"this sentence is cancelled while its first window is out.") == OK
status, windows, total = listen(302, hold_first)
assert status == CANCELLED and len(windows) == 1
late = seen["window"]
assert call(TTS, 4, 302, pack_nybs(late, late["offset"], late["length"],
                                   late["flags"]))[0] == INVALID

# Half speed doubles the frames, so the same text becomes two units.
assert say(304, b"w01 w02 w03 w04 w05 w06 w07 w08 w09 w10 w11 w12.",
           speed=0.5, window=262144) == OK
status, windows, total = listen(304)
assert status == OK and len(windows) == 2 and total == 47 * 16 * 512

ours.close()
assert harness.wait(timeout=20) == 0
print(f"VOICE_WIRE_FLOW_PASS asr_partial_frames={asr_frames} "
      f"kws_detections=1 attach_consumed={position - end} "
      f"tts_units={len(units)} tts_samples={sum(len(u) for u in units) * 4096}")
