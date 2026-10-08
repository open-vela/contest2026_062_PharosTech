/****************************************************************************
 * app/camenc/camenc_3a.h
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
 * The exposure, gain and white balance loop.
 *
 * This is the decision half of the split described in
 * <arch/chip/cam3a.h>: the driver measures a frame and applies whatever
 * it is told, and everything about *how much* lives here.
 *
 * There is nothing here that touches a device.  Measurement goes in as a
 * struct, settings come out as a struct, and the caller does the ioctls.  It
 * is arranged that way so that the loop can be run against a sequence of
 * recorded measurements on a host, which is the only way to see what a
 * control loop does before trusting it to a camera -- the interesting
 * behaviour is what it does over fifty frames, not what it does to one.
 *
 * WHY ONE LOOP AND NOT THREE
 *
 * The three controls are one problem.  Brightness is exposure times gain, so
 * a loop that moves them independently hunts; and white balance must not
 * chase a picture whose brightness is still changing, because a channel mean
 * that is moving because the exposure is moving says nothing about the
 * colour of the light.  Both of those are reasons to decide in one place,
 * and neither can be expressed from inside a driver that only ever sees one
 * frame at a time and one control at a time.
 *
 * WHAT THE TWO LOOPS DO
 *
 * Exposure and gain are treated as one quantity -- "how much light" -- and
 * that quantity is moved towards the target in one step, with a damping
 * factor.  It is then split between them with exposure first: exposure
 * costs nothing but motion blur and a longer frame, and gain costs noise,
 * so the exposure is opened up to its ceiling before any gain is added and
 * is closed down first when the light goes.  This is the trade a video
 * camera always makes, and it is the reason a fixed exposure with an
 * automatic gain -- the thing this arrangement exists to allow -- is a
 * setting rather than a reimplementation: hold exposure_max at its floor
 * and none of it is used.
 *
 * White balance steers each channel towards the same mean while leaving the
 * luma alone, which is the same thing the driver's own loop does; what is
 * different here is that it stops while the exposure is unsettled.
 *
 * WHY THIS LOOP CAN BE TURNED OFF, AND WHY THAT IS NOT A CONTRADICTION
 *
 * A loop is a policy: how bright the picture should be, and which of noise,
 * motion blur and frame rate to spend getting there.  The policy here is a
 * reasonable default and it is not always the right one -- an application
 * that needs a fixed exposure for a measurement, or a fixed white balance so
 * that two runs can be compared, is not asking for a better default but for
 * no default at all.
 *
 * So each of the two loops can be handed back: the measurement still happens
 * (it is free -- the demosaic already reads every sample) and the decision
 * stops.  The four combinations are all useful and none of them is a
 * fallback:
 *
 *   auto exposure, auto white balance    the default
 *   manual exposure, auto white balance  a fixed shutter with the colour
 *                                        following the light -- the setting a
 *                                        camera calls shutter priority, and
 *                                        the one this pair exists to allow
 *   auto exposure, manual white balance  a colour that will not drift
 *   manual both                          a camera held still
 *
 * A manual value is where the loop resumes from, not a state the loop has to
 * be talked out of.  Turning exposure back to automatic continues from
 * whatever the picture is at now, which is what stops the switch being a
 * visible jump.
 ****************************************************************************/

#ifndef __APP_CAMENC_CAMENC_3A_H
#define __APP_CAMENC_CAMENC_3A_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <arch/chip/cam3a.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* What the loop wants changed, so the caller writes only what moved.  A gain
 * written every frame is an I2C transaction every frame for no reason, and
 * the sensor's own registers are the slowest thing in the path.
 */

enum camenc_3a_change_e
{
  CAMENC_3A_NONE = 0,
  CAMENC_3A_EXPOSURE = 1u << 0,
  CAMENC_3A_GAIN = 1u << 1,
  CAMENC_3A_WB = 1u << 2
};

/* What a status line says the loop is doing, as one number.
 *
 * The first three are the same distinction the report makes in words: a loop
 * that has stopped moving because it arrived and one that has stopped
 * because it ran out of range look identical from the settings alone, and
 * only the second is a complaint about the scene.  The fourth says the loop
 * has been told not to move at all, which is not a verdict on anything.
 */

#define CAMENC_3A_ST_MOVING  0
#define CAMENC_3A_ST_SETTLED 1
#define CAMENC_3A_ST_RANGE   2
#define CAMENC_3A_ST_MANUAL  3

/* Why the white balance is set as three gains and not as a temperature.
 *
 * A camera's "3200 K" preset is an assumption rather than a measurement: it
 * says the light is the colour of a black body at that temperature, and
 * applies the gains that would neutralise it.  That works on a calibrated
 * camera, where the sensor's own spectral response has been measured and
 * folded in.  Nothing here has been, and the size of the unknown is not
 * small -- the gains this sensor and lens arrive at for a neutral room are
 * around 374/214/318, where a black-body model predicts something near
 * 256/256/256 with at most a third either way.  The sensor's blue is far
 * weaker than its green, and no table derived from first principles contains
 * that.
 *
 * An earlier version of this file tried to work around it by anchoring a
 * temperature to whatever the automatic loop had last converged on, so that
 * the scale was relative and self-calibrating.  It was honest about what it
 * was and it was still the wrong control, for a reason worth writing down:
 * the mapping had no inverse.  Given three gains and an anchor there is no
 * single temperature that produced them, so a display could only show the
 * number that was last *typed* rather than the gains actually in force --
 * which is a control that lies whenever the automatic loop is the one
 * deciding.  The measured gains are the thing that is true, and they are
 * what the driver's control plane speaks, so they are what this speaks too.
 *
 * A real product would calibrate: measure the gains that neutralise a lamp
 * of known colour temperature, and fit a curve through it.  That belongs to
 * the product and not to reference code, and the honest note here is that it
 * has not been done rather than a plausible-looking number in its place.
 *
 * The range is the hardware's, from <arch/chip/cam3a.h>.  It is deliberately
 * not the narrower range the automatic loop keeps to -- that bound is a
 * policy about what a plausible correction looks like, and a policy is not a
 * capability.
 */

struct camenc_3a_cfg_s
{
  /* The luma the loop steers towards, on the same 0..255 scale the driver's
   * measurement uses.
   *
   * That scale is linear in light, so this is not a percentage of what the
   * picture looks like: mid grey is 46 and the eye's own half-way point is
   * 55.  A target chosen as "half the range" is more than a stop too bright,
   * which is a mistake this line made once.  See CAMENC_3A_TARGET_DEFAULT in
   * camenc_main.c for what the default is and why it is where it is.
   */

  uint32_t target;

  /* The sensor's limits, in its own units.  Both are needed because the
   * split between exposure and gain has to know where each one runs out.
   */

  uint32_t exposure_min;
  uint32_t exposure_max;
  uint32_t gain_min;
  uint32_t gain_max;

  /* What the gain control counts as unity: the value at which it multiplies
   * by one.  The exposure is a count of lines and the gain is a fixed-point
   * multiplier, so the two have to be brought to a common scale before they
   * can be added, and this is that scale.
   */

  uint32_t gain_one;

  /* How much of the remaining error to take each frame, as a shift: one
   * means half, three means an eighth.  Larger is steadier and slower.  A
   * loop that takes all of it overshoots on anything but a step change,
   * because the frame it is measuring was exposed by the setting before
   * last.
   */

  uint32_t ae_shift;
  uint32_t awb_shift;

  /* How close to the target counts as arrived, in the same 0..255 units.
   * The exposure and gain are held while the measurement is inside this,
   * which is what stops the loop from moving on the noise between two
   * frames of the same scene.
   */

  uint32_t settle;

  /* The least signal a channel can carry and still be worth steering by, as
   * a channel mean on the 0..255 scale.
   *
   * Below some level there is no colour in a frame to measure.  A channel
   * whose mean is three counts out of 255 differs from its neighbours by a
   * count or two of sensor noise, and their ratios are then not the colour
   * of the light but the noise between them -- so a loop that corrects on
   * them moves the white balance a long way on nothing, and moves it back
   * when the light returns.
   *
   * This is not hypothetical.  Covering the lens took this loop's gains from
   * 366/214/317 to 426/213/420 and back, which is a visible colour cast
   * appearing for as long as the hand was there.
   *
   * The right answer to a frame with no colour in it is to leave the gains
   * where they are: they were right for the last frame that had light in it,
   * and they are still right for the next one.  So the channels below this
   * are skipped rather than corrected, and the loop resumes on its own when
   * the light comes back.
   */

  uint32_t awb_min_mean;
};

struct camenc_3a_s
{
  struct camenc_3a_cfg_s cfg;

  /* What the loop believes is in force.  Seeded by the caller rather than
   * assumed, because the sensor has whatever the driver last wrote -- and it
   * is the driver's value, not a default, that the first correction has to
   * be relative to.
   */

  uint32_t exposure;
  uint32_t gain;
  uint32_t wb[3];

  /* The last measurement acted on, so that a frame the loop has already
   * answered cannot steer it twice.  Acting twice on one measurement doubles
   * the correction, and the two calls are not distinguishable from the
   * outside.
   */

  uint32_t sequence;
  bool started;

  /* Whether the brightness has stopped moving.
   *
   * Note what this does *not* mean: that the brightness reached the target.
   * A scene too dark for the longest exposure and the highest gain leaves
   * the loop pinned at both ceilings and permanently short of its target,
   * and there is nothing to be done about that -- but the colour is still
   * there to be corrected, so a flag meaning "at target" switches the white
   * balance off for as long as the scene stays dark, which is when a wrong
   * colour is hardest to live with.  So it means "the exposure and gain
   * stopped changing", which is true both when the target was reached and
   * when the loop ran out of range.
   */

  bool settled;

  /* The previous frame's brightness, and a frame whose brightness jumped too
   * far from it to believe straight away.
   *
   * A single frame that is black while its neighbours are not is a glitch
   * rather than a change of scene -- a sensor settling after stream start, a
   * torn buffer.  Acting on it asks for tens of times more light, and the
   * correction shows as a flash.  A real change of scene survives one frame,
   * so a frame that disagrees with the last is held back until the next one
   * agrees with it.
   */

  uint32_t last_level;
  uint32_t pending_level;
  bool have_last;
  bool have_pending;

  /* Whether each loop is the application's or this module's.  Both start
   * automatic, which is what the caller's -A flag and this module's default
   * agree on without either having to say so twice.
   */

  bool ae_auto;
  bool awb_auto;

  /* The brightness of the last frame measured, whether or not it was acted
   * on.  It is what a status line has to show; the loop's own memory is the
   * last level it *believed*, which is a different thing.
   *
   * This is the whole frame's brightness, with clipped samples counted at
   * the top of the range, which is the figure the exposure steers on.  The
   * white balance steers on a luma over the counted samples alone -- the
   * same ones its channel means are taken over -- so that the two agree
   * about what the picture's colour is.  The two differ by exactly the part
   * of the frame that was blown out; see camenc_3a_measure.
   */

  uint32_t level;
  bool have_level;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Seed the loop.  `exposure`, `gain` and `wb` are what the hardware is
 * holding now, so that the first correction is measured from the truth.
 */

void camenc_3a_init(FAR struct camenc_3a_s *a,
                    FAR const struct camenc_3a_cfg_s *cfg, uint32_t exposure,
                    uint32_t gain, FAR const uint32_t *wb, uint32_t sequence);

/* Fold one measurement in.  Returns the set of camenc_3a_change_e bits whose
 * value in `a` is new and needs writing; CAMENC_3A_NONE when nothing moved,
 * which is the usual answer once a scene has settled.
 *
 * A measurement whose sequence has already been seen is ignored.
 */

uint32_t camenc_3a_update(FAR struct camenc_3a_s *a,
                          FAR const struct cam3a_stats_s *stats);

/* Whether the last frame measured came within the settle band of the target.
 *
 * This is the question "is the brightness where it was asked to be", which is
 * not the same as `settled` -- see the note on that field.  A report that
 * showed only the second would call a scene out of range "settled" and leave
 * a reader to wonder why the picture is still dark.
 *
 * False until a frame has been measured.
 */

bool camenc_3a_at_target(FAR const struct camenc_3a_s *a);

/* Hand one of the two loops to the application, or take it back.
 *
 * Taking the exposure and gain back does not move them: the loop resumes
 * from where the manual setting left the picture, so the switch is not a
 * jump.  Same for the white balance: taking the loop back leaves the gains
 * where the manual setting put them and corrects from there.
 */

void camenc_3a_set_ae(FAR struct camenc_3a_s *a, bool automatic);
void camenc_3a_set_awb(FAR struct camenc_3a_s *a, bool automatic);

/* Set the exposure and gain by hand, in the sensor's own units.  Values
 * outside the sensor's range are clamped, and the clamp is visible in the
 * caller's copy -- the returned bits say what actually changed, so a caller
 * that wants to know what the sensor took reads `a` back rather than
 * assuming its own value was accepted.
 *
 * Only meaningful while the exposure loop is manual; it does not stop the
 * loop, because a caller that wants one setting at a time while the loop is
 * running has asked for the loop.
 */

uint32_t camenc_3a_manual_ae(FAR struct camenc_3a_s *a, uint32_t exposure,
                             uint32_t gain);

/* Set the white balance by hand, as the three gains the demosaicer applies.
 *
 * 256 is unity, as everywhere else in this file.  Values outside what the
 * capture driver accepts are clamped to CAM3A_WB_MIN and CAM3A_WB_MAX, and
 * the clamp is visible in `a` rather than in the caller's copy, for the same
 * reason the exposure setter works that way: the driver takes a number and
 * the caller has to be able to see which one.
 *
 * These are the gains themselves, so unlike the automatic loop they are not
 * normalised to leave the brightness alone.  Raising all three brightens the
 * picture and the exposure loop will spend the next frames undoing it, which
 * is the honest behaviour: a caller that sets all three has said exactly
 * what it wants applied, and quietly rescaling it would be a different
 * answer than the one it gave.
 */

uint32_t camenc_3a_manual_wb(FAR struct camenc_3a_s *a,
                             FAR const uint32_t *gain);

/* What the loop is doing, as one of the CAMENC_3A_ST_ values. */

uint32_t camenc_3a_state(FAR const struct camenc_3a_s *a);

/* Write a status line into `buf`: one line of space-separated `name=value`
 * pairs, with no line ending, and never longer than `len` including the
 * terminator.  Returns what was written, or 0 if it did not fit.
 *
 * Named values rather than JSON because both ends of this are a few lines of
 * C or JavaScript and neither has a parser; space-separated `name=value` is
 * the shape that needs the least of both.  The vocabulary is documented where
 * it is read, in the page in camenc_ws.c.
 *
 * The ranges travel with the values.  A page that hard-coded them would be a
 * second copy of the sensor's limits, and the two would disagree the first
 * time either moved -- with the slider looking like it works and the
 * hardware refusing what it asked for.
 */

size_t camenc_3a_status(FAR const struct camenc_3a_s *a, FAR char *buf,
                        size_t len);

#endif /* __APP_CAMENC_CAMENC_3A_H */
