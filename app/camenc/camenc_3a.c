/****************************************************************************
 * app/camenc/camenc_3a.c
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
 * The exposure, gain and white balance loop.  See camenc_3a.h.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "camenc_3a.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The limits on what the white balance may ask for.
 *
 * The lower bound keeps a channel from being driven towards nothing.  A
 * scene with no light in one channel is not a cast to correct, and a gain
 * that has run to the floor cannot be recovered from -- whatever the channel
 * had has already been multiplied away.
 *
 * The upper bound is the point past which a correction stops being one.  A
 * scene lit by a single saturated colour cannot be made neutral, and a loop
 * that keeps trying turns sensor noise into coloured noise, which looks
 * worse than the cast did.
 */

#define CAMENC_3A_WB_MIN 64u
#define CAMENC_3A_WB_MAX 1024u

/* The weights the driver's own measurement uses, and the same ones the
 * capture driver's white balance loop uses.  Held here as well because the
 * two are separate decisions that happen to share a definition: this one is
 * the level the loop steers to, and if a caller ever wants a different
 * notion of brightness, this is the line to change.
 */

#define CAMENC_3A_LUMA_R 66u
#define CAMENC_3A_LUMA_G 129u
#define CAMENC_3A_LUMA_B 25u
#define CAMENC_3A_LUMA_SUM \
  (CAMENC_3A_LUMA_R + CAMENC_3A_LUMA_G + CAMENC_3A_LUMA_B)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The frame's brightness, on the driver's 0..255 scale, and each channel's
 * mean.
 *
 * The sums are wide -- a 640x480 frame sums to tens of millions -- so the
 * luma is accumulated in 64 bits.  Doing it in 32 would silently wrap on any
 * picture with something in it, and wrap to a small number, which reads as a
 * very dark frame and drives the exposure wide open.
 *
 * TWO BRIGHTNESSES COME OUT, AND THE DIFFERENCE BETWEEN THEM IS THE CLIPPED
 * SAMPLES
 *
 * The driver leaves a 2x2 block out of `sum` and `count` as soon as any of
 * its four samples is at the top of the range -- see cam3a.h -- because past
 * full scale the code the sensor reports stops depending on how much light
 * arrived, so the ratios between the channels there describe the ceiling and
 * not the scene.  That is right for the white balance and wrong for the
 * exposure, and wrong in the one direction that matters.
 *
 * An exposure loop steering on the counted samples alone measures a frame
 * *darker* the more of it is blown out: the blown part is dropped from the
 * measurement rather than counted as the brightest part of it.  The target
 * then reads as "the unclipped part averages to the target", which any
 * amount of clipping satisfies, so there is no overshoot that ends it.  And
 * because the measured brightness falls as the light rises, the loop asks
 * for more light precisely when it should be asking for less -- which is how
 * a camera ends up holding its gain at the ceiling with the highlights gone,
 * the picture measuring dark while it is the brightest frame there is.
 *
 * So the exposure is given the whole frame's brightness, with every clipped
 * sample counted at the top of its range.  That is the least the sample's
 * true value could be -- it is at or past full scale by definition -- so the
 * figure is a lower bound that rises with the clipping, which is what gives
 * the loop something to close on.  The white balance keeps the measurement
 * over the counted samples, where a mean still describes the colour of the
 * light, and that one comes out in `awb_level`.
 *
 * `count` is in samples and not blocks -- the driver counts the four samples
 * of each block it accepted -- so width * height - count is the number of
 * samples that were left out, and the two can be combined directly.
 *
 * A block is left out whole, so a block with one saturated sample counts
 * four times here and not once.  That errs towards closing the exposure
 * down, which is the direction to err in: the alternative is to leave a
 * blown highlight uncounted, and that is the failure this exists to fix.
 *
 * The geometry is carried in the measurement rather than assumed because a
 * change of mode changes the pixel count, and a mean built from one frame's
 * sums over another frame's area belongs to neither.
 */

static uint32_t camenc_3a_measure(FAR const struct cam3a_stats_s *stats,
                                  FAR uint32_t *mean, FAR uint32_t *awb_level)
{
  uint64_t luma;
  uint64_t full;
  uint32_t counted;
  unsigned int i;

  if (stats->count == 0u)
    {
      /* No sample was counted, so every block held a saturated sample: a
       * frame that is black counts its samples, so a count of zero is not
       * darkness but a frame with no unclipped part at all.  There is no
       * colour in it to balance, and steering the exposure to the top of
       * the range is what stops the loop asking for more light on the frame
       * that is most plainly getting too much.
       */

      for (i = 0; i < 3u; i++)
        {
          mean[i] = 0u;
        }

      *awb_level = 0u;
      return 255u;
    }

  for (i = 0; i < 3u; i++)
    {
      mean[i] = stats->sum[i] / stats->count;
    }

  luma = CAMENC_3A_LUMA_R * (uint64_t)stats->sum[0] +
         CAMENC_3A_LUMA_G * (uint64_t)stats->sum[1] +
         CAMENC_3A_LUMA_B * (uint64_t)stats->sum[2];

  counted = (uint32_t)(luma / (CAMENC_3A_LUMA_SUM * (uint64_t)stats->count));
  *awb_level = counted;

  full = (uint64_t)stats->width * stats->height;

  if (full <= stats->count)
    {
      /* Either nothing was clipped, or the frame has no geometry and there
       * is nothing to say about the samples that were left out.  The two
       * brightnesses are then the same number.
       */

      return counted;
    }

  return (uint32_t)(((uint64_t)counted * stats->count +
                     255u * (full - stats->count)) /
                    full);
}

/* Move `current` a fraction of the way to `want`, taking at least one step
 * while the two differ by more than the step.
 *
 * Without the floor a loop whose error is smaller than the shift stalls
 * short of the target and stays there, which reads as a loop that does not
 * work rather than one that is nearly there.  Without the shift it overshoots
 * on every frame, because the frame being measured was exposed by the
 * setting before last.
 */

static uint32_t camenc_3a_step(uint32_t current, uint32_t want, uint32_t shift)
{
  int32_t delta = (int32_t)want - (int32_t)current;
  int32_t quantum = (int32_t)1 << shift;
  int32_t step;

  if (delta == 0)
    {
      return current;
    }

  step = delta / quantum;

  if (step == 0)
    {
      step = delta > 0 ? 1 : -1;
    }

  return (uint32_t)((int32_t)current + step);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void camenc_3a_init(FAR struct camenc_3a_s *a,
                    FAR const struct camenc_3a_cfg_s *cfg, uint32_t exposure,
                    uint32_t gain, FAR const uint32_t *wb, uint32_t sequence)
{
  memset(a, 0, sizeof(*a));

  a->cfg = *cfg;
  a->exposure = exposure;
  a->gain = gain;

  if (wb != NULL)
    {
      a->wb[0] = wb[0];
      a->wb[1] = wb[1];
      a->wb[2] = wb[2];
    }
  else
    {
      a->wb[0] = CAM3A_WB_ONE;
      a->wb[1] = CAM3A_WB_ONE;
      a->wb[2] = CAM3A_WB_ONE;
    }

  /* The sequence is carried in, not zeroed.  The first measurement the
   * caller passes may be one the driver had already taken before the loop
   * existed, and starting from zero would make the loop act on a frame that
   * was exposed by the previous configuration -- which is a correction
   * computed against a setting the loop never chose.
   */

  a->sequence = sequence;
  a->started = false;
  a->settled = false;

  /* Both loops are the application's to begin with.  A caller that wants one
   * held says so afterwards, which keeps the default in one place -- the
   * caller's -A flag -- instead of being spelled out in both.
   */

  a->ae_auto = true;
  a->awb_auto = true;
}

bool camenc_3a_at_target(FAR const struct camenc_3a_s *a)
{
  return a->have_last && a->last_level + a->cfg.settle >= a->cfg.target &&
         a->last_level <= a->cfg.target + a->cfg.settle;
}

/* Whether a frame's brightness is believable, given the last one acted on.
 *
 * A single frame that differs wildly from its neighbours is a glitch rather
 * than a change of scene: the sensor settling after stream start, a torn
 * buffer.  Acting on it asks for tens of times more light and the correction
 * shows as a flash, which is worse than being 16 ms late.  A real change of
 * scene outlives one frame, so a frame that disagrees is held back until the
 * next one agrees with it -- and a scene that genuinely changed is believed
 * on the second frame, one frame later than before.
 *
 * The first frame establishes the baseline and is not acted on either.  There
 * is nothing to compare it with, and a camera's first frame after the stream
 * starts is not necessarily a picture.
 */

static bool camenc_3a_plausible(FAR struct camenc_3a_s *a, uint32_t level)
{
  if (!a->have_last)
    {
      a->last_level = level;
      a->have_last = true;
      return false;
    }

  /* Within a factor of four of the last brightness believed: the same scene
   * at worst slowly changing, which is what a control loop is for.
   */

  if (level <= a->last_level * 4u && a->last_level <= level * 4u)
    {
      a->last_level = level;
      a->have_pending = false;
      return true;
    }

  /* Outside it.  Believe it only if the frame before said the same thing.
   */

  if (a->have_pending && level <= a->pending_level * 2u &&
      a->pending_level <= level * 2u)
    {
      a->last_level = level;
      a->have_pending = false;
      return true;
    }

  a->pending_level = level;
  a->have_pending = true;
  return false;
}

uint32_t camenc_3a_update(FAR struct camenc_3a_s *a,
                          FAR const struct cam3a_stats_s *stats)
{
  FAR const struct camenc_3a_cfg_s *cfg = &a->cfg;
  uint32_t mean[3];
  uint32_t level;
  uint32_t awb_level;
  uint32_t changed = 0;
  unsigned int i;

  /* A brightness needs an area to be one, and the driver publishes the sums
   * and the frame's size together.  No size means the demosaicer has not
   * produced a frame yet, and there is nothing to conclude from a frame that
   * has not been measured.
   *
   * A count of zero is a different case and not this one: it is a frame with
   * no unclipped samples at all, which is the brightest frame there is
   * rather than a frame with nothing in it.  See camenc_3a_measure.
   */

  if (stats->width == 0u || stats->height == 0u)
    {
      return CAMENC_3A_NONE;
    }

  /* A frame the loop has already answered.  Acting on it twice would apply
   * the correction twice, and from here the two calls are identical.
   */

  if (a->started && stats->sequence == a->sequence)
    {
      return CAMENC_3A_NONE;
    }

  a->sequence = stats->sequence;
  a->started = true;

  level = camenc_3a_measure(stats, mean, &awb_level);

  /* The measurement is worth reporting whether or not it is acted on: it is
   * what the picture is, and a status line that went blank every time a
   * frame was disbelieved would be showing the loop's opinion rather than
   * the camera's output.
   */

  a->level = level;
  a->have_level = true;

  if (!camenc_3a_plausible(a, level))
    {
      return CAMENC_3A_NONE;
    }

  /* ------------------------------------------------------------------ */
  /* Exposure and gain                                                   */
  /* ------------------------------------------------------------------ */

  /* How much light the picture is getting, in units of exposure lines at
   * unity gain.  Exposure and gain multiply to produce brightness, so this
   * product is the quantity the loop is really steering; the split below
   * only decides which of the two delivers it.
   */

  if (a->ae_auto)
    {
      uint32_t light =
          (uint32_t)((uint64_t)a->exposure * a->gain / cfg->gain_one);
      uint32_t want;
      uint32_t exp_new;
      uint32_t gain_new;

      /* Inside the band, the brightness is left alone.  The band is what stops
       * the loop from chasing the noise between two frames of the same scene.
       */

      if (level + cfg->settle >= cfg->target &&
          level <= cfg->target + cfg->settle)
        {
          want = light;
        }
      else if (level == 0u)
        {
          /* A black frame asks for an infinite correction.  Capped rather
           * than allowed to saturate the arithmetic; the damping is what
           * spreads the rest over the following frames.
           */

          want = light * 4u;
        }
      else
        {
          want = (uint32_t)((uint64_t)light * cfg->target / level);
        }

      want = camenc_3a_step(light, want, cfg->ae_shift);

      /* Split it, exposure first.
       *
       * Exposure costs motion blur and nothing else; gain costs noise in every
       * frame it is used in.  So the exposure is opened up to its ceiling
       * before any gain is added, and closed down first when the light goes --
       * the trade a video camera always makes, and one a caller turns off by
       * pinning exposure_max to its floor.
       */

      exp_new = want;

      if (exp_new < cfg->exposure_min)
        {
          exp_new = cfg->exposure_min;
        }
      else if (exp_new > cfg->exposure_max)
        {
          exp_new = cfg->exposure_max;
        }

      /* The gain that delivers the rest, rounded to nearest so that the pair
       * does not drift low by half a unit on every frame.
       */

      gain_new = (uint32_t)(((uint64_t)want * cfg->gain_one + exp_new / 2u) /
                            exp_new);

      if (gain_new < cfg->gain_min)
        {
          gain_new = cfg->gain_min;
        }
      else if (gain_new > cfg->gain_max)
        {
          gain_new = cfg->gain_max;
        }

      /* The split can land back on the pair already in force even though the
       * light is not where it should be.
       *
       * The gain has a resolution of one part in gain_one of itself, so on a
       * long exposure the smallest step it can take is coarser than the error
       * that is left; the loop then stalls just outside its own settle band
       * and stays there, which reads as a target it never reaches rather than
       * a loop that has run out of resolution.  One unit in the direction
       * asked for is what closes the last of it, and the band is what keeps
       * that from becoming a hunting loop.
       */

      if (exp_new == a->exposure && gain_new == a->gain && want != light)
        {
          if (want > light && gain_new < cfg->gain_max)
            {
              gain_new++;
            }
          else if (want < light && gain_new > cfg->gain_min)
            {
              gain_new--;
            }
          else if (want > light && exp_new < cfg->exposure_max)
            {
              exp_new++;
            }
          else if (want < light && exp_new > cfg->exposure_min)
            {
              exp_new--;
            }
        }

      if (exp_new != a->exposure)
        {
          a->exposure = exp_new;
          changed |= CAMENC_3A_EXPOSURE;
        }

      if (gain_new != a->gain)
        {
          a->gain = gain_new;
          changed |= CAMENC_3A_GAIN;
        }
    }

  /* The brightness has stopped moving when the exposure and gain stopped
   * changing -- not when the target was reached.  See the note in the
   * header: a scene out of range leaves the loop pinned at its ceilings and
   * permanently short, and the colour still wants correcting.
   *
   * A loop that has been handed to the application counts as stopped, and
   * that is what keeps the useful combination working: a fixed exposure with
   * the white balance still following the light needs the colour half to run,
   * and it is gated on this.
   */

  a->settled =
      !a->ae_auto || (changed & (CAMENC_3A_EXPOSURE | CAMENC_3A_GAIN)) == 0u;

  /* ------------------------------------------------------------------ */
  /* White balance                                                       */
  /* ------------------------------------------------------------------ */

  /* Only once the brightness has stopped moving.
   *
   * A channel mean moves when the exposure moves, so a loop that corrected
   * the colour while the exposure was still settling would read its own
   * brightness as a colour cast and correct for it.  On a scene that starts
   * dark and brightens, that shows as the picture going blue and then coming
   * back.
   */

  if (a->settled && a->awb_auto)
    {
      for (i = 0; i < 3u; i++)
        {
          uint32_t tagain;

          /* A channel this dark has no colour in it to measure -- see
           * awb_min_mean.  The gains are left where the last frame with
           * light in it left them, which is where they belong.
           */

          if (mean[i] < cfg->awb_min_mean)
            {
              continue;
            }

          /* What this channel's gain would have to be for its mean to come
           * out level with the others, at the brightness the frame already
           * has.
           *
           * Steering to the frame's own luma rather than to a fixed number
           * is what leaves the brightness alone: correcting a colour cast
           * must not re-expose the frame, because the level already belongs
           * to the exposure and raising it would push the bright parts into
           * clipping.  The driver's own loop makes the same choice.
           *
           * The luma here is the one over the counted samples and not the
           * whole frame's, because `mean` is a mean over those samples: a
           * luma that counted the clipped ones would steer them up by the
           * amount that was clipped, which is a brightness the exposure has
           * already decided and not one the colour gets to change.
           */

          tagain = (uint32_t)((uint64_t)CAM3A_WB_ONE * awb_level / mean[i]);

          if (tagain < CAMENC_3A_WB_MIN)
            {
              tagain = CAMENC_3A_WB_MIN;
            }
          else if (tagain > CAMENC_3A_WB_MAX)
            {
              tagain = CAMENC_3A_WB_MAX;
            }

          /* Move whenever there is anything left to correct.
           *
           * There is no deadband here, and there does not need to be one:
           * the measurement is taken *before* the gains are applied, so
           * changing a gain cannot change the next measurement.  This is an
           * open calculation rather than a servo, and a deadband would only
           * leave the colour permanently short of where it should be -- by
           * more than the deadband, because the channels stop short by
           * different amounts and the error that shows is the difference
           * between them.
           *
           * The damping is kept all the same, because it is about how the
           * correction looks rather than whether it arrives, and the step
           * floor inside camenc_3a_step() is what makes it arrive exactly.
           */

          if (tagain != a->wb[i])
            {
              a->wb[i] = camenc_3a_step(a->wb[i], tagain, cfg->awb_shift);
              changed |= CAMENC_3A_WB;
            }
        }
    }

  return changed;
}

void camenc_3a_set_ae(FAR struct camenc_3a_s *a, bool automatic)
{
  /* Nothing else has to happen.  The loop resumes from `a`'s own exposure and
   * gain, which is where the manual setting left them, and a caller that was
   * driving them by hand has already put its values there.
   */

  a->ae_auto = automatic;
}

void camenc_3a_set_awb(FAR struct camenc_3a_s *a, bool automatic)
{
  /* The anchor is deliberately not refreshed here.  It is what the loop last
   * converged to, so that stopping and starting the loop does not move the
   * meaning of a manual temperature that is sitting on the slider.
   */

  a->awb_auto = automatic;
}

uint32_t camenc_3a_manual_ae(FAR struct camenc_3a_s *a, uint32_t exposure,
                             uint32_t gain)
{
  FAR const struct camenc_3a_cfg_s *cfg = &a->cfg;
  uint32_t changed = 0;

  if (exposure < cfg->exposure_min)
    {
      exposure = cfg->exposure_min;
    }
  else if (exposure > cfg->exposure_max)
    {
      exposure = cfg->exposure_max;
    }

  if (gain < cfg->gain_min)
    {
      gain = cfg->gain_min;
    }
  else if (gain > cfg->gain_max)
    {
      gain = cfg->gain_max;
    }

  if (exposure != a->exposure)
    {
      a->exposure = exposure;
      changed |= CAMENC_3A_EXPOSURE;
    }

  if (gain != a->gain)
    {
      a->gain = gain;
      changed |= CAMENC_3A_GAIN;
    }

  return changed;
}

uint32_t camenc_3a_manual_wb(FAR struct camenc_3a_s *a,
                             FAR const uint32_t *gain)
{
  uint32_t changed = 0;
  unsigned int i;

  if (gain == NULL)
    {
      return CAMENC_3A_NONE;
    }

  /* The bounds are the driver's, not the automatic loop's.
   *
   * The loop keeps to a much narrower range than the hardware can express,
   * and that is a policy about what a plausible correction looks like.  A
   * caller setting the gains by hand is not asking for that policy, and
   * quietly imposing it would mean a slider whose travel stops at a quarter
   * of what the hardware can do with nothing saying why.
   */

  for (i = 0; i < 3u; i++)
    {
      uint32_t wanted = gain[i];

      if (wanted < CAM3A_WB_MIN)
        {
          wanted = CAM3A_WB_MIN;
        }
      else if (wanted > CAM3A_WB_MAX)
        {
          wanted = CAM3A_WB_MAX;
        }

      if (wanted != a->wb[i])
        {
          a->wb[i] = wanted;
          changed |= CAMENC_3A_WB;
        }
    }

  return changed;
}

uint32_t camenc_3a_state(FAR const struct camenc_3a_s *a)
{
  if (!a->ae_auto)
    {
      return CAMENC_3A_ST_MANUAL;
    }

  if (!a->settled)
    {
      return CAMENC_3A_ST_MOVING;
    }

  return camenc_3a_at_target(a) ? CAMENC_3A_ST_SETTLED : CAMENC_3A_ST_RANGE;
}

size_t camenc_3a_status(FAR const struct camenc_3a_s *a, FAR char *buf,
                        size_t len)
{
  int n;

  n = snprintf(buf, len,
               "3a e=%" PRIu32 " g=%" PRIu32 " kr=%" PRIu32 " kg=%" PRIu32
               " kb=%" PRIu32 " lv=%" PRIu32 " tg=%" PRIu32 " st=%" PRIu32
               " ae=%u aw=%u emax=%" PRIu32 " gmax=%" PRIu32 " emin=%" PRIu32
               " gmin=%" PRIu32 " wbmin=%" PRIu32 " wbmax=%" PRIu32,
               a->exposure, a->gain, a->wb[0], a->wb[1], a->wb[2], a->level,
               a->cfg.target, camenc_3a_state(a), a->ae_auto ? 1u : 0u,
               a->awb_auto ? 1u : 0u, a->cfg.exposure_max, a->cfg.gain_max,
               a->cfg.exposure_min, a->cfg.gain_min, CAM3A_WB_MIN,
               CAM3A_WB_MAX);

  if (n < 0 || (size_t)n >= len)
    {
      buf[0] = '\0';
      return 0;
    }

  return (size_t)n;
}
