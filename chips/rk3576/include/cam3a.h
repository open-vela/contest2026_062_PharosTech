/****************************************************************************
 * chips/rk3576/include/cam3a.h
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
 * The 3A control plane for the capture path.
 *
 * Automatic exposure, gain and white balance are decisions rather than
 * mechanisms.  How bright a picture should be, and which of noise, motion
 * blur and frame rate to spend getting there, depends on what the camera is
 * being used for -- so the decision belongs to the application.  This
 * interface is how it is reached: the capture driver measures and applies,
 * the application decides.
 *
 * WHY THE MEASUREMENT IS IN THE DRIVER
 *
 * The channel sums come out of the demosaic loop, which is already reading
 * every RAW sample, so measuring a frame costs nothing extra.  That is the
 * lesser reason.  The greater one is that the measurement cannot be taken
 * anywhere else.
 *
 * The sums have to be taken *before* the white balance gains are applied: a
 * loop that measured after would read its own output, because a gain that is
 * already too high raises that channel's average and so lowers the
 * correction it asks for, and the loop settles on the square root of the
 * gain it was supposed to find.
 *
 * And they cannot be recovered from the output.  Once the frame is NV12 the
 * colour it carried has been corrected away: an application holding a white
 * balanced picture cannot tell a grey scene from a strongly cast one, which
 * is exactly the distinction white balance exists to make.  Handing the
 * application RAW instead would mean either giving up the fused
 * single-pass demosaic or doing the demosaic twice.
 *
 * WHY THIS IS A DEVICE AND NOT CONTROLS ON /dev/videoN
 *
 * Exposure and gain belong to the sensor, which the capture device's control
 * path reaches; white balance belongs to the capture driver, which it does
 * not.  Folding the two together would mean either a gain stored in the
 * sensor driver that the sensor has no use for, or a sensor driver that
 * knows about the SoC's video block.  A separate device keeps each control
 * where the hardware it programs lives.
 *
 * The ordinary V4L2 controls on /dev/videoN still work and are unaffected:
 * exposure and gain remain settable there, because they are the sensor's.
 *
 * WHERE THIS INTERFACE LIVES
 *
 * The control plane is implemented inside the VICAP driver and appears as a
 * second device node beside /dev/videoN, so its interface lives with that
 * driver rather than with the platform drivers: this directory is the chip's
 * public include directory, which the build exposes as <arch/chip/cam3a.h>
 * (include/arch/chip is a symlink to chips/rk3576/include).  That is what
 * lets the VICAP driver and the application share one definition without
 * either side reaching into the other's tree -- the driver does not include
 * a drivers/ header, and the application does not need a private copy of the
 * numbers or of the structures.
 *
 * The alternative -- a second copy on the application side -- would put the
 * ioctl numbers, the two structures and the gain bounds in two places that
 * have to move together, which is the kind of mirror that drifts.
 *
 * In mainline NuttX this file belongs at arch/arm64/include/rk3576/cam3a.h
 * (the custom-chip and mainline-chip builds both expose the chip's include
 * directory under the same <arch/chip/...> name), so the include lines below
 * would not change -- only this path and the file's location.
 ****************************************************************************/

#ifndef __ARCH_ARM64_INCLUDE_RK3576_CAM3A_H
#define __ARCH_ARM64_INCLUDE_RK3576_CAM3A_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <sys/ioctl.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Where the interface is reachable.  Shared with the driver so the two
 * cannot disagree about it.
 */

#define CAM3A_DEVPATH "/dev/cam3a"

/* The ioctl base.
 *
 * 0x4700 is the first value past the block NuttX reserves for its own
 * subsystems, which runs to 0x4600 and then resumes at 0x8b00 for network
 * ioctls.  Deriving one from a reserved base -- the usual out-of-tree
 * choice, as _JOYBASE | 0x0040 -- would collide with the next command
 * added to that subsystem, and this device is not that subsystem.
 */

#define _CAM3AIOCBASE     (0x4700)
#define _CAM3AIOCVALID(c) (_IOC_TYPE(c) == _CAM3AIOCBASE)
#define _CAM3AIOC(nr)     _IOC(_CAM3AIOCBASE, nr)

/* White balance gains are eight-bit fixed point: 256 is unity.  The demosaic
 * applies them as (value * gain) >> 8.
 */

#define CAM3A_WB_ONE 256u

/* The largest gain the capture driver will accept, and the smallest.
 *
 * These are what the hardware can apply rather than a judgement about what
 * it should: the vector demosaicer narrows the gain to sixteen bits after
 * scaling it by four, so anything above CAM3A_WB_MAX would arrive in the
 * picture as a different number -- a gain the caller did not ask for,
 * applied silently.  Zero is refused because multiplying a channel by zero
 * has already destroyed what was there and no later correction can bring it
 * back.
 *
 * A caller's own policy is a narrower thing than this and belongs to the
 * caller.  The automatic loop in camenc keeps to 64..1024, which is about
 * the range over which a correction is still a correction; an application
 * that wants the whole of the hardware's range has it here.
 */

#define CAM3A_WB_MIN 1u
#define CAM3A_WB_MAX 0x1fffu

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* What the demosaicer measured on one frame, and what it is doing with the
 * result.
 *
 * The sums and the count are the measurement; everything else is state that
 * a caller needs in order to interpret it.
 */

struct cam3a_stats_s
{
  /* Bumped once per measured frame.  Comparing it against the value from a
   * previous call is how a caller tells "a new frame has arrived" from "I am
   * looking at the same measurement again" -- which matters because acting
   * twice on one measurement doubles the correction.
   */

  uint32_t sequence;

  /* The frame the sums below came from.  Carried because the geometry can
   * change between streams and a caller must not mix a frame's sums with
   * another frame's pixel count.
   */

  uint32_t width;
  uint32_t height;

  /* Samples that were counted.  The demosaicer leaves out any 2x2 block that
   * contains a saturated sample, because past the top of its range the
   * sensor reports the same code for any amount of light and the sample
   * carries no colour to balance.  So width * height - count is the number
   * of samples that were clipped, and the fraction is what an exposure loop
   * needs in order to know it is about to lose the highlights.
   */

  uint32_t count;

  /* Channel sums over those samples, in the RAW domain and *before* the
   * white balance gains -- see the note at the top of this file.  Divided by
   * count they give the channel means; the ratio between them is the
   * illumination's colour cast.
   */

  uint32_t sum[3]; /* R, G, B */

  /* The gains in force while that frame was demosaiced, so that a caller can
   * see what the driver did with the previous measurement.
   */

  uint32_t wb[3];

  /* CAM3A_FLAG_* below. */

  uint32_t flags;
};

#define CAM3A_FLAG_AWB_ACTIVE (1u << 0)

/* White balance gains to apply from the next frame on.
 *
 * Setting these takes the white balance away from the driver: a caller that
 * is computing gains itself must not have the driver's own loop overwriting
 * them one frame later.  CAM3A_SET_AWB turns the driver's loop back on.
 */

struct cam3a_wb_s
{
  uint32_t gain[3]; /* R, G, B */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Read the statistics for the most recent demosaiced frame.  Before the
 * first frame has been demosaiced, count is zero and the sums are
 * meaningless.
 */

#define CAM3A_GET_STATS _CAM3AIOC(0)

/* Set the white balance gains, and stop the driver from steering them. */

#define CAM3A_SET_WB _CAM3AIOC(1)

/* Whether the driver steers the white balance from its own measurement
 * argument: 1 leaves it to the driver, 0 holds the gains where they are
 * until CAM3A_SET_WB changes them.  The argument is a pointer to an int.
 */

#define CAM3A_SET_AWB _CAM3AIOC(2)

#endif /* __ARCH_ARM64_INCLUDE_RK3576_CAM3A_H */
