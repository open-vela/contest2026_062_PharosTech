/****************************************************************************
 * app/camenc/tools/test_camenc_3a.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * The camenc exposure, gain and white balance loop, run against a scene
 * model on the host.
 *
 * The loop is a controller, and what a controller does is only visible over
 * time.  One step says nothing; the behaviour that matters -- overshoot,
 * hunting, stalling short of the target, never settling, settling on the
 * wrong value -- shows up over twenty or thirty frames.  None of that can be
 * seen by looking at the code, and all of it is expensive to find on a
 * camera, so it is run here instead.
 *
 * The model is deliberately crude: brightness is exposure times gain, the
 * scene has a fixed reflectance per channel, and the sensor clips.  That is
 * enough to answer the questions that matter, and being crude it cannot be
 * mistaken for a claim about image quality.
 *
 * It sits next to the module it tests and nothing runs it automatically: it
 * is a thing to reach for while working on camenc_3a.c, not a gate.  See the
 * note in run_camenc_3a_test.sh for why that is deliberate.
 *
 * Build and run with app/camenc/tools/run_camenc_3a_test.sh, or by hand:
 *
 *   gcc -std=gnu11 -O1 -Wall -DFAR= \
 *       -Iapp/camenc -Idrivers/include \
 *       -o /tmp/t3a app/camenc/tools/test_camenc_3a.c app/camenc/camenc_3a.c
 *
 * The module includes <nuttx/config.h>, so a build tree's include directory
 * has to be on the path as well -- or an empty file in place of it, which is
 * what the script does, because the loop reads nothing out of it.
 *
 * Pass -v to print the trajectory of the first case.
 ****************************************************************************/

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "camenc_3a.h"

#define WIDTH  640
#define HEIGHT 480
#define COUNT  ((uint32_t)WIDTH * HEIGHT)

/* The band the automatic white balance loop keeps to, mirroring the private
 * CAMENC_3A_WB_MIN and CAMENC_3A_WB_MAX in camenc_3a.c.  Kept here as
 * literals rather than exported, because nothing outside that file has any
 * business choosing them -- and a test that repeats a bound is a test that
 * notices when the bound moves.
 *
 * This is the loop's policy and not the hardware's range, which is
 * CAM3A_WB_MIN..CAM3A_WB_MAX from cam3a.h and is what a manual setting is
 * bounded by.  The two being different is the point of the tests below.
 */

#define WB_FLOOR 64u
#define WB_CEIL  1024u

/* The scene: what a mid-grey needs, and the reflectance of each channel. */

static uint32_t g_scene_light = 1000u;
static uint32_t g_refl[3] = { 256u, 256u, 256u };

static struct cam3a_stats_s g_stats;

static void scene_measure(uint32_t exposure, uint32_t gain)
{
  uint64_t light = (uint64_t)exposure * gain / 16u;
  uint64_t base = light * 110u / g_scene_light;
  unsigned int i;

  memset(&g_stats, 0, sizeof(g_stats));

  for (i = 0; i < 3u; i++)
    {
      uint64_t v = base * g_refl[i] / 256u;

      if (v > 255u)
        {
          v = 255u; /* the sensor clips, which is what loses the colour */
        }

      g_stats.sum[i] = (uint32_t)v * COUNT;
    }

  g_stats.count = COUNT;
  g_stats.width = WIDTH;
  g_stats.height = HEIGHT;
  g_stats.wb[0] = 256;
  g_stats.wb[1] = 256;
  g_stats.wb[2] = 256;
}

static uint32_t level_of(void)
{
  uint64_t luma = 66ull * g_stats.sum[0] + 129ull * g_stats.sum[1] +
                  25ull * g_stats.sum[2];

  return (uint32_t)(luma / (220ull * g_stats.count));
}

static uint32_t g_seq = 1u;

static uint32_t ran(FAR struct camenc_3a_s *a, uint32_t exposure,
                    uint32_t gain, int frames, int verbose)
{
  uint32_t ch = 0;
  int i;

  for (i = 0; i < frames; i++)
    {
      scene_measure(exposure, gain);
      g_stats.sequence = g_seq++;

      ch = camenc_3a_update(a, &g_stats);

      if ((ch & CAMENC_3A_EXPOSURE) != 0)
        {
          exposure = a->exposure;
        }

      if ((ch & CAMENC_3A_GAIN) != 0)
        {
          gain = a->gain;
        }

      if (verbose)
        {
          printf("    f%-3d level %3" PRIu32 "  exp %4" PRIu32
                 "  gain %4" PRIu32 "  wb %4" PRIu32 "/%4" PRIu32 "/%4" PRIu32
                 "  %s\n",
                 i, level_of(), exposure, gain, a->wb[0], a->wb[1], a->wb[2],
                 ch == 0 ? "(no change)" : "");
        }
    }

  return ch;
}

static struct camenc_3a_cfg_s cfg(void)
{
  struct camenc_3a_cfg_s c;

  memset(&c, 0, sizeof(c));
  c.target = 110;
  c.exposure_min = 4;
  c.exposure_max = 500; /* VTS - 4 in the 640x480 mode */
  c.gain_min = 16;
  c.gain_max = 1023;
  c.gain_one = 16;
  c.ae_shift = 2; /* a quarter of the error per frame */
  c.awb_shift = 2;
  c.settle = 4;
  c.awb_min_mean = 12;
  return c;
}

static int g_fails;

static void check(FAR const char *what, int ok, FAR const char *detail)
{
  printf("  %-46s %s%s%s\n", what, ok ? "OK" : "FAIL", ok ? "" : " -- ",
         ok ? "" : detail);

  if (!ok)
    {
      g_fails++;
    }
}

int main(int argc, FAR char *argv[])
{
  struct camenc_3a_s a;
  struct camenc_3a_cfg_s c = cfg();
  uint32_t wb[3] = { 256u, 256u, 256u };
  int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
  char buf[128];

  /* A dark start: the loop has to open up, and it should spend the exposure
   * before it spends any gain.
   */

  printf("dark scene, starting under-exposed\n");
  g_scene_light = 1000u;
  g_refl[0] = g_refl[1] = g_refl[2] = 256u;

  camenc_3a_init(&a, &c, 100u, 16u, wb, 0u);
  ran(&a, 100u, 16u, 60, verbose);

  printf("    -> level %" PRIu32 " exp %" PRIu32 " gain %" PRIu32 "\n",
         level_of(), a.exposure, a.gain);
  snprintf(buf, sizeof(buf), "level %" PRIu32 ", wanted ~%" PRIu32, level_of(),
           c.target);
  check("settles near the target",
        level_of() + 6u >= c.target && level_of() <= c.target + 6u, buf);
  check("used the exposure before the gain", a.exposure > 100u,
        "the exposure never moved");

  /* A bright start: the loop has to close down. */

  printf("bright scene, starting over-exposed\n");
  g_scene_light = 1000u;
  camenc_3a_init(&a, &c, 500u, 1023u, wb, 0u);
  ran(&a, 500u, 1023u, 300, 0);

  printf("    -> level %" PRIu32 " exp %" PRIu32 " gain %" PRIu32 "\n",
         level_of(), a.exposure, a.gain);
  snprintf(buf, sizeof(buf), "level %" PRIu32 ", wanted ~%" PRIu32, level_of(),
           c.target);
  check("settles near the target",
        level_of() + 6u >= c.target && level_of() <= c.target + 6u, buf);

  /* Too dark for the exposure alone: the exposure has to reach its ceiling
   * and the gain has to take over from there.
   */

  printf("scene too dark for the exposure ceiling\n");
  g_scene_light = 6000u;
  camenc_3a_init(&a, &c, 500u, 16u, wb, 0u);
  ran(&a, 500u, 16u, 200, 0);

  printf("    -> level %" PRIu32 " exp %" PRIu32 " gain %" PRIu32 "\n",
         level_of(), a.exposure, a.gain);
  check("opened the exposure to its ceiling", a.exposure == c.exposure_max,
        "not at the ceiling");
  check("added gain on top", a.gain > 16u, "the gain never moved");
  snprintf(buf, sizeof(buf), "level %" PRIu32 ", wanted ~%" PRIu32, level_of(),
           c.target);
  check("settles near the target",
        level_of() + 6u >= c.target && level_of() <= c.target + 6u, buf);

  /* Too bright for even the shortest exposure: the target is unreachable and
   * the loop must sit at the bottom rather than run away or hunt.
   */

  printf("scene too bright for the exposure floor\n");
  g_scene_light = 2u;
  camenc_3a_init(&a, &c, 500u, 1023u, wb, 0u);
  ran(&a, 500u, 1023u, 300, 0);

  printf("    -> level %" PRIu32 " exp %" PRIu32 " gain %" PRIu32 "\n",
         level_of(), a.exposure, a.gain);
  check("sits at the exposure floor", a.exposure == c.exposure_min,
        "not at the floor");
  check("sits at the gain floor", a.gain == c.gain_min, "not at the floor");

  /* A colour cast: the gains have to bring the channels level with one
   * another.  The scale of the gains is not the question and cannot be --
   * any common factor is absorbed by the luma the frame already has, which
   * is exactly why the loop steers to the frame's own brightness rather than
   * to a fixed number.
   */

  printf("scene with a red cast (r twice g and b)\n");
  g_scene_light = 1000u;
  g_refl[0] = 512u;
  g_refl[1] = 256u;
  g_refl[2] = 256u;

  camenc_3a_init(&a, &c, 500u, 16u, wb, 0u);
  ran(&a, 500u, 16u, 200, 0);

  printf("    -> wb %" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n", a.wb[0], a.wb[1],
         a.wb[2]);

  {
    uint64_t out[3];
    uint32_t lo;
    uint32_t hi;
    unsigned int k;

    for (k = 0; k < 3u; k++)
      {
        out[k] = (uint64_t)(g_stats.sum[k] / g_stats.count) * a.wb[k] / 256u;
      }

    lo = (uint32_t)out[0];
    hi = (uint32_t)out[0];

    for (k = 1; k < 3u; k++)
      {
        if (out[k] < lo)
          {
            lo = (uint32_t)out[k];
          }

        if (out[k] > hi)
          {
            hi = (uint32_t)out[k];
          }
      }

    printf("    -> gained means %llu/%llu/%llu\n", (unsigned long long)out[0],
           (unsigned long long)out[1], (unsigned long long)out[2]);

    snprintf(buf, sizeof(buf), "%" PRIu32 "..%" PRIu32 ", %.1f%% apart", lo,
             hi, 100.0 * (hi - lo) / (double)hi);
    check("gains bring the channels level",
          hi != 0u && (hi - lo) * 100u <= hi * 2u, buf);
  }

  /* The colour must not be corrected while the brightness is still moving:
   * a channel mean moves when the exposure moves, so a loop that corrected
   * both would read its own brightness as a cast.
   */

  printf("cast correction waits for the exposure\n");
  g_scene_light = 1000u;
  g_refl[0] = g_refl[1] = g_refl[2] = 256u;
  camenc_3a_init(&a, &c, 100u, 16u, wb, 0u);

  {
    uint32_t exposure = 100u;
    int moved = 0;
    int i;

    /* The first frame only establishes a baseline, so the exposure has to be
     * followed for a few frames: what is being checked is that the colour
     * stays put for as long as the brightness is still moving.
     */

    for (i = 0; i < 4; i++)
      {
        uint32_t ch;

        scene_measure(exposure, 16u);
        g_stats.sequence = g_seq++;
        ch = camenc_3a_update(&a, &g_stats);

        if ((ch & CAMENC_3A_EXPOSURE) != 0)
          {
            exposure = a.exposure;
            moved++;
          }

        if (a.wb[0] != 256u || a.wb[1] != 256u || a.wb[2] != 256u)
          {
            break;
          }
      }

    check("white balance held while the exposure moved",
          a.wb[0] == 256u && a.wb[1] == 256u && a.wb[2] == 256u,
          "it moved while the exposure was unsettled");
    check("and the exposure did move", moved > 0, "nothing happened at all");
  }

  /* Acting twice on one measurement would apply the same correction twice,
   * which from the loop's side is indistinguishable from a correct one. */

  printf("a measurement already answered is ignored\n");
  g_scene_light = 1000u;
  camenc_3a_init(&a, &c, 100u, 16u, wb, 0u);

  {
    uint32_t first;
    uint32_t second;

    scene_measure(100u, 16u);
    g_stats.sequence = g_seq++;
    camenc_3a_update(&a, &g_stats); /* the baseline frame */

    scene_measure(100u, 16u);
    g_stats.sequence = g_seq++;
    camenc_3a_update(&a, &g_stats);

    first = a.exposure;
    second = camenc_3a_update(&a, &g_stats);

    check("second call is a no-op",
          second == CAMENC_3A_NONE && a.exposure == first,
          "the same frame steered the loop twice");
  }

  /* A black frame carries no information, and a loop that acted on it would
   * drive the exposure wide open on a scene it cannot see. */

  printf("a frame with nothing in it is not acted on\n");
  camenc_3a_init(&a, &c, 500u, 32u, wb, 0u);

  {
    uint32_t before = a.exposure;

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.width = WIDTH;
    g_stats.height = HEIGHT;
    g_stats.sequence = g_seq++;

    check("a frame with no counted samples is ignored",
          camenc_3a_update(&a, &g_stats) == CAMENC_3A_NONE &&
              a.exposure == before,
          "the loop moved on a frame it could not measure");
  }

  /* A settled scene has to stop generating writes, or the loop is an I2C
   * transaction and a register write per frame for the rest of the run. */

  printf("a settled scene stops generating writes\n");
  g_scene_light = 1000u;
  g_refl[0] = g_refl[1] = g_refl[2] = 256u;
  camenc_3a_init(&a, &c, 500u, 16u, wb, 0u);

  {
    uint32_t ch = ran(&a, 500u, 16u, 200, 0);

    check("no changes once settled", ch == CAMENC_3A_NONE,
          "still writing after 200 frames");
  }

  /* A scene the loop cannot reach the target on is not a scene it gives up
   * on.  This is the case the board was in: too dark for the longest
   * exposure and the highest gain, so the brightness sat below the target
   * for every frame -- and the white balance, which waited for the
   * brightness to arrive rather than to stop moving, never ran at all.
   */

  printf("an unreachable target still corrects the colour\n");
  g_scene_light = 60000u; /* out of range even at both ceilings */
  g_refl[0] = 512u;
  g_refl[1] = 256u;
  g_refl[2] = 256u;

  camenc_3a_init(&a, &c, 500u, 1023u, wb, 0u);
  ran(&a, 500u, 1023u, 200, 0);

  printf("    -> level %" PRIu32 " exp %" PRIu32 " gain %" PRIu32
         "  wb %" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
         level_of(), a.exposure, a.gain, a.wb[0], a.wb[1], a.wb[2]);
  check("pinned at both ceilings",
        a.exposure == c.exposure_max && a.gain == c.gain_max,
        "it was not out of range after all");
  check("corrected the colour anyway", a.wb[0] != 256u,
        "the white balance never ran because the target was unreachable");

  /* A single frame that does not look like its neighbours is a glitch, not a
   * change of scene, and must not yank the exposure.  The board produced one
   * of these: a frame with every sample counted and a mean of 3 out of 255,
   * between frames whose mean was 78.
   */

  printf("a lone black frame does not move the loop\n");
  g_scene_light = 1000u;
  g_refl[0] = g_refl[1] = g_refl[2] = 256u;
  camenc_3a_init(&a, &c, 500u, 16u, wb, 0u);
  ran(&a, 500u, 16u, 40, 0); /* settle first */

  {
    uint32_t exposure = a.exposure;
    uint32_t gain = a.gain;
    unsigned int i;

    /* One frame that is nearly black, with a full count of usable samples --
     * the shape of the glitch the board produced, and one that the
     * no-samples check cannot catch.
     */

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.width = WIDTH;
    g_stats.height = HEIGHT;
    g_stats.count = COUNT;
    g_stats.sum[0] = 3u * COUNT;
    g_stats.sum[1] = 3u * COUNT;
    g_stats.sum[2] = 3u * COUNT;
    g_stats.sequence = g_seq++;

    check("the lone frame is not acted on",
          camenc_3a_update(&a, &g_stats) == CAMENC_3A_NONE &&
              a.exposure == exposure && a.gain == gain,
          "one glitched frame moved the exposure");

    /* And the frames around it are still followed: the scene has not been
     * frozen, only the outlier ignored.
     */

    for (i = 0; i < 3; i++)
      {
        ran(&a, exposure, gain, 1, 0);
      }

    check("and the loop carries on afterwards", a.settled,
          "the loop did not carry on");
  }

  /* A frame dark enough to have no colour in it must not steer the white
   * balance.  The board produced exactly this: covering the lens took the
   * gains from 366/214/317 to 426/213/420 and back, which is a colour cast
   * appearing for as long as the hand was over the lens.
   */

  printf("white balance holds when there is no light to see colour by\n");
  g_scene_light = 1000u;
  g_refl[0] = 512u; /* a cast, so the gains settle away from unity */
  g_refl[1] = 256u;
  g_refl[2] = 256u;

  {
    uint32_t held[3];
    uint32_t exposure;
    uint32_t gain;
    uint32_t ch;
    unsigned int k;

    /* Settle on a scene with room in it and something to correct, so that
     * the value being held is a correction rather than the default.
     */

    camenc_3a_init(&a, &c, 300u, 16u, wb, 0u);
    exposure = 300u;
    gain = 16u;

    for (k = 0; k < 300u; k++)
      {
        scene_measure(exposure, gain);
        g_stats.sequence = g_seq++;
        ch = camenc_3a_update(&a, &g_stats);

        if ((ch & CAMENC_3A_EXPOSURE) != 0)
          {
            exposure = a.exposure;
          }

        if ((ch & CAMENC_3A_GAIN) != 0)
          {
            gain = a.gain;
          }
      }

    held[0] = a.wb[0];
    held[1] = a.wb[1];
    held[2] = a.wb[2];

    check("settled on a correction to hold", held[0] != 256u,
          "the gains never left unity, so there is nothing to hold");

    /* Now cover the lens.  The frame still reports a full count of usable
     * samples -- it is dark, not clipped -- and the channels differ from one
     * another by a count or two, which is noise rather than a cast.
     */

    for (k = 0; k < 30u; k++)
      {
        memset(&g_stats, 0, sizeof(g_stats));
        g_stats.width = WIDTH;
        g_stats.height = HEIGHT;
        g_stats.count = COUNT;
        g_stats.sum[0] = (5u + (k & 1u)) * COUNT;
        g_stats.sum[1] = 4u * COUNT;
        g_stats.sum[2] = 3u * COUNT;
        g_stats.sequence = g_seq++;

        camenc_3a_update(&a, &g_stats);
      }

    printf("    -> level %" PRIu32 "  wb %" PRIu32 "/%" PRIu32 "/%" PRIu32
           " (held %" PRIu32 "/%" PRIu32 "/%" PRIu32 ")\n",
           level_of(), a.wb[0], a.wb[1], a.wb[2], held[0], held[1], held[2]);
    check("the gains did not move in the dark",
          a.wb[0] == held[0] && a.wb[1] == held[1] && a.wb[2] == held[2],
          "the loop corrected a cast that was only noise");

    /* And they are still live: light the scene again and the loop picks the
     * correction back up rather than being stuck.
     */

    for (k = 0; k < 200u; k++)
      {
        uint32_t ch2;

        scene_measure(exposure, gain);
        g_stats.sequence = g_seq++;
        ch2 = camenc_3a_update(&a, &g_stats);

        if ((ch2 & CAMENC_3A_EXPOSURE) != 0)
          {
            exposure = a.exposure;
          }

        if ((ch2 & CAMENC_3A_GAIN) != 0)
          {
            gain = a.gain;
          }
      }

    printf("    -> level %" PRIu32 "  wb %" PRIu32 "/%" PRIu32 "/%" PRIu32
           "\n",
           level_of(), a.wb[0], a.wb[1], a.wb[2]);
    check("and they are live again once there is light", a.wb[0] == held[0],
          "the loop did not come back to the correction it was holding");
  }

  /* ------------------------------------------------------------------ */
  /* Loops the application has taken over                               */
  /* ------------------------------------------------------------------ */

  printf("a hand-set exposure is left alone\n");
  g_scene_light = 1000u;
  g_refl[0] = g_refl[1] = g_refl[2] = 256u;

  {
    struct camenc_3a_s m;
    uint32_t anchor[3] = { 256u, 256u, 256u };
    uint32_t exposure = 120u;
    uint32_t gain = 40u;
    uint32_t moved = 0;
    unsigned int k;

    camenc_3a_init(&m, &c, exposure, gain, anchor, 0u);
    camenc_3a_set_ae(&m, false);

    /* A scene that wants a completely different exposure, run for long
     * enough that the loop would have moved several times over.
     */

    for (k = 0; k < 60u; k++)
      {
        scene_measure(exposure, gain);
        g_stats.sequence = g_seq++;
        moved |= camenc_3a_update(&m, &g_stats) &
                 (CAMENC_3A_EXPOSURE | CAMENC_3A_GAIN);
      }

    check("the exposure never moved", m.exposure == exposure,
          "the loop wrote to a control it was told to leave");
    check("the gain never moved", m.gain == gain,
          "the loop wrote to a control it was told to leave");
    check("and it never asked for a write", moved == 0u,
          "the caller would have written to the sensor every frame");
    check("the report says the loop is being driven by hand",
          camenc_3a_state(&m) == CAMENC_3A_ST_MANUAL,
          "a manual setting was reported as a loop that had settled");
  }

  printf("a fixed exposure still leaves the colour to the loop\n");

  {
    struct camenc_3a_s m;
    uint32_t anchor[3] = { 256u, 256u, 256u };
    uint32_t exposure = 200u;
    uint32_t gain = 16u;
    unsigned int k;

    camenc_3a_init(&m, &c, exposure, gain, anchor, 0u);
    camenc_3a_set_ae(&m, false);
    camenc_3a_set_awb(&m, true);

    g_scene_light = 1000u;
    g_refl[0] = 512u; /* a red cast, which the exposure must not hide */
    g_refl[1] = 256u;
    g_refl[2] = 256u;

    for (k = 0; k < 60u; k++)
      {
        scene_measure(exposure, gain);
        g_stats.sequence = g_seq++;
        camenc_3a_update(&m, &g_stats);
      }

    printf("    -> wb %" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n", m.wb[0], m.wb[1],
           m.wb[2]);
    check("the colour was corrected anyway", m.wb[0] < m.wb[1],
          "the colour half stopped because the brightness half was manual");
    check("with the exposure untouched", m.exposure == exposure,
          "the manual exposure was moved");
  }

  printf("a hand-set white balance is left alone\n");

  {
    struct camenc_3a_s m;
    uint32_t anchor[3] = { 256u, 256u, 256u };
    uint32_t want[3] = { 300u, 256u, 200u };
    uint32_t held[3];
    uint32_t moved = 0;
    unsigned int k;

    camenc_3a_init(&m, &c, 300u, 16u, anchor, 0u);
    camenc_3a_set_awb(&m, false);
    camenc_3a_manual_wb(&m, want);

    check("a hand-set gain is the gain that is applied",
          m.wb[0] == want[0] && m.wb[1] == want[1] && m.wb[2] == want[2],
          "the setter changed what it was asked for");

    held[0] = m.wb[0];
    held[1] = m.wb[1];
    held[2] = m.wb[2];

    g_scene_light = 1000u;
    g_refl[0] = 512u;
    g_refl[1] = 256u;
    g_refl[2] = 256u;

    for (k = 0; k < 80u; k++)
      {
        scene_measure(300u, 16u);
        g_stats.sequence = g_seq++;
        moved |= camenc_3a_update(&m, &g_stats) & CAMENC_3A_WB;
      }

    check("the gains never moved",
          m.wb[0] == held[0] && m.wb[1] == held[1] && m.wb[2] == held[2],
          "the loop overrode a colour that was set by hand");
    check("and it never asked for a write", moved == 0u,
          "the caller would have written to the driver every frame");
  }

  /* ------------------------------------------------------------------ */
  /* The range a manual white balance is held to                            */
  /* ------------------------------------------------------------------ */

  printf("a hand-set white balance is bounded by the driver, not by the "
         "loop\n");

  {
    struct camenc_3a_s m;
    uint32_t anchor[3] = { 256u, 256u, 256u };
    uint32_t asked[3];
    unsigned int k;

    /* The automatic loop keeps to a narrow band, and that is a judgement
     * about what a plausible correction looks like.  A caller setting the
     * gains by hand has not asked for that judgement, and applying it
     * quietly would mean a slider whose travel stops well short of what the
     * hardware can do, with nothing saying why.
     */

    camenc_3a_init(&m, &c, 300u, 16u, anchor, 0u);

    asked[0] = CAM3A_WB_MAX + 1000u;
    asked[1] = CAM3A_WB_ONE;
    asked[2] = 2u * CAM3A_WB_ONE;
    camenc_3a_manual_wb(&m, asked);

    check("above the driver's ceiling is clamped to it",
          m.wb[0] == CAM3A_WB_MAX,
          "a gain the driver would refuse was passed through");
    check("and a value inside the range is left alone",
          m.wb[1] == CAM3A_WB_ONE && m.wb[2] == 2u * CAM3A_WB_ONE,
          "the setter moved a value that needed no clamping");

    /* Zero is the one value that must never be applied.  Multiplying a
     * channel by zero has already destroyed what was there, and no later
     * correction brings it back -- so a caller that asks for it gets the
     * smallest gain instead of a black channel.
     */

    asked[0] = 0u;
    camenc_3a_manual_wb(&m, asked);
    check("zero becomes the driver's floor rather than a black channel",
          m.wb[0] == CAM3A_WB_MIN,
          "a zero gain was applied, which no correction can undo");

    /* And the range reaches past what the loop will use, which is the whole
     * point of bounding this by the hardware instead.
     */

    asked[0] = 8u * CAM3A_WB_ONE;
    camenc_3a_manual_wb(&m, asked);
    check("the range reaches past the automatic loop's own limits",
          m.wb[0] == 8u * CAM3A_WB_ONE,
          "the manual range is the loop's policy rather than the hardware's");

    /* All three at once, which is how the page sets them. */

    for (k = 0; k < 3u; k++)
      {
        asked[k] = CAM3A_WB_MAX + 1u;
      }

    camenc_3a_manual_wb(&m, asked);

    check("all three are clamped together",
          m.wb[0] == CAM3A_WB_MAX && m.wb[1] == CAM3A_WB_MAX &&
              m.wb[2] == CAM3A_WB_MAX,
          "one of the three escaped the clamp");
  }

  /* ------------------------------------------------------------------ */
  /* The status line                                                    */
  /* ------------------------------------------------------------------ */

  printf("the status line the page reads\n");

  {
    struct camenc_3a_s m;
    uint32_t anchor[3] = { 256u, 256u, 256u };
    uint32_t wbw[3] = { 410u, 256u, 300u };
    char buf[192];
    size_t n;

    camenc_3a_init(&m, &c, 300u, 16u, anchor, 0u);
    camenc_3a_set_ae(&m, false);
    camenc_3a_manual_ae(&m, 200u, 40u);
    camenc_3a_set_awb(&m, false);
    camenc_3a_manual_wb(&m, wbw);

    n = camenc_3a_status(&m, buf, sizeof(buf));
    printf("    -> %s\n", buf);

    check("it fits", n > 0u && n < sizeof(buf),
          "the line did not fit the buffer the server sends it in");
    check("it carries the exposure and gain in force",
          strstr(buf, "e=200") != NULL && strstr(buf, "g=40") != NULL,
          "the page would show sliders that do not match the hardware");
    check("it says both loops are being driven by hand",
          strstr(buf, "ae=0") != NULL && strstr(buf, "aw=0") != NULL,
          "the page cannot tell a manual setting from an automatic one");
    check("it says so in one word", strstr(buf, "st=3") != NULL,
          "a manual setting was reported as a loop that had settled");
    check("and it reports the three gains that are in force",
          strstr(buf, "kr=410") != NULL && strstr(buf, "kg=256") != NULL &&
              strstr(buf, "kb=300") != NULL,
          "the page would move the sliders away from what is applied");
    check("and the range they may be set to",
          strstr(buf, "wbmin=") != NULL && strstr(buf, "wbmax=") != NULL,
          "the page has no bounds for the gain sliders");
  }

  /* A buffer too small for the line must produce nothing rather than half a
   * line: a truncated message is a different message, and the page parses
   * what it is given.
   */

  printf("a status line that does not fit is not half-written\n");

  {
    struct camenc_3a_s m;
    uint32_t anchor[3] = { 256u, 256u, 256u };
    char small[24];

    memset(small, 'x', sizeof(small));
    camenc_3a_init(&m, &c, 300u, 16u, anchor, 0u);

    check("nothing is written",
          camenc_3a_status(&m, small, 8u) == 0u && small[0] == '\0',
          "a truncated line was handed to the caller as a whole one");
  }

  printf("\n%s\n", g_fails == 0 ? "all checks passed" : "FAILURES");
  return g_fails == 0 ? 0 : 1;
}
