# NYAMP wire additions for wake word and owner voiceprint (PROPOSAL)

Status: proposal, not implemented in `tools/amp/protocol/`. Nothing here changes an existing
message. It is written against `nyamp_protocol.h` wire version 1 (40-byte header, 496-byte
RPMsg MTU, `NYAMP_INLINE_MAX` = 456, shared-buffer descriptor "NYBS", services 1-8).

Service id 9 is reserved by another design for BLOB (model delivery), so this proposal takes:

```c
NYAMP_SERVICE_KWS     = 10,
NYAMP_SERVICE_SPEAKER = 11,
```

All integers are little-endian and encoded field by field, as everywhere else in NYAMP.
Status values are `nyamp_model_status_e`. `generation`, `request_id`, `deadline_ms` and the
REQUEST / RESPONSE / EVENT / CANCEL / ERROR flags keep their existing meaning.

## 1. Why the audio is owned by the KWS service

The ASR service receives audio only between `ASR_BEGIN` and its final window, because an ASR
request is one utterance of at most 60 s. A wake word listener has no utterances: it must
hear everything, including the moments when no ASR request exists. If KWS rode on ASR
windows it would be deaf exactly when it is needed.

So the always-on stream belongs to KWS, and ASR and SPEAKER read from it:

- The control domain pushes every captured window **once**, to KWS.
- The compute domain keeps the last `NYAMP_KWS_RING_SECONDS` (proposed: 10 s, 640 KiB of
  float32, in ordinary Linux memory, not in the 0x47C00000 shared region) in a ring indexed
  by an absolute `stream_sample` counter.
- ASR can **attach** to that stream from a given sample offset instead of being pushed
  audio (section 3). The first command after the wake word is therefore never lost and never
  sent twice.
- SPEAKER enrols and verifies on a **sample range** of the same ring (section 4); it has no
  audio ingress of its own.

`stream_sample` is a `uint64` count of samples since `KWS_BEGIN`. At 16 kHz it does not wrap
in the lifetime of the device, which avoids every wrap rule a `uint32` would need.

## 2. KWS service (id 10)

| Opcode | Name | Kind | Payload |
|---|---|---|---|
| 1 | `NYAMP_KWS_LOAD` | request | model directory, UTF-8, <= `NYAMP_INLINE_MAX` |
| 2 | `NYAMP_KWS_UNLOAD` | request | none |
| 3 | `NYAMP_KWS_BEGIN` | request | 16 B, see below; response carries one NYBS descriptor |
| 4 | `NYAMP_KWS_PUSH` | request | NYBS descriptor + 16 B |
| 5 | `NYAMP_KWS_RELEASE` | request | NYBS descriptor |
| 6 | `NYAMP_KWS_END` | request | none; stops the stream, ring is dropped |
| 7 | `NYAMP_KWS_LIST` | request | none; response lists keyword labels |
| 0x80 | `NYAMP_KWS_EVENT_DETECTED` | event | 40 B + label |
| 0x81 | `NYAMP_KWS_EVENT_FINISH` | event | status i32, sequence u32 |

The model directory holds `encoder.onnx`, `decoder.onnx`, `joiner.onnx`, `tokens.txt` and
`keywords.txt`. Keywords are part of the verified model directory rather than a wire
message: they are tuned together with the threshold (see README.md) and must not drift from
it. Every keywords line ends in `@label`; the **keyword id** is the zero-based index of the
label among the distinct labels in file order, so pronunciation variants that share a label
share an id.

`KWS_BEGIN` (16 B), same shape as `ASR_BEGIN` so one encoder can serve both:

```
u32 sample_rate      must be 16000
u16 channels         must be 1
u16 flags            bit0 NYAMP_KWS_BEGIN_F32 (else S16)
u32 window_samples   fixed window size, 1600..16000
u32 reserved         0
```

`KWS_PUSH` (`NYAMP_BUFFER_SIZE` + 16 B). The grant/lease rules are exactly those of
`ASR_PUSH`: the compute domain mints the lease, a published window is not written again
until it is released.

```
nyamp_buffer_s buffer
u32 sequence         +1 per window; a gap makes the compute domain reset the decoder
u16 flags            bit0 NYAMP_KWS_PUSH_DISCONTINUITY (capture gap: reset, keep counting)
u16 reserved         0
u64 stream_sample    offset of the first sample of this window
```

`stream_sample` is sent although the compute domain could count it, because after a
dropped window both sides must still agree on what "sample N" means; a PUSH whose
`stream_sample` is not the expected value is treated as a discontinuity, not an error.

The PUSH response is sent as soon as the window has been copied into the ring, before it is
decoded, so the control domain's capture loop is never blocked by inference.

`KWS_EVENT_DETECTED` (40 B + label):

```
u32 sequence         +1 per event of this stream
u16 keyword_id       0xffff = label not in keywords.txt (should not happen)
u16 flags            bit0 HAS_OFFSETS, bit1 HAS_SCORE
u32 score            float bits; valid only with HAS_SCORE
u32 label_length
u64 start_sample     first token of the phrase      (HAS_OFFSETS)
u64 end_sample       one frame past the last token  (HAS_OFFSETS)
u64 trigger_sample   stream position when the trigger fired; always valid
u8  label[label_length]   UTF-8, not NUL terminated
```

`HAS_SCORE` is never set today: sherpa-onnx 1.13.8 applies `keywords_threshold` internally
and its C API does not export the acoustic probability. The field exists so the event does
not change shape when a runtime that exposes it is adopted.

Offsets have 40 ms resolution (decoder frame). `trigger_sample - end_sample` is the
detection latency and was 0.3-0.6 s on the host.

Events carry the `request_id` of the `KWS_BEGIN` they belong to. A control domain that
cannot take an event (RPMsg back-pressure) loses nothing but that event; the stream keeps
running. `KWS_EVENT_FINISH` is sent once, after `KWS_END`, `KWS_UNLOAD`, a CANCEL of the
BEGIN request, or a backend error.

`KWS_LIST` response: `u16 count`, `u16 reserved`, then per label `u16 length` + UTF-8.

## 3. ASR attach (addition to service 3, no existing message changes)

```c
#define NYAMP_ASR_BEGIN_ATTACH_KWS (1U << 0)   /* in ASR_BEGIN flags          */
#define NYAMP_ASR_BEGIN_ATTACH_SIZE 24U        /* 16 B + u64 start_sample     */
#define NYAMP_ASR_END               7U         /* payload: u64 end_sample     */
```

With `ATTACH_KWS` set, `ASR_BEGIN` is 24 bytes and ends with the `stream_sample` at which
recognition starts (normally `end_sample` of the DETECTED event, so the wake phrase itself is
not transcribed). The response carries no grant, `ASR_PUSH` is refused with
`NYAMP_MODEL_INVALID`, and every later KWS window feeds both consumers. `ASR_END` names the
last sample and produces the usual `EVENT_FINISH`. A `start_sample` older than the ring
retention is `NYAMP_MODEL_INVALID`; one in the future is accepted and simply waits.

Without the flag, `ASR_BEGIN` stays 16 bytes and behaves exactly as today.

## 4. SPEAKER service (id 11)

Division of state, as decided by the owner:

- The **control domain (NuttX)** owns the voiceprints. It persists them under
  `/config/voice/owner-<slot>.nyvp` and is the only side that survives a reboot.
- The **compute domain (Linux)** is stateless across reboots. Within one boot it keeps a
  cache of the voiceprints it was given, so a verification at every wake word does not have
  to carry 400 bytes and re-parse them. After any `generation` change the cache is empty and
  the control domain re-sends its store.

Voiceprint container ("NYVP", `nyamp_speaker.h`): 16-byte header + `dim` IEEE half floats.
For CAM++ (`dim` = 192) that is **400 bytes, which fits one RPMsg payload**; float32 would
be 784 bytes and would need either chunking or a shared-region lease for what is a tiny,
rarely moved object. The measured cosine between a float32 voiceprint and its half-precision
round trip is 1.000000 (README.md). `model_tag` in the header identifies the embedding model;
a voiceprint with another tag is refused with `NYAMP_MODEL_UNSUPPORTED`, never scored.

| Opcode | Name | Payload | Response payload |
|---|---|---|---|
| 1 | `NYAMP_SPK_LOAD` | model directory (`speaker.onnx`) | status, `u16 dim`, `u16 reserved`, `u32 model_tag` |
| 2 | `NYAMP_SPK_UNLOAD` | none | status (cache dropped) |
| 3 | `NYAMP_SPK_ENROL_BEGIN` | 12 B | status |
| 4 | `NYAMP_SPK_ENROL_ADD` | 16 B | 16 B |
| 5 | `NYAMP_SPK_ENROL_COMMIT` | `u16 slot`, `u16 reserved` | 12 B + NYVP |
| 6 | `NYAMP_SPK_ENROL_ABORT` | none | status |
| 7 | `NYAMP_SPK_TEMPLATE_PUT` | `u16 slot`, `u16 reserved`, NYVP | status |
| 8 | `NYAMP_SPK_DELETE` | `u16 slot` (0xffff = all), `u16 reserved` | status |
| 9 | `NYAMP_SPK_LIST` | none | `u16 count`, `u16 reserved`, 12 B per slot |
| 10 | `NYAMP_SPK_VERIFY` | 20 B | 16 B |

Every SPEAKER message is request/response; one embedding is a single short inference
(25-70 ms on the x86 host, to be measured on the board), so no event stream is needed and
CANCEL uses the normal header rule (`payload_size == 0`, target in `request_id`).

`ENROL_BEGIN` (12 B): `u8 min_kept` (default 3), `u8 max_takes` (default 8, hard limit 8),
`u16 reserved`, `u32 min_mean_cosine` (float bits, default 0.45), `u32 reserved`. One
enrolment dialogue at a time; a second BEGIN is `NYAMP_MODEL_BUSY`.

`ENROL_ADD` (16 B): `u64 start_sample`, `u64 end_sample` -- a range of the KWS ring, normally
copied from a DETECTED event. Response (16 B): `i32 status`, `u16 take_index`, `u16 takes`,
`u32 agreement` (float bits: cosine of this take to the mean of the earlier ones, -2 for the
first), `u32 speech_samples`. `agreement` lets the UI say "please repeat" immediately instead
of only failing at COMMIT. Ranges shorter than 0.5 s are `NYAMP_MODEL_INVALID`.

`ENROL_COMMIT` response (12 B + NYVP): `i32 status`, `u16 kept_mask` (bit n = take n was
used), `u16 kept`, `u32 cohesion` (float bits), then the voiceprint. Outlier takes are removed
one at a time, worst first; if fewer than `min_kept` consistent takes remain the status is
`NYAMP_MODEL_INVALID` and no voiceprint is returned. On success the voiceprint is also placed
in the cache under `slot`, so the control domain does not have to PUT it back.

`TEMPLATE_PUT` loads one stored voiceprint into the cache (boot, or after a generation
change). `DELETE` removes it from the cache; the control domain deletes its file first and
then sends DELETE, so a crash in between leaves the voiceprint gone from storage, which is
the safe direction. `LIST` reports what the compute domain currently holds -- per slot
`u16 slot`, `u16 segments`, `u32 model_tag`, `u32 crc32` of the NYVP bytes -- so the control
domain can reconcile its store with the cache instead of assuming.

`VERIFY` (20 B): `u64 start_sample`, `u64 end_sample`, `u16 slot_mask` (bit n = compare with
slot n; up to 16 owners), `u16 reserved`. Response (16 B): `i32 status`, `u16 best_slot`
(0xffff if none), `u16 reserved`, `u32 score` (float bits, cosine of the best slot),
`u32 speech_samples`. **The response is a score, not a decision.** The threshold lives in the
control domain because it is product policy (how much to trust the owner for which action),
it must be tunable without a compute-domain update, and it has to be re-derived from real
recordings (README.md).

## 5. Privacy and failure rules

- A voiceprint is biometric personal data. It never leaves the device, is stored only under
  `/config/voice/`, and "forget me" must delete the file and send `SPK_DELETE`.
- The ring holds at most 10 s of audio, only in compute-domain RAM, and is dropped on
  `KWS_END`, `KWS_UNLOAD` and on every generation change.
- A compute-domain restart is visible as a generation change: the control domain re-issues
  `KWS_LOAD`/`KWS_BEGIN`, `SPK_LOAD` and one `TEMPLATE_PUT` per stored slot. Until that is
  done VERIFY answers `NYAMP_MODEL_NOT_READY` and the product must treat the speaker as
  "unknown", never as "owner".
- Speaker verification on a 1-2 s wake phrase is a convenience signal (personalisation,
  "only the owner may change settings by voice"), not an authentication factor; the measured
  error rates in README.md make that explicit.

## 6. What this proposal needs from `tools/amp/protocol`

Encoders/decoders in the existing style (`nyamp_kws_begin_*`, `nyamp_kws_push_*`,
`nyamp_kws_detected_*`, `nyamp_spk_enrol_add_*`, `nyamp_spk_verify_*`, ...), their size
macros, the two service ids, the `ASR_BEGIN` attach variant, and round-trip plus truncation
tests in `nyamp_protocol_test.c`. The C++ classes in this directory already produce every
field these messages carry.
