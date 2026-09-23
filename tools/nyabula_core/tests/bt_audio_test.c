/* SPDX-License-Identifier: Apache-2.0
 *
 * Host test of the pieces of the Bluetooth audio path that need no board:
 * the SBC door (ny_sbc), the mSBC H2 framing and the sample plumbing
 * (ny_pcm).  Built and run by bt_audio_test.py.
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ny_pcm.h"
#include "ny_sbc.h"

#include "bt_audio_vectors.inc"

/* Golden FNV-1a sums of the decoded vectors.  They pin the decoder output
 * bit for bit; the tone check below is what says the output is also right.
 */

#define GOLDEN_SBC      ((uint32_t)NY_GOLDEN_SBC)
#define GOLDEN_MSBC     ((uint32_t)NY_GOLDEN_MSBC)

#define SBC_FRAME_BYTES 120

static uint32_t fnv1a(const void *data, size_t size, uint32_t sum)
{
  const uint8_t *bytes = data;

  while (size-- > 0)
    {
      sum = (sum ^ *bytes++) * UINT32_C(16777619);
    }

  return sum;
}

/* Share of the energy that sits at hz, by correlation with both phases. */

static double tone_share(const int16_t *pcm, size_t samples, size_t stride,
                         double rate, double hz)
{
  double re = 0;
  double im = 0;
  double total = 0;
  size_t index;

  for (index = 0; index < samples; index++)
    {
      double value = pcm[index * stride];
      double phase = 2 * M_PI * hz * (double)index / rate;

      re += value * cos(phase);
      im += value * sin(phase);
      total += value * value;
    }

  return total == 0 ? 0 : 2 * (re * re + im * im) / ((double)samples * total);
}

static void test_sbc_decode(void)
{
  struct ny_sbc_decoder_s *decoder = ny_sbc_decoder_create(false);
  int16_t pcm[2 * NY_SBC_FRAME_SAMPLES_MAX];
  static int16_t all[6 * 2 * NY_SBC_FRAME_SAMPLES_MAX];
  struct ny_sbc_format_s format;
  size_t offset = 0;
  size_t used;
  size_t frames = 0;
  uint32_t sum = UINT32_C(2166136261);

  assert(decoder != NULL);
  assert(sizeof(g_vector_sbc) == 6 * SBC_FRAME_BYTES);
  while (offset < sizeof(g_vector_sbc))
    {
      int samples = ny_sbc_decode(decoder, g_vector_sbc + offset,
                                  sizeof(g_vector_sbc) - offset, &used, pcm,
                                  2 * NY_SBC_FRAME_SAMPLES_MAX, &format);

      assert(samples == 128);
      assert(used == SBC_FRAME_BYTES);
      assert(format.rate == 44100 && format.channels == 2);
      assert(format.blocks == 16 && format.subbands == 8);
      assert(format.samples == 128);
      memcpy(all + frames * 256, pcm, sizeof(pcm));
      sum = fnv1a(pcm, sizeof(pcm), sum);
      offset += used;
      frames++;
    }

  assert(frames == 6);

  /* Skip the first frame: the synthesis filter is still filling. */

  assert(tone_share(all + 256, 5 * 128, 2, 44100, 1000) > 0.95);
  assert(tone_share(all + 257, 5 * 128, 2, 44100, 1000) > 0.95);
  assert(tone_share(all + 256, 5 * 128, 2, 44100, 3000) < 0.01);
  fprintf(stderr, "sbc checksum 0x%08x\n", (unsigned)sum);
  assert(sum == GOLDEN_SBC);
  ny_sbc_decoder_destroy(decoder);
}

static void test_sbc_damage(void)
{
  struct ny_sbc_decoder_s *decoder = ny_sbc_decoder_create(false);
  int16_t pcm[2 * NY_SBC_FRAME_SAMPLES_MAX];
  uint8_t damaged[2 * SBC_FRAME_BYTES];
  uint8_t noise[64];
  size_t used;

  assert(decoder != NULL);

  /* Too small an output is refused before anything is consumed. */

  assert(ny_sbc_decode(decoder, g_vector_sbc, SBC_FRAME_BYTES, &used, pcm, 100,
                       NULL) == -ENOSPC);
  assert(used == 0);

  /* A frame cut short waits for the rest. */

  assert(ny_sbc_decode(decoder, g_vector_sbc, SBC_FRAME_BYTES - 1, &used, pcm,
                       256, NULL) == -EAGAIN);
  assert(used == 0);
  assert(ny_sbc_decode(decoder, g_vector_sbc, 1, &used, pcm, 256, NULL) ==
         -EAGAIN);

  /* A payload bit flipped fails the CRC when it hits the scale factors; the
   * frame is stepped over and the next one decodes.
   */

  memcpy(damaged, g_vector_sbc, sizeof(damaged));
  damaged[5] ^= 0x10;
  assert(ny_sbc_decode(decoder, damaged, sizeof(damaged), &used, pcm, 256,
                       NULL) == -EBADMSG);
  assert(used == SBC_FRAME_BYTES);
  assert(ny_sbc_decode(decoder, damaged + used, sizeof(damaged) - used, &used,
                       pcm, 256, NULL) == 128);
  assert(used == SBC_FRAME_BYTES);

  /* Bytes that hold no frame at all are given up on, never looped over. */

  memset(noise, 0x55, sizeof(noise));
  assert(ny_sbc_decode(decoder, noise, sizeof(noise), &used, pcm, 256, NULL) <
         0);
  assert(used == sizeof(noise));
  assert(ny_sbc_decoder_reset(decoder) == 0);
  ny_sbc_decoder_destroy(decoder);
}

static void test_msbc_decode(void)
{
  struct ny_sbc_decoder_s *decoder = ny_sbc_decoder_create(true);
  int16_t pcm[NY_SBC_FRAME_SAMPLES_MAX];
  static int16_t all[4 * NY_MSBC_FRAME_SAMPLES];
  uint32_t sum = UINT32_C(2166136261);
  size_t used;
  int frame;

  assert(decoder != NULL);
  assert(sizeof(g_vector_msbc) == 4 * NY_MSBC_FRAME_BYTES);
  for (frame = 0; frame < 4; frame++)
    {
      int samples = ny_sbc_decode(
          decoder, g_vector_msbc + frame * NY_MSBC_FRAME_BYTES,
          NY_MSBC_FRAME_BYTES, &used, pcm, NY_SBC_FRAME_SAMPLES_MAX, NULL);

      assert(samples == NY_MSBC_FRAME_SAMPLES);
      assert(used == NY_MSBC_FRAME_BYTES);
      memcpy(all + frame * NY_MSBC_FRAME_SAMPLES, pcm,
             NY_MSBC_FRAME_SAMPLES * sizeof(int16_t));
      sum = fnv1a(pcm, NY_MSBC_FRAME_SAMPLES * sizeof(int16_t), sum);
    }

  assert(tone_share(all + NY_MSBC_FRAME_SAMPLES, 3 * NY_MSBC_FRAME_SAMPLES, 1,
                    16000, 1000) > 0.95);
  fprintf(stderr, "msbc checksum 0x%08x\n", (unsigned)sum);
  assert(sum == GOLDEN_MSBC);
  ny_sbc_decoder_destroy(decoder);
}

static void test_msbc_round_trip(void)
{
  struct ny_msbc_encoder_s *encoder = ny_msbc_encoder_create();
  struct ny_sbc_decoder_s *decoder = ny_sbc_decoder_create(true);
  static int16_t out[8 * NY_MSBC_FRAME_SAMPLES];
  int16_t in[NY_MSBC_FRAME_SAMPLES];
  int16_t pcm[NY_SBC_FRAME_SAMPLES_MAX];
  uint8_t frame[NY_MSBC_FRAME_BYTES];
  size_t used;
  int count;
  int index;

  assert(encoder != NULL && decoder != NULL);
  for (count = 0; count < 8; count++)
    {
      for (index = 0; index < NY_MSBC_FRAME_SAMPLES; index++)
        {
          int position = count * NY_MSBC_FRAME_SAMPLES + index;

          in[index] = (int16_t)(12000 * sin(2 * M_PI * 440 * position /
                                            (double)NY_MSBC_RATE));
        }

      assert(ny_msbc_encode(encoder, in, frame) == NY_MSBC_FRAME_BYTES);
      assert(frame[0] == 0xad);
      assert(ny_sbc_decode(decoder, frame, sizeof(frame), &used, pcm,
                           NY_SBC_FRAME_SAMPLES_MAX,
                           NULL) == NY_MSBC_FRAME_SAMPLES);
      memcpy(out + count * NY_MSBC_FRAME_SAMPLES, pcm,
             NY_MSBC_FRAME_SAMPLES * sizeof(int16_t));
    }

  assert(tone_share(out + 2 * NY_MSBC_FRAME_SAMPLES, 6 * NY_MSBC_FRAME_SAMPLES,
                    1, NY_MSBC_RATE, 440) > 0.9);
  assert(ny_pcm_rms(out + 2 * NY_MSBC_FRAME_SAMPLES,
                    6 * NY_MSBC_FRAME_SAMPLES) > 7000);
  ny_msbc_encoder_destroy(encoder);
  ny_sbc_decoder_destroy(decoder);
}

static void test_h2_framing(void)
{
  static const size_t chunks[] = { 60, 24, 48, 7, 1, 120 };
  uint8_t stream[3 + 9 * NY_MSBC_H2_BYTES];
  uint8_t frame[NY_MSBC_FRAME_BYTES];
  uint8_t payload[NY_MSBC_FRAME_BYTES];
  struct ny_msbc_deframer_s deframer;
  size_t chunk;
  int sequence;

  /* Three stray bytes, then frames 0..8 with frame 5 lost on the air. */

  stream[0] = 0x01;
  stream[1] = 0x77;
  stream[2] = 0x33;
  for (sequence = 0; sequence < 9; sequence++)
    {
      memset(payload, sequence + 1, sizeof(payload));
      payload[0] = 0xad;
      ny_msbc_h2_pack(sequence, payload,
                      stream + 3 + sequence * NY_MSBC_H2_BYTES);
    }

  assert(stream[3] == 0x01 && stream[4] == 0x08 && stream[5] == 0xad);
  assert(stream[3 + NY_MSBC_H2_BYTES + 1] == 0x38);
  assert(stream[3 + 2 * NY_MSBC_H2_BYTES + 1] == 0xc8);
  assert(stream[3 + 3 * NY_MSBC_H2_BYTES + 1] == 0xf8);
  assert(stream[3 + 4 * NY_MSBC_H2_BYTES + 1] == 0x08);
  memmove(stream + 3 + 5 * NY_MSBC_H2_BYTES, stream + 3 + 6 * NY_MSBC_H2_BYTES,
          3 * NY_MSBC_H2_BYTES);

  for (chunk = 0; chunk < sizeof(chunks) / sizeof(chunks[0]); chunk++)
    {
      size_t total = 3 + 8 * NY_MSBC_H2_BYTES;
      size_t offset = 0;
      unsigned int lost_total = 0;
      int expected = 0;

      ny_msbc_deframer_reset(&deframer);
      while (offset < total)
        {
          size_t size = chunks[chunk];
          size_t done = 0;

          if (size > total - offset)
            {
              size = total - offset;
            }

          while (done < size)
            {
              unsigned int lost;
              size_t used;

              if (ny_msbc_deframer_push(&deframer, stream + offset + done,
                                        size - done, &used, frame, &lost))
                {
                  expected += (int)lost;
                  assert(frame[0] == 0xad);
                  assert(frame[1] == expected + 1);
                  assert(frame[NY_MSBC_FRAME_BYTES - 1] == expected + 1);
                  lost_total += lost;
                  expected++;
                }

              assert(used > 0);
              done += used;
            }

          offset += size;
        }

      assert(expected == 9);
      assert(lost_total == 1 && deframer.lost == 1);
      assert(deframer.frames == 8);
      assert(deframer.skipped >= 2 && deframer.skipped <= 3);
    }
}

static void test_ring(void)
{
  struct ny_pcm_ring_s ring;
  uint8_t in[10];
  uint8_t out[16];
  int round;
  int index;

  assert(ny_pcm_ring_init(&ring, 0) == -EINVAL);
  assert(ny_pcm_ring_init(&ring, 16) == 0);
  for (index = 0; index < 10; index++)
    {
      in[index] = (uint8_t)index;
    }

  for (round = 0; round < 40; round++)
    {
      assert(ny_pcm_ring_write(&ring, in, 10));
      assert(!ny_pcm_ring_write(&ring, in, 7));
      assert(ny_pcm_ring_space(&ring) == 6);
      assert(ny_pcm_ring_read(&ring, out, 3) == 3);
      assert(out[0] == 0 && out[2] == 2);
      assert(ny_pcm_ring_read(&ring, out, 16) == 7);
      assert(out[0] == 3 && out[6] == 9);
      assert(ny_pcm_ring_read(&ring, out, 16) == 0);
    }

  for (round = 0; round < 40; round++)
    {
      assert(ny_pcm_ring_put(&ring, in, 5));
      assert(ny_pcm_ring_put(&ring, in + 5, 3));
      assert(!ny_pcm_ring_put(&ring, in, 10));
      assert(ny_pcm_ring_get(&ring, out, sizeof(out)) == 5);
      assert(out[4] == 4);
      assert(ny_pcm_ring_get(&ring, out, 2) == -EMSGSIZE);
      assert(ny_pcm_ring_get(&ring, out, sizeof(out)) == 0);
    }

  ny_pcm_ring_clear(&ring);
  assert(ny_pcm_ring_space(&ring) == 16);
  ny_pcm_ring_free(&ring);
  assert(!ny_pcm_ring_write(&ring, in, 1));
}

static void test_wav_and_channels(void)
{
  uint8_t header[NY_PCM_WAV_HEADER_BYTES];
  int16_t mono[4] = { 100, -200, 32767, -32768 };
  int16_t stereo[8];
  int16_t back[4];

  ny_pcm_wav_header(header, 44100, 2);
  assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8));
  assert(header[16] == 16 && header[20] == 1 && header[22] == 2);
  assert(header[24] == 0x44 && header[25] == 0xac && header[26] == 0);
  assert(header[28] == 0x10 && header[29] == 0xb1 && header[30] == 0x02);
  assert(header[32] == 4 && header[34] == 16);
  assert(!memcmp(header + 36, "data", 4));
  ny_pcm_wav_header(header, 16000, 1);
  assert(header[22] == 1 && header[32] == 2);
  assert(header[28] == 0x00 && header[29] == 0x7d);

  ny_pcm_mono_to_stereo(mono, stereo, 4);
  assert(stereo[0] == 100 && stereo[1] == 100 && stereo[7] == -32768);
  ny_pcm_stereo_to_mono(stereo, back, 4);
  assert(!memcmp(mono, back, sizeof(mono)));
  memcpy(stereo, mono, sizeof(mono));
  ny_pcm_mono_to_stereo(stereo, stereo, 4);
  assert(stereo[2] == -200 && stereo[3] == -200 && stereo[6] == -32768);
  assert(ny_pcm_rms(mono, 0) == 0);
  stereo[0] = 1000;
  stereo[1] = -1000;
  assert(ny_pcm_rms(stereo, 2) == 1000);
}

static void test_gate(void)
{
  struct ny_pcm_gate_s gate;
  int16_t loud[120];
  int16_t quiet[120];
  int16_t mic[120];
  int frame;
  int index;

  for (index = 0; index < 120; index++)
    {
      loud[index] = index % 2 ? 8000 : -8000;
      quiet[index] = index % 2 ? 50 : -50;
    }

  /* 24 dB down for 200 ms after the far end falls silent, 7.5 ms frames. */

  ny_pcm_gate_init(&gate, 600, 24, 200, 75);
  assert(gate.hang_frames == 27);
  assert(gate.closed_gain == NY_PCM_GAIN_UNITY / 16);

  for (index = 0; index < 120; index++)
    {
      mic[index] = 16000;
    }

  ny_pcm_gate_far(&gate, quiet, 120);
  assert(!ny_pcm_gate_near(&gate, mic, 120));
  assert(mic[0] == 16000 && mic[119] == 16000);

  /* Closing ramps down across one frame instead of stepping. */

  ny_pcm_gate_far(&gate, loud, 120);
  assert(gate.far_rms == 8000 && gate.closures == 1);
  assert(ny_pcm_gate_near(&gate, mic, 120));
  assert(mic[0] < 16000 && mic[0] > 15000);
  assert(mic[119] == 1000);
  for (index = 1; index < 120; index++)
    {
      assert(mic[index] <= mic[index - 1]);
    }

  /* Held closed for the hang time, then ramped open again. */

  for (frame = 0; frame < 27; frame++)
    {
      for (index = 0; index < 120; index++)
        {
          mic[index] = 16000;
        }

      ny_pcm_gate_far(&gate, frame % 2 ? quiet : NULL, 120);
      assert(ny_pcm_gate_near(&gate, mic, 120));
      if (frame < 26)
        {
          assert(mic[60] == 1000);
        }
    }

  assert(mic[119] == 16000 && mic[0] < 2000);
  ny_pcm_gate_far(&gate, quiet, 120);
  assert(!ny_pcm_gate_near(&gate, mic, 120));
  assert(gate.closures == 1);
}

int main(void)
{
  test_sbc_decode();
  test_sbc_damage();
  test_msbc_decode();
  test_msbc_round_trip();
  test_h2_framing();
  test_ring();
  test_wav_and_channels();
  test_gate();
  puts("PASS: SBC and mSBC decode vectors, damaged input, mSBC round trip, "
       "H2 framing with loss, ring, WAV header, channel helpers, gate");
  return 0;
}
