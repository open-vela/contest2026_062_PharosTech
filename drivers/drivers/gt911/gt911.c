/****************************************************************************
 * drivers/drivers/gt911/gt911.c
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
 * Goodix GT911 capacitive touch controller driver.
 *
 * This is a lower half of the generic touchscreen framework
 * (drivers/input/touchscreen_upper.c, CONFIG_INPUT_TOUCHSCREEN): the driver
 * owns the hardware and pushes samples with touch_event(), the framework owns
 * /dev/inputN and its read/poll/ioctl methods.  It does NOT register its own
 * file_operations -- see the "sample contract" note below for what that buys.
 *
 * References:
 *   - GT911 datasheet Rev.03 (pinout, electrical, register map, 4.2 address
 *     selection, 7.1/7.2 modes and interrupt behaviour)
 *   - GT9 non-single-layer programming guide (register field semantics)
 *
 * Wiring assumptions: the controller has its own RST and INT lines (both
 * board-supplied, see struct gt911_board_s) and sits on an I2C bus with
 * external pull-ups.  The module is normally configured at the factory (X/Y
 * output range, touch point count, refresh rate, trigger edge) and is then
 * left alone: only a module that arrived without a configuration gets one
 * written, and the table comes from the board.  How the module is mounted
 * relative to the framebuffer is likewise a board property, and it arrives as
 * a parameter of gt911_register() -- see struct gt911_orientation_s.
 *
 * ---------------------------------------------------------------------------
 * Sample contract (why the layout below is what it is)
 * ---------------------------------------------------------------------------
 * The framework pops samples out of a circular buffer and hands each read()
 * caller whatever is queued, and LVGL's driver (lv_nuttx_touchscreen.c) asks
 * for TSIOC_GETMAXPOINTS up front and then requires every read() to return
 * exactly SIZEOF_TOUCH_SAMPLE_S(maxpoint) bytes -- a short read is treated as
 * "no sample" and dropped.  The framework queues
 * SIZEOF_TOUCH_SAMPLE_S(sample->npoints) bytes per event, so npoints must
 * equal maxpoint on every event this driver pushes.  Slots with no contact
 * therefore carry flags == 0 and are ignored by both consumers.
 *
 * LVGL also only ever looks at point[0] for its single-point path, so a
 * release must be reported in point[0]; the samples below are re-ordered
 * around a "primary" contact for exactly that reason.
 *
 * ---------------------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------------------
 * Messages go through the input subsystem's own macros -- ierr()/iwarn()/
 * iinfo() from debug.h -- which is the convention the other platform drivers
 * in this tree follow (rtcerr() in pcf8563, lcderr() in st77916).  They are
 * compiled to _none() unless CONFIG_DEBUG_INPUT_ERROR/WARN/INFO is set, so the
 * driver is silent by default and the level it speaks at is a build-time
 * switch; that is deliberate, because the levels are also what the rest of the
 * input subsystem prints at.
 *
 * What that means for a bring-up: the driver's own account of what it found
 * (product id, firmware version, coordinate range, configuration state,
 * orientation) only appears with CONFIG_DEBUG_INPUT_INFO=y.  A board that
 * wants it enables the three options; see the KICKPI_K7_TOUCH help text for a
 * worked example.  Registering the device and failing to do so are still
 * visible without them, because the board that calls gt911_register() logs its
 * result with syslog().
 *
 * Nothing here is logged per sample: with a 7-10 ms scan period that would
 * flood the console even at INFO.
 *
 * ---------------------------------------------------------------------------
 * Sampling
 * ---------------------------------------------------------------------------
 * The panel asserts INT once per scan period while the coordinate buffer holds
 * data the master has not acknowledged (the buffer status byte, 0x814e, is
 * cleared by writing 0 to it).  The interrupt path gives the latency that a
 * finger needs; the LRU-style poll below is a safety net that keeps the device
 * usable on a board that does not wire INT at all, and repairs a missed edge
 * otherwise.  Both run through one work item, so they cannot overlap.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/compiler.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include <nuttx/input/touchscreen.h>

#include "gt911.h"

#ifdef CONFIG_INPUT_GT911

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller registers (16-bit register address, MSB first) */

#define GT911_REG_PRODUCT_ID 0x8140 /* 4 ASCII bytes, e.g. "911\0" */
#define GT911_REG_FW_VERSION 0x8144 /* 16-bit version, low byte first */
#define GT911_REG_XRES       0x8146 /* X output range, low byte first */
#define GT911_REG_YRES       0x8148 /* Y output range, low byte first */
#define GT911_REG_VENDOR_ID  0x814a /* Module option information */
#define GT911_REG_STATUS     0x814e /* buffer status + contact count */
#define GT911_REG_POINT1     0x814f /* first point group */

/* 0x814e: bit7 = buffer status (1 = coordinates ready), bit3-0 = contacts.
 * The master must write the whole byte back as 0 after reading the frame.
 */

#define GT911_STATUS_READY   0x80
#define GT911_STATUS_NPOINTS 0x0f

/* Point group layout.  The datasheet's register list puts the group's track id
 * at 0x814f, so point 1 does NOT start at 0x8150 -- 0x8150 is its X low byte.
 * Each group is 8 bytes:
 *
 *   +0 track id   +1/+2 x (little endian)   +3/+4 y (little endian)
 *   +5/+6 size    +7 reserved
 *
 * The track id is stable for the lifetime of a contact, which is what lets
 * the tracker below follow a moving finger instead of renumbering it every
 * scan period.
 */

#define GT911_POINT_GROUP_LEN 8

/* Register access timeouts, in I2C transfers.  A failed read is retried once
 * at most: the GT911 does not NAK on its own, so a failure means the bus or
 * the device is gone, and blocking the worker on it only delays recovery.
 */

#define GT911_PRODUCT_ID_LEN 4

/* Module configuration area.  0x8047..0x80fe holds the configuration the
 * controller runs with (X/Y output range, contact limit, sensing channel map,
 * frequency, ...), 0x80ff its checksum and 0x8100 the "apply" flag.
 *
 * This is what makes the difference between a working module and one that
 * answers every read and senses nothing: a module delivered with a blank
 * configuration area has no X/Y output range and no sensing channel map, so it
 * never completes a scan frame, never raises INT and never reports a
 * coordinate -- while a presence probe of the product id still succeeds.  The
 * driver therefore reports the area at start-up and writes a table when the
 * board supplies one and the controller has none (see gt911_config_apply()).
 *
 * The area is walked in FIFO-sized chunks because the rest of this board only
 * ever exercises the RK3576 I2C lower half with short transfers.
 */

#define GT911_REG_CONFIG_VER    0x8047
#define GT911_REG_COMMAND       0x8040 /* Real-time command, write only */
#define GT911_REG_CONFIG_CHKSUM 0x80ff
#define GT911_REG_CONFIG_FRESH  0x8100
#define GT911_CONFIG_CHUNK      30u /* register address + payload <= FIFO */

/* Largest single transfer this driver issues: the register address, the whole
 * configuration payload, its checksum and the apply flag.  Used to size the
 * message buffer in gt911_write_buf(), which is what carries the configuration
 * download.
 */

#define GT911_WRITE_MAX (2u + GT911_CONFIG_LEN + 2u)

/* 0x8040 real-time command values (datasheet 3.1) */

#define GT911_CMD_SOFT_RESET 2 /* Software reset */

/* GT911_CONFIG_LEN comes from gt911.h: the board's table and the driver's
 * length check are then the same constant by construction, so a board whose
 * array is a different size is caught by its own static assertion.
 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One tracked contact */

struct gt911_contact_s
{
  bool used;     /* Slot tracks a contact (live or just released) */
  bool fresh;    /* Contact appeared in the frame being reported */
  bool released; /* Contact is missing from the frame being reported */
  uint8_t id;    /* Track id reported by the controller */
  int16_t x;     /* Last known position, controller coordinates */
  int16_t y;
};

/* What the controller reports about its own configuration area */

struct gt911_cfg_info_s
{
  uint8_t version;  /* 0x8047: configuration version */
  uint16_t xmax;    /* 0x8048/0x8049: X output max, low byte first */
  uint16_t ymax;    /* 0x804a/0x804b: Y output max, low byte first */
  uint8_t contacts; /* 0x804c: output contact limit */
  uint8_t chksum;   /* 0x80ff as read back */
  uint8_t expected; /* 0x80ff computed over the data */
  unsigned int sum; /* Byte sum over 0x8047..0x80fe */
  bool readable;    /* The area could be read at all */
  bool valid;       /* Checksum consistent and not all-zero */
  bool blank;       /* Every byte read back as zero */
};

/* A full-length sample, i.e. struct touch_sample_s with its trailing point[1]
 * widened to the number of points this device reports.  Field order and types
 * are identical to the framework's structure, so the pointer passed to
 * touch_event() reads back exactly the same layout; widening it here is what
 * makes every sample exactly
 * SIZEOF_TOUCH_SAMPLE_S(CONFIG_INPUT_GT911_MAX_POINTS) bytes, which is the
 * length LVGL requires from each read().
 */

struct gt911_sample_s
{
  int32_t npoints;
  int32_t dummy;
  struct touch_point_s point[CONFIG_INPUT_GT911_MAX_POINTS];
};

_Static_assert(sizeof(struct gt911_sample_s) ==
                   SIZEOF_TOUCH_SAMPLE_S(CONFIG_INPUT_GT911_MAX_POINTS),
               "GT911 sample must match the framework's sample size");

/* Driver state */

struct gt911_dev_s
{
  struct touch_lowerhalf_s lower; /* Must be first (framework callback) */

  FAR struct i2c_master_s *i2c; /* I2C bus the panel is attached to */
  uint8_t addr;                 /* 7-bit slave address in use */

  struct work_s work; /* Sampling work item (LPWORK) */
  spinlock_t lock;    /* Guards seen/irqpending below */
  volatile bool seen; /* An INT edge was observed */
  bool irq;           /* INT line is wired and unmasked */

  uint16_t fw_version; /* Reported through TSIOC_GETFWVERSION */
  uint16_t xrange;     /* Engine coordinate range, 0 = unset */
  uint16_t yrange;

  /* Coordinate orientation, as supplied by the caller of gt911_register().
   * The structure is copied rather than referenced, so the caller's does not
   * have to outlive the call, and the two range fields below are resolved once
   * the engine has reported what it is running: after that the sampling path
   * only does arithmetic, and a mirror that cannot be computed has already
   * been dropped.
   */

  struct gt911_orientation_s orient; /* Copied from the caller */
  uint16_t xmirror;                  /* Range the X mirror works against */
  uint16_t ymirror;

  /* Contact tracking.  Owned by the worker: nothing else runs while the
   * worker executes, and it never yields, so no lock is needed here.
   */

  FAR struct gt911_contact_s *contacts;
  int primary;      /* Slot LVGL sees as "the" touch, or -1 */
  uint32_t errors;  /* Consecutive failed transfers */
  bool sched_error; /* Sampling could not be re-armed */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int gt911_read(FAR struct gt911_dev_s *dev, uint16_t reg,
                      FAR uint8_t *buf, size_t len);
static int gt911_write_buf(FAR struct gt911_dev_s *dev, uint16_t reg,
                           FAR const uint8_t *buf, size_t len);
static int gt911_write(FAR struct gt911_dev_s *dev, uint16_t reg,
                       uint8_t value);
static void gt911_config_read(FAR struct gt911_dev_s *dev,
                              FAR struct gt911_cfg_info_s *info);
static void gt911_config_log(FAR const struct gt911_cfg_info_s *info);
static void gt911_geometry(FAR struct gt911_dev_s *dev);
static bool gt911_engine_ready(FAR struct gt911_dev_s *dev);
static int gt911_config_bringup(FAR struct gt911_dev_s *dev,
                                FAR const struct gt911_board_s *board);
static int gt911_soft_reset(FAR struct gt911_dev_s *dev);
static int gt911_config_commit(FAR struct gt911_dev_s *dev,
                               FAR const uint8_t *cfg, size_t len, bool split);
static int gt911_control(FAR struct touch_lowerhalf_s *lower, int cmd,
                         unsigned long arg);
static void gt911_transform(FAR struct gt911_dev_s *dev, FAR int16_t *x,
                            FAR int16_t *y);
static void gt911_orientation_setup(FAR struct gt911_dev_s *dev);
static void gt911_track(FAR struct gt911_dev_s *dev, FAR const uint8_t *frame,
                        unsigned int npoints);
static bool gt911_report(FAR struct gt911_dev_s *dev);
static void gt911_schedule(FAR struct gt911_dev_s *dev, clock_t delay);
static void gt911_worker(FAR void *arg);
static int gt911_isr(int irq, FAR void *context, FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_read
 *
 * Description:
 *   Read consecutive registers starting at reg.  The 16-bit register address
 *   is written first and the read repeated-starts without releasing the bus,
 *   which is what the controller expects; the register pointer auto-increments
 *   across the burst.
 *
 *   Note the success convention: a completed transfer returns a NON-NEGATIVE
 *   value, not OK.  The RK3576 I2C lower half returns the controller's
 *   positive STOP-phase status word when a transfer completes and a negated
 *   errno only on failure, so callers must test `ret < 0` and never
 *   `ret == OK` -- the latter silently turns every successful transfer into a
 *   failure.
 *
 ****************************************************************************/

static int gt911_read(FAR struct gt911_dev_s *dev, uint16_t reg,
                      FAR uint8_t *buf, size_t len)
{
  uint8_t regbuf[2];
  struct i2c_msg_s msgv[2];

  regbuf[0] = (uint8_t)(reg >> 8);
  regbuf[1] = (uint8_t)(reg & 0xff);

  msgv[0].frequency = CONFIG_INPUT_GT911_I2C_FREQUENCY;
  msgv[0].addr = dev->addr;
  msgv[0].flags = 0;
  msgv[0].buffer = regbuf;
  msgv[0].length = sizeof(regbuf);

  msgv[1].frequency = CONFIG_INPUT_GT911_I2C_FREQUENCY;
  msgv[1].addr = dev->addr;
  msgv[1].flags = I2C_M_READ;
  msgv[1].buffer = buf;
  msgv[1].length = len;

  return I2C_TRANSFER(dev->i2c, msgv, 2);
}

/****************************************************************************
 * Name: gt911_transform
 *
 * Description:
 *   Map a controller coordinate into the display's coordinate system, as the
 *   board described it: mirror each axis against the range the coordinates
 *   actually arrive in, then exchange the axes if it asked for that.
 *
 *   Nothing here can be discovered from the hardware -- the controller has no
 *   "which way up am I" register -- so "no orientation supplied" and "an
 *   orientation that happens to be the identity" are the same case, and x and
 *   y come back untouched.
 *
 *   The ranges are the ones gt911_orientation_setup() resolved, i.e. the
 *   controller's own when it reported them, so the mirror stays exact even on
 *   a module whose configuration is not the one the board expected.  A mirror
 *   whose range resolved to zero cannot be computed at all; that case is
 *   cleared (with a warning) by the setup, so no negative coordinate can be
 *   produced here.
 *
 ****************************************************************************/

static void gt911_transform(FAR struct gt911_dev_s *dev, FAR int16_t *x,
                            FAR int16_t *y)
{
  FAR const struct gt911_orientation_s *orient = &dev->orient;
  int32_t tx = *x;
  int32_t ty = *y;

  if (orient->mirrorx)
    {
      tx = (int32_t)dev->xmirror - 1 - tx;
    }

  if (orient->mirrory)
    {
      ty = (int32_t)dev->ymirror - 1 - ty;
    }

  if (orient->swapxy)
    {
      int32_t tmp = tx;
      tx = ty;
      ty = tmp;
    }

  *x = (int16_t)tx;
  *y = (int16_t)ty;
}

/****************************************************************************
 * Name: gt911_orientation_setup
 *
 * Description:
 *   Resolve the board's orientation against what the controller reports about
 *   itself, and say in the log what will be applied.
 *
 *   The mirror ranges are chosen here rather than taken from the board alone
 *   because the controller's own registers (0x8146/0x8148) state the frame its
 *   coordinates arrive in, and that is the frame a mirror has to be computed
 *   against.  The board's values are the fallback for a controller that came
 *   up without a range, which happens when the module arrived unconfigured --
 *   in which case there is nothing to mirror anyway, but the driver still has
 *   to leave the coordinates alone rather than compute with a zero range.
 *
 *   Called once, after the configuration bring-up, so it sees the engine's
 *   final state rather than an intermediate one.
 *
 ****************************************************************************/

static void gt911_orientation_setup(FAR struct gt911_dev_s *dev)
{
  FAR struct gt911_orientation_s *orient = &dev->orient;

  dev->xmirror = dev->xrange != 0 ? dev->xrange : orient->xres;
  dev->ymirror = dev->yrange != 0 ? dev->yrange : orient->yres;

  /* The controller's own range wins when it has one.  A disagreement means the
   * board's value describes a different module or a different configuration
   * table than the one the controller is running, which is worth saying out
   * loud: it is the difference between "the table you expected won" and "the
   * engine is running something else".
   */

  if (dev->xrange != 0 && orient->xres != 0 && orient->xres != dev->xrange)
    {
      iwarn("WARNING: GT911 orientation mirrors X against %u but the "
            "controller reports %u, so the controller's range is used\n",
            orient->xres, dev->xrange);
    }

  if (dev->yrange != 0 && orient->yres != 0 && orient->yres != dev->yrange)
    {
      iwarn("WARNING: GT911 orientation mirrors Y against %u but the "
            "controller reports %u, so the controller's range is used\n",
            orient->yres, dev->yrange);
    }

  /* A mirror with no range to mirror against would turn every coordinate
   * negative, so it is dropped instead of applied.  That leaves the frame as
   * the controller reports it, which is wrong but visible; applying it would
   * simply be wrong -- and there is nothing to mirror anyway when the engine
   * came up without a range, because it is not scanning either.
   */

  if (orient->mirrorx && dev->xmirror == 0)
    {
      iwarn("WARNING: GT911 cannot mirror X: no coordinate range is "
            "known, so the coordinates are left as reported\n");
      orient->mirrorx = false;
    }

  if (orient->mirrory && dev->ymirror == 0)
    {
      iwarn("WARNING: GT911 cannot mirror Y: no coordinate range is "
            "known, so the coordinates are left as reported\n");
      orient->mirrory = false;
    }

  if (orient->swapxy || orient->mirrorx || orient->mirrory)
    {
      iinfo("GT911 orientation: mirror X %s, mirror Y %s, swap XY %s, "
            "mirrors computed against %u x %u\n",
            orient->mirrorx ? "on" : "off", orient->mirrory ? "on" : "off",
            orient->swapxy ? "on" : "off", dev->xmirror, dev->ymirror);
    }
  else
    {
      iinfo("GT911 orientation: coordinates are used as the controller "
            "reports them\n");
    }
}

/****************************************************************************
 * Name: gt911_track
 *
 * Description:
 *   Fold one hardware frame into the tracked contact set.
 *
 *   Contacts are matched by the controller's track id rather than by position
 *   or sample index, so a finger keeps its slot while it moves.  A tracked
 *   contact that the frame does not mention is marked released here and is
 *   dropped by the report step after its TOUCH_UP has been published; that
 *   two-phase approach is why a release always reaches the application, even
 *   though the panel has already stopped mentioning the contact.
 *
 *   With no contact to match, a free slot is used; if every slot is held by a
 *   released contact (a simultaneous five-finger swap, the only way to run out
 *   of slots when MAX_POINTS covers the controller's own maximum) the oldest
 *   released slot is recycled and its TOUCH_UP is dropped.
 *
 ****************************************************************************/

static void gt911_track(FAR struct gt911_dev_s *dev, FAR const uint8_t *frame,
                        unsigned int npoints)
{
  unsigned int n;
  int i;

  /* Assume every tracked contact ended in this frame; the loop below clears
   * the flag for the contacts that are still present.
   */

  for (i = 0; i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
    {
      dev->contacts[i].fresh = false;
      dev->contacts[i].released = dev->contacts[i].used;
    }

  for (n = 0; n < npoints; n++)
    {
      FAR const uint8_t *group = &frame[n * GT911_POINT_GROUP_LEN];
      uint8_t id = group[0];
      int16_t x;
      int16_t y;
      int slot = -1;

      x = (int16_t)((uint16_t)group[1] | ((uint16_t)group[2] << 8));
      y = (int16_t)((uint16_t)group[3] | ((uint16_t)group[4] << 8));

      gt911_transform(dev, &x, &y);

      /* Prefer the slot this contact already occupies */

      for (i = 0; i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
        {
          if (dev->contacts[i].used && !dev->contacts[i].released &&
              dev->contacts[i].id == id)
            {
              slot = i;
              break;
            }
        }

      /* Otherwise take a free one, then one whose contact has just gone */

      for (i = 0; slot < 0 && i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
        {
          if (!dev->contacts[i].used)
            {
              slot = i;
            }
        }

      for (i = 0; slot < 0 && i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
        {
          if (dev->contacts[i].released)
            {
              slot = i;
            }
        }

      if (slot < 0)
        {
          /* Cannot happen while MAX_POINTS >= the controller's maximum */

          continue;
        }

      if (!dev->contacts[slot].used)
        {
          dev->contacts[slot].used = true;
          dev->contacts[slot].fresh = true;
        }

      dev->contacts[slot].id = id;
      dev->contacts[slot].released = false;
      dev->contacts[slot].x = x;
      dev->contacts[slot].y = y;
    }
}

/****************************************************************************
 * Name: gt911_primary
 *
 * Description:
 *   Pick the slot LVGL should treat as the active touch.
 *
 *   A live contact always wins, and the previous primary is kept while it
 *   lives so that a second finger cannot steal the pointer mid-drag.  Only
 *   once nothing is down does a released slot get returned, so that its
 *   TOUCH_UP lands in point[0]: LVGL reads only point[0] on its single-point
 *   path, and a release it never sees leaves the widget stuck in the pressed
 *   state.  A release that happens while another contact is still down is
 *   reported only as a point with TOUCH_UP in a later slot -- which is also
 *   how LVGL's own gesture path treats it, the pointer moving to the finger
 *   that is still down rather than lifting.
 *
 ****************************************************************************/

static int gt911_primary(FAR struct gt911_dev_s *dev)
{
  int i;

  /* The current primary, if it is still down */

  if (dev->primary >= 0)
    {
      FAR struct gt911_contact_s *c = &dev->contacts[dev->primary];

      if (c->used && !c->released)
        {
          return dev->primary;
        }
    }

  /* Any live contact */

  for (i = 0; i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
    {
      if (dev->contacts[i].used && !dev->contacts[i].released)
        {
          return i;
        }
    }

  /* Nothing is down: the previous primary is the one whose release matters,
   * so report it rather than an arbitrary other contact.
   */

  if (dev->primary >= 0 && dev->contacts[dev->primary].used)
    {
      return dev->primary;
    }

  for (i = 0; i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
    {
      if (dev->contacts[i].used)
        {
          return i;
        }
    }

  return -1;
}

/****************************************************************************
 * Name: gt911_report
 *
 * Description:
 *   Publish the tracked contacts as one touch sample, primary first, and drop
 *   the contacts that have been released.  Returns true if the sample carried
 *   any contact, i.e. whether a sample was pushed at all: an idle frame is not
 *   reported, so a reader that is waiting in read() is not woken up by a
 *   controller that has nothing to say.
 *
 ****************************************************************************/

static bool gt911_report(FAR struct gt911_dev_s *dev)
{
  struct gt911_sample_s sample;
  int order[CONFIG_INPUT_GT911_MAX_POINTS];
  unsigned int n = 0;
  uint64_t timestamp;
  bool activity = false;
  int primary;
  int i;

  memset(&sample, 0, sizeof(sample));

  primary = gt911_primary(dev);

  if (primary < 0)
    {
      /* Nothing to report and nothing to release */

      return false;
    }

  /* Order the remaining slots after the primary, live contacts first */

  order[n++] = primary;

  for (i = 0; i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
    {
      if (i != primary && dev->contacts[i].used && !dev->contacts[i].released)
        {
          order[n++] = i;
        }
    }

  for (i = 0; i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
    {
      if (i != primary && dev->contacts[i].used && dev->contacts[i].released)
        {
          order[n++] = i;
        }
    }

  timestamp = touch_get_time();

  /* The sample is always full length: slots the tracker did not fill keep
   * flags == 0, which both the framework and LVGL treat as "no contact".
   */

  sample.npoints = CONFIG_INPUT_GT911_MAX_POINTS;

  for (i = 0; i < (int)n && i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
    {
      FAR struct gt911_contact_s *c = &dev->contacts[order[i]];
      FAR struct touch_point_s *point = &sample.point[i];

      point->id = c->id;
      point->x = c->x;
      point->y = c->y;
      point->flags = TOUCH_ID_VALID | TOUCH_POS_VALID;

      if (c->released)
        {
          point->flags |= TOUCH_UP;
        }
      else if (c->fresh)
        {
          point->flags |= TOUCH_DOWN;
        }
      else
        {
          point->flags |= TOUCH_MOVE;
        }

      point->timestamp = timestamp;
      activity = true;
    }

  if (!activity)
    {
      return false;
    }

  touch_event(dev->lower.priv, (FAR const struct touch_sample_s *)&sample);

  /* The release has now been published, so the slot can be recycled.  The
   * primary is cleared too, or a dead slot would be reported as the primary
   * for the next contact's first frame.
   */

  for (i = 0; i < CONFIG_INPUT_GT911_MAX_POINTS; i++)
    {
      if (dev->contacts[i].used && dev->contacts[i].released)
        {
          dev->contacts[i].used = false;
          dev->contacts[i].released = false;
          dev->contacts[i].fresh = false;
        }
    }

  if (dev->primary >= 0 && !dev->contacts[dev->primary].used)
    {
      dev->primary = -1;
    }

  if (dev->primary < 0)
    {
      dev->primary = gt911_primary(dev);
    }

  return true;
}

/****************************************************************************
 * Name: gt911_schedule
 *
 * Description:
 *   Queue the next sampling run.  A failure means the work queue is not
 *   usable, so the device stops sampling; that is worth one loud message
 *   rather than a silent dead touchscreen -- but only the first one, since
 *   every later run would fail the same way.
 *
 ****************************************************************************/

static void gt911_schedule(FAR struct gt911_dev_s *dev, clock_t delay)
{
  int ret;

  ret = work_queue(LPWORK, &dev->work, gt911_worker, dev, delay);
  if (ret < 0 && !dev->sched_error)
    {
      dev->sched_error = true;
      ierr("ERROR: GT911 sampling could not be re-armed: %d\n", ret);
    }
}

/****************************************************************************
 * Name: gt911_worker
 *
 * Description:
 *   Read the controller's coordinate buffer, publish what changed and re-arm.
 *
 *   The buffer status byte is acknowledged (written back as 0) as soon as the
 *   frame has been copied out: the datasheet requires the master to do that
 *   within one scan period, otherwise the panel keeps pulsing INT without
 *   refreshing the buffer.
 *
 ****************************************************************************/

static void gt911_worker(FAR void *arg)
{
  FAR struct gt911_dev_s *dev = (FAR struct gt911_dev_s *)arg;
  uint8_t error = 0;
  uint8_t status = 0;
  uint8_t frame[CONFIG_INPUT_GT911_MAX_POINTS * GT911_POINT_GROUP_LEN];
  unsigned int npoints;
  bool ready = false;
  bool activity = false;
  irqstate_t flags;
  bool seen;
  int ret;

  /* Consume the "an edge was seen" flag handed over by the interrupt, so it
   * accounts for edges that arrived while this run was in progress.
   */

  flags = spin_lock_irqsave(&dev->lock);
  seen = dev->seen;
  dev->seen = false;
  spin_unlock_irqrestore(&dev->lock, flags);

  ret = gt911_read(dev, GT911_REG_STATUS, &status, 1);
  if (ret < 0)
    {
      error = 1;
      goto report;
    }

  if ((status & GT911_STATUS_READY) != 0)
    {
      ready = true;
      npoints = (unsigned int)(status & GT911_STATUS_NPOINTS);

      if (npoints > CONFIG_INPUT_GT911_MAX_POINTS)
        {
          /* Cannot be produced by the controller, but a truncated frame is
           * worse than a clamped one.
           */

          ierr("ERROR: GT911 reported %u points, clamping to %u\n", npoints,
               (unsigned int)CONFIG_INPUT_GT911_MAX_POINTS);
          npoints = CONFIG_INPUT_GT911_MAX_POINTS;
        }

      if (npoints > 0)
        {
          ret = gt911_read(dev, GT911_REG_POINT1, frame,
                           (size_t)npoints * GT911_POINT_GROUP_LEN);
          if (ret < 0)
            {
              error = 1;
              goto report;
            }
        }

      /* Acknowledge the frame.  A failure here is not fatal for this frame --
       * the coordinates are already in hand -- but it will keep the panel
       * pulsing INT, so remember it for the log.
       */

      if (gt911_write(dev, GT911_REG_STATUS, 0) < 0)
        {
          iwarn("WARNING: GT911 buffer status acknowledge failed\n");
        }
    }

report:
  if (error != 0)
    {
      dev->errors++;

      /* Only the first failure of a run of them is worth a message: a panel
       * that has been unplugged would otherwise flood the console at the
       * sampling rate.
       */

      if (dev->errors == 1)
        {
          ierr("ERROR: GT911 I2C transfer failed: %d\n", ret);
        }
    }
  else
    {
      if (dev->errors != 0)
        {
          iinfo("GT911 I2C recovered after %lu failure(s)\n",
                (unsigned long)dev->errors);
          dev->errors = 0;
        }

      if (ready)
        {
          gt911_track(dev, frame, npoints);
          activity = gt911_report(dev);
        }
    }

  /* Re-arm.  An edge seen during this run means a newer frame is waiting, so
   * serve it immediately instead of waiting for the poll interval -- but only
   * after a clean run: with the bus failing the controller can keep the line
   * asserted, and an immediate re-arm would spin the worker at full speed
   * instead of retrying at the poll rate.
   */

  if (activity || (seen && error == 0))
    {
      gt911_schedule(dev, 0);
    }
#if CONFIG_INPUT_GT911_POLL_MS > 0
  else
    {
      gt911_schedule(dev, MSEC2TICK(CONFIG_INPUT_GT911_POLL_MS));
    }
#endif
}

/****************************************************************************
 * Name: gt911_isr
 *
 * Description:
 *   INT line interrupt handler.  The controller pulses this line once per scan
 *   period for as long as a frame is waiting to be read, so the handler does
 *   nothing but schedule the worker; any bookkeeping is done there, where I2C
 *   may be used.
 *
 ****************************************************************************/

static int gt911_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct gt911_dev_s *dev = (FAR struct gt911_dev_s *)arg;
  irqstate_t flags;

  flags = spin_lock_irqsave(&dev->lock);
  dev->seen = true;
  spin_unlock_irqrestore(&dev->lock, flags);

  /* Queueing replaces any pending poll, so the frame is read now rather than
   * at the next idle tick.  The work item is already claimed if this run is in
   * progress; the flag above then makes the worker re-arm immediately.
   */

  work_queue(LPWORK, &dev->work, gt911_worker, dev, 0);
  return OK;
}

/****************************************************************************
 * Name: gt911_control
 *
 * Description:
 *   Lower-half ioctl hook.  The framework answers TSIOC_GETMAXPOINTS and
 *   TSIOC_GRAB itself and forwards everything else here.
 *
 ****************************************************************************/

static int gt911_control(FAR struct touch_lowerhalf_s *lower, int cmd,
                         unsigned long arg)
{
  FAR struct gt911_dev_s *dev = (FAR struct gt911_dev_s *)lower;

  switch (cmd)
    {
      case TSIOC_GETRESOLUTION:
        {
          FAR struct touch_resolution_s *res =
              (FAR struct touch_resolution_s *)(uintptr_t)arg;

          if (res == NULL)
            {
              return -EINVAL;
            }

          /* The range of the coordinates this driver reports, i.e. after the
           * orientation has been applied: a mirror does not change a range,
           * but a swap exchanges the two, so they have to follow it.
           */

          res->res_x = dev->orient.swapxy ? dev->ymirror : dev->xmirror;
          res->res_y = dev->orient.swapxy ? dev->xmirror : dev->ymirror;
          return OK;
        }

      case TSIOC_GETFWVERSION:
        {
          FAR uint32_t *version = (FAR uint32_t *)(uintptr_t)arg;

          if (version == NULL)
            {
              return -EINVAL;
            }

          *version = dev->fw_version;
          return OK;
        }

      default:
        return -ENOTTY;
    }
}

/****************************************************************************
 * Name: gt911_write_buf
 *
 * Description:
 *   Write len bytes starting at reg in one transfer: the 16-bit register
 *   address followed by the data, so the controller sees a single
 *uninterrupted command and stores the payload at consecutive registers.
 *
 *   len may be any size up to GT911_WRITE_MAX - 2: the adapter's transmit path
 *   refills its FIFO for transfers longer than 32 bytes, so a whole
 *   configuration download can go out in one transfer.
 *
 ****************************************************************************/

static int gt911_write_buf(FAR struct gt911_dev_s *dev, uint16_t reg,
                           FAR const uint8_t *buf, size_t len)
{
  uint8_t msg[GT911_WRITE_MAX];
  struct i2c_msg_s xfer;

  DEBUGASSERT(len <= GT911_WRITE_MAX - 2);

  msg[0] = (uint8_t)(reg >> 8);
  msg[1] = (uint8_t)(reg & 0xff);
  memcpy(&msg[2], buf, len);

  xfer.frequency = CONFIG_INPUT_GT911_I2C_FREQUENCY;
  xfer.addr = dev->addr;
  xfer.flags = 0;
  xfer.buffer = msg;
  xfer.length = len + 2;

  return I2C_TRANSFER(dev->i2c, &xfer, 1);
}

/****************************************************************************
 * Name: gt911_write
 *
 * Description:
 *   Write one register.
 *
 ****************************************************************************/

static int gt911_write(FAR struct gt911_dev_s *dev, uint16_t reg,
                       uint8_t value)
{
  return gt911_write_buf(dev, reg, &value, 1);
}

/****************************************************************************
 * Name: gt911_config_read
 *
 * Description:
 *   Read back the controller's configuration area and judge it.
 *
 *   The checksum definition is the two's complement of the byte sum over
 *   0x8047..0x80fe, i.e. (0 - sum), and it is not a guess: every table the
 *   vendor ships for this board satisfies it (the chip's own stored byte in
 *   each vendor table equals 0x100 - (sum & 0xff), with the apply flag making
 *   the sum over all 186 bytes 0x01).
 *
 *   A blank area -- every byte zero -- is reported separately from a corrupt
 *   one, because all-zero data is trivially self-consistent: its sum is zero
 *   and so is the checksum, so a naive check calls an unprogrammed module
 *   valid.  That case is the whole reason the controller can look healthy and
 *   sense nothing.
 *
 ****************************************************************************/

static void gt911_config_read(FAR struct gt911_dev_s *dev,
                              FAR struct gt911_cfg_info_s *info)
{
  uint8_t chunk[GT911_CONFIG_CHUNK];
  uint16_t reg = GT911_REG_CONFIG_VER;
  unsigned int left = GT911_CONFIG_LEN;
  unsigned int n;
  unsigned int i;
  int ret;

  memset(info, 0, sizeof(*info));

  while (left > 0)
    {
      n = left > GT911_CONFIG_CHUNK ? GT911_CONFIG_CHUNK : left;

      ret = gt911_read(dev, reg, chunk, n);
      if (ret < 0)
        {
          iwarn("WARNING: GT911 configuration read failed at 0x%04x: %d\n",
                reg, ret);
          return;
        }

      for (i = 0; i < n; i++)
        {
          info->sum += chunk[i];
        }

      /* Keep the fields of interest, which all live in the first chunk */

      if (reg == GT911_REG_CONFIG_VER && n >= 6)
        {
          info->version = chunk[0];
          info->xmax = (uint16_t)chunk[1] | ((uint16_t)chunk[2] << 8);
          info->ymax = (uint16_t)chunk[3] | ((uint16_t)chunk[4] << 8);
          info->contacts = chunk[5];
        }

      reg += n;
      left -= n;
    }

  ret = gt911_read(dev, GT911_REG_CONFIG_CHKSUM, &info->chksum, 1);
  if (ret < 0)
    {
      iwarn("WARNING: GT911 configuration checksum unreadable: %d\n", ret);
      return;
    }

  info->readable = true;
  info->expected = (uint8_t)(0u - info->sum);
  info->blank = (info->sum == 0u && info->version == 0u && info->chksum == 0u);
  info->valid = !info->blank && info->chksum == info->expected;
}

/****************************************************************************
 * Name: gt911_config_log
 *
 * Description:
 *   Report the configuration state in one line, and say what it implies.
 *
 ****************************************************************************/

static void gt911_config_log(FAR const struct gt911_cfg_info_s *info)
{
  if (!info->readable)
    {
      iwarn("WARNING: GT911 configuration could not be read at all\n");
      return;
    }

  iinfo("GT911 configuration: version 0x%02x, X/Y range %u x %u, "
        "contacts 0x%02x, checksum 0x%02x (expected 0x%02x)\n",
        info->version, info->xmax, info->ymax, info->contacts, info->chksum,
        info->expected);

  if (info->blank)
    {
      iwarn("WARNING: GT911 configuration area is blank: the "
            "module is not configured and cannot sense anything\n");
    }
  else if (!info->valid)
    {
      iwarn("WARNING: GT911 configuration checksum mismatch: "
            "the stored configuration is not coherent\n");
    }
  else
    {
      iinfo("GT911 configuration is valid\n");
    }
}

/****************************************************************************
 * Name: gt911_soft_reset
 *
 * Description:
 *   Restart the sensing engine with the real-time command 0x8040 = 2, which
 *   re-initialises the engine from the configuration area without reloading
 *   that area from the controller's non-volatile store.
 *
 *   That distinction is the whole point of using this instead of the reset
 *   pin: on a module delivered with a blank configuration area, a hardware
 *   reset discards a configuration the host has just downloaded, because the
 *   controller comes back up from its own empty store.  This command is how
 *   such a module runs a host-supplied configuration at all.
 *
 *   The controller's initialisation, including auto-calibration, takes under
 *   200 ms (datasheet 1), so that is what is allowed here before the caller
 *   asks the engine anything.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int gt911_soft_reset(FAR struct gt911_dev_s *dev)
{
  int ret;

  ret = gt911_write(dev, GT911_REG_COMMAND, GT911_CMD_SOFT_RESET);
  if (ret < 0)
    {
      ierr("ERROR: GT911 software reset command failed: %d\n", ret);
      return ret;
    }

  up_mdelay(200);
  return OK;
}

/****************************************************************************
 * Name: gt911_engine_ready
 *
 * Description:
 *   Whether the sensing engine reports a coordinate range, i.e. whether it is
 *   running a configuration.  The resolution registers are read-only and
 *   reflect the engine rather than the configuration area, so this is the
 *   difference between "the controller is holding a table" and "the controller
 *   is scanning with it" -- a controller in the first state answers every
 *   register read and never reports a touch.
 *
 ****************************************************************************/

static bool gt911_engine_ready(FAR struct gt911_dev_s *dev)
{
  return dev->xrange != 0 || dev->yrange != 0;
}

/****************************************************************************
 * Name: gt911_config_bringup
 *
 * Description:
 *   Get a board-supplied table into the sensing engine.
 *
 *   The controller cannot be expected to tell us which of the vendor's tables
 *   belongs to the module in hand, and a table that does not match the sensor
 *   leaves exactly the state this driver keeps meeting: I2C answers perfectly,
 *   the written area verifies byte for byte, the apply flag reads back set --
 *   and the sensing engine still reports no coordinate range and never raises
 *   INT.  So each candidate table is tried in turn, and each one is tried in
 *   both placements that could plausibly matter:
 *
 *     write  - write it into the controller as it is.
 *     reset  - restart the engine (real-time soft reset, which does not reload
 *              the area from the controller's non-volatile store) and write it
 *              while the engine is coming up.
 *
 *   The engine's own resolution registers decide whether a table worked, so a
 *   success is reported only when the controller is demonstrably running the
 *   configuration -- not when it merely accepted the write.
 *
 * Returned Value:
 *   Zero (OK) if some table got the engine running; a negated errno value if
 *   none did.
 *
 ****************************************************************************/

static int gt911_config_bringup(FAR struct gt911_dev_s *dev,
                                FAR const struct gt911_board_s *board)
{
  size_t t;

  for (t = 0; t < board->nconfigs; t++)
    {
      FAR const struct gt911_config_s *table = &board->configs[t];
      int placement;

      if (table->data == NULL || table->len != GT911_CONFIG_LEN)
        {
          ierr("ERROR: GT911 table %u is not %u bytes, skipping\n",
               (unsigned int)t, (unsigned int)GT911_CONFIG_LEN);
          continue;
        }

      for (placement = 0; placement < 2; placement++)
        {
          iinfo("GT911 trying table \"%s\" (%s)\n", table->name,
                placement == 0 ? "as-is" : "after an engine restart");

          if (placement == 1 && gt911_soft_reset(dev) < 0)
            {
              continue;
            }

          if (gt911_config_commit(dev, table->data, table->len, false) < 0)
            {
              continue;
            }

          gt911_geometry(dev);

          if (gt911_engine_ready(dev))
            {
              iinfo("GT911 table \"%s\" is the one: engine is "
                    "configured\n",
                    table->name);
              return OK;
            }
        }
    }

  ierr("ERROR: GT911 accepted every configuration the board supplied and "
       "reported it as applied, but its sensing engine never came up: no "
       "coordinate range, and it will not report touches\n");

  return -EIO;
}

/****************************************************************************
 * Name: gt911_config_commit
 *
 * Description:
 *   Write a configuration table to the controller and check what it reports
 *   afterwards.
 *
 *   Two write styles are supported, because they are the two that real drivers
 *   for this controller use and only one of them comes from a driver that
 *   supports programming a module that was delivered without a configuration:
 *
 *     flat  - payload, checksum and apply flag in ONE transfer.  This is what
 *             the vendor's own Linux driver does (it hands its device-tree
 *             array to a single i2c write).
 *     split - payload first, then the checksum and apply flag in a SECOND
 *             transfer.  This is what this tree's ArtInChip driver does, and
 *             that is the only driver here with an explicit "load firmware at
 *             boot" path for unprogrammed modules -- so "the controller
 *             commits an area when the applying write arrives on its own" is a
 *             real possibility rather than a hypothetical one, and cheap to
 *             try.
 *
 *   The checksum is (0 - sum) & 0xff over the payload.  That definition is not
 *   a guess: the value the panel vendor's own table carries for it is exactly
 *   what this produces, and so is the value in every table of the vendor's
 *   Linux device trees in the Debian image (verified against all of them).
 *
 *   The result is read back and judged with the same code that reports the
 *   state at start-up, so the log states what the controller actually holds --
 *   not what was sent.
 *
 * Input Parameters:
 *   dev   - Driver state.
 *   cfg   - Configuration payload, GT911_CONFIG_LEN bytes.
 *   len   - Length of cfg.
 *   split - True for the split write style, false for the flat one.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int gt911_config_commit(FAR struct gt911_dev_s *dev,
                               FAR const uint8_t *cfg, size_t len, bool split)
{
  struct gt911_cfg_info_s info;
  uint8_t tail[2];
  uint8_t fresh = 0;
  unsigned int sum = 0;
  unsigned int i;
  int ret;

  if (len != GT911_CONFIG_LEN)
    {
      ierr("ERROR: GT911 configuration table must be %u bytes, got %u\n",
           (unsigned int)GT911_CONFIG_LEN, (unsigned int)len);
      return -EINVAL;
    }

  for (i = 0; i < len; i++)
    {
      sum += cfg[i];
    }

  tail[0] = (uint8_t)(0u - sum);
  tail[1] = 1;

  iinfo("GT911 configuration write (%s): version 0x%02x, X/Y range %u x %u, "
        "contacts 0x%02x, checksum 0x%02x\n",
        split ? "payload then checksum" : "one transfer", cfg[0],
        (unsigned int)cfg[1] | ((unsigned int)cfg[2] << 8),
        (unsigned int)cfg[3] | ((unsigned int)cfg[4] << 8), cfg[5], tail[0]);

  if (split)
    {
      ret = gt911_write_buf(dev, GT911_REG_CONFIG_VER, cfg, len);
      if (ret < 0)
        {
          ierr("ERROR: GT911 configuration payload write failed: %d\n", ret);
          return ret;
        }

      ret = gt911_write_buf(dev, GT911_REG_CONFIG_CHKSUM, tail, sizeof(tail));
      if (ret < 0)
        {
          ierr("ERROR: GT911 configuration checksum/apply write failed: "
               "%d\n",
               ret);
          return ret;
        }
    }
  else
    {
      uint8_t msg[GT911_CONFIG_LEN + 2];

      memcpy(msg, cfg, len);
      msg[len] = tail[0];
      msg[len + 1] = tail[1];

      ret = gt911_write_buf(dev, GT911_REG_CONFIG_VER, msg, sizeof(msg));
      if (ret < 0)
        {
          ierr("ERROR: GT911 configuration write failed: %d\n", ret);
          return ret;
        }
    }

  /* Give the controller time to validate and store the configuration before
   * reading it back.
   */

  up_mdelay(100);

  gt911_config_read(dev, &info);
  gt911_config_log(&info);

  if (!info.valid)
    {
      ierr("ERROR: GT911 configuration did not take effect\n");
      return -EIO;
    }

  /* Confirm the apply flag landed too.  The payload can survive a write while
   * the flag does not (the datasheet is explicit that a modified area takes
   * effect only once the flag is set), and a table that is present but not
   * applied is for the engine exactly what no table is.
   */

  ret = gt911_read(dev, GT911_REG_CONFIG_FRESH, &fresh, 1);
  if (ret >= 0)
    {
      iinfo("GT911 configuration apply flag 0x%02x\n", fresh);

      if (fresh == 0)
        {
          iwarn("WARNING: GT911 configuration apply flag is clear: the "
                "controller is not using the table\n");
        }
    }

  return OK;
}

/****************************************************************************
 * Name: gt911_geometry
 *
 * Description:
 *   Read and log the controller's runtime identity and geometry: firmware
 *   version, module vendor id, and the X/Y coordinate resolution.
 *
 *   These are the registers that say what the sensing engine is actually
 *   doing, as opposed to what the configuration area contains.  They read as
 *   zero (and the vendor id as 0xff) while the engine is unconfigured, and
 *   only change once the engine has been initialised against a configuration
 *   -- which is why this is called both at probe time and again after a
 *   configuration download followed by a reset: comparing the two is what
 *   distinguishes "the table was written" from "the engine is running it".
 *
 *   The resolution is also the range the reported coordinates arrive in, which
 *   is what the orientation's mirrors are computed against: it is read again
 *   after a configuration download, and gt911_orientation_setup() takes
 *   whichever value this leaves behind.
 *
 ****************************************************************************/

static void gt911_geometry(FAR struct gt911_dev_s *dev)
{
  uint8_t version[2];
  uint8_t range[2];
  uint16_t xrange = 0;
  uint16_t yrange = 0;

  /* Read into its own variable rather than a buffer: it is only reported, and
   * taking its address keeps it "used" when the info log below is compiled
   * away, which it is unless CONFIG_DEBUG_INPUT_INFO is set.
   */

  uint8_t vendor = 0xff;

  if (gt911_read(dev, GT911_REG_FW_VERSION, version, sizeof(version)) >= 0)
    {
      dev->fw_version = (uint16_t)version[0] | ((uint16_t)version[1] << 8);
    }

  if (gt911_read(dev, GT911_REG_XRES, range, sizeof(range)) >= 0)
    {
      xrange = (uint16_t)range[0] | ((uint16_t)range[1] << 8);
    }

  if (gt911_read(dev, GT911_REG_YRES, range, sizeof(range)) >= 0)
    {
      yrange = (uint16_t)range[0] | ((uint16_t)range[1] << 8);
    }

  /* Remembered so the caller can tell whether the sensing engine came up
   * configured: these registers are read-only and reflect the state of the
   * engine, not of the configuration area.
   */

  dev->xrange = xrange;
  dev->yrange = yrange;

  /* 0x814a identifies the module option the touch sensor was built with.  It
   * is the only field that ties this specific module to the vendor's own
   * record of it, so it is worth having in a bring-up log.
   */

  gt911_read(dev, GT911_REG_VENDOR_ID, &vendor, 1);

  iinfo("GT911 firmware version 0x%04x, output range %u x %u, module vendor "
        "id %u\n",
        dev->fw_version, xrange, yrange, vendor);
}

/****************************************************************************
 * Name: gt911_probe
 *
 * Description:
 *   Read the product id and the runtime geometry, and keep the firmware
 *   version for TSIOC_GETFWVERSION.
 *
 *   The id read doubles as the presence check: a device that does not answer
 *   at all is the one reliable "no panel" answer.  An id that answers but does
 *   not look like a GT911 is only warned about, because refusing to register
 *   would leave a bring-up with no device node to poke at, and the framing
 *   below is identical across the GT9 family anyway.
 *
 ****************************************************************************/

static int gt911_probe(FAR struct gt911_dev_s *dev)
{
  uint8_t id[GT911_PRODUCT_ID_LEN];
  int ret;

  ret = gt911_read(dev, GT911_REG_PRODUCT_ID, id, sizeof(id));
  if (ret < 0)
    {
      ierr("ERROR: GT911 does not answer on address 0x%02x: %d\n", dev->addr,
           ret);
      return ret;
    }

  if (id[0] != '9' || id[1] != '1' || id[2] != '1')
    {
      iwarn("WARNING: GT911 unexpected product id %02x %02x %02x %02x\n",
            id[0], id[1], id[2], id[3]);
    }
  else
    {
      iinfo("GT911 product id \"%c%c%c%c\" on address 0x%02x\n", id[0], id[1],
            id[2], id[3], dev->addr);
    }

  gt911_geometry(dev);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int gt911_register(FAR const char *devpath, FAR struct i2c_master_s *i2c,
                   uint8_t i2c_devaddr, FAR const struct gt911_board_s *board,
                   FAR const struct gt911_orientation_s *oriented)
{
  FAR struct gt911_dev_s *dev;
  struct gt911_cfg_info_s cfg;
  int ret;

  DEBUGASSERT(devpath != NULL && i2c != NULL && board != NULL);

  dev = kmm_zalloc(sizeof(struct gt911_dev_s));
  if (dev == NULL)
    {
      ierr("ERROR: GT911 allocation failed\n");
      return -ENOMEM;
    }

  dev->contacts = kmm_zalloc(sizeof(struct gt911_contact_s) *
                             CONFIG_INPUT_GT911_MAX_POINTS);
  if (dev->contacts == NULL)
    {
      kmm_free(dev);
      ierr("ERROR: GT911 contact allocation failed\n");
      return -ENOMEM;
    }

  /* A NULL orientation means "no transform": the device state is zeroed, so
   * nothing else is needed for the identity case.
   */

  if (oriented != NULL)
    {
      dev->orient = *oriented;
    }

  dev->i2c = i2c;
  dev->addr = i2c_devaddr;
  dev->primary = -1;
  spin_lock_init(&dev->lock);

  /* Power up and reset the panel.  The board owns that sequence because it is
   * where the INT line has to be driven to the level that selects the slave
   * address, and it must be complete before the first transfer below.
   */

  if (board->set_power != NULL)
    {
      ret = board->set_power(board, true);
      if (ret < 0)
        {
          ierr("ERROR: GT911 panel power-up failed: %d\n", ret);
          goto err_free;
        }
    }

  /* When the caller does not know which address the panel latched, probe the
   * two the datasheet defines.  0 is not a valid GT911 address, so it makes a
   * safe "auto" value.
   */

  if (dev->addr == 0)
    {
      dev->addr = GT911_I2C_ADDR_INT_HIGH;
      ret = gt911_probe(dev);

      if (ret < 0)
        {
          dev->addr = GT911_I2C_ADDR_INT_LOW;
          ret = gt911_probe(dev);
        }
    }
  else
    {
      ret = gt911_probe(dev);
    }

  if (ret < 0)
    {
      goto err_power;
    }

  /* Report the configuration the controller is running with, and program the
   * board's table if it has none.  This is the step that decides whether the
   * module can sense at all: a blank configuration area answers every read
   * above and never produces a coordinate frame, so without this a bring-up
   * looks perfectly healthy and the touchscreen is silently deaf.
   *
   * A module that already carries a valid configuration is left alone -- its
   * sensing parameters belong to the touch sensor and are not ours to guess at
   * -- unless CONFIG_INPUT_GT911_CONFIG_FORCE asks for a rewrite, which is how
   * a different table is tried on a module that has already been programmed.
   */

  gt911_config_read(dev, &cfg);
  gt911_config_log(&cfg);

  if (board->configs != NULL && board->nconfigs > 0)
    {
      bool apply = !cfg.valid;

#ifdef CONFIG_INPUT_GT911_CONFIG_FORCE
      apply = true;
#endif

      if (apply)
        {
          /* A failure here is reported but not fatal: the device node still
           * gets registered, so a bring-up can be inspected and the table
           * corrected rather than leaving no device at all.
           */

          if (gt911_config_bringup(dev, board) < 0)
            {
              ierr("ERROR: GT911 could not get a configuration into its "
                   "sensing engine, continuing without it\n");
            }
        }
      else
        {
          iinfo("GT911 module already configured, no table downloaded "
                "(set CONFIG_INPUT_GT911_CONFIG_FORCE to overwrite)\n");
        }
    }
  else if (!cfg.valid)
    {
      iwarn("WARNING: GT911 has no usable configuration and the board "
            "supplies no table: the touchscreen will not report anything\n");
    }

  /* State plainly whether the engine ended up configured.  This is the one
   * line that separates "the controller holds a configuration" from "the
   * controller is running one": the coordinate range registers are read-only
   * and report zero until the engine has been initialised against a
   * configuration, so a zero here means no touch will ever be reported, no
   * matter how healthy everything else looks.
   */

  if (dev->xrange == 0 && dev->yrange == 0)
    {
      ierr("ERROR: GT911 sensing engine reports no coordinate range: it is "
           "not running a configuration and will not report touches\n");
    }
  else
    {
      iinfo("GT911 sensing engine is configured for %u x %u\n", dev->xrange,
            dev->yrange);
    }

  /* The engine is in its final state now, so this is the point at which the
   * board's orientation can be resolved against it and reported.
   */

  gt911_orientation_setup(dev);

  /* Register with the generic touchscreen framework.  maxpoint is what the
   * framework reports through TSIOC_GETMAXPOINTS and what sizes every sample
   * this driver pushes, so the two can never disagree.
   */

  dev->lower.maxpoint = CONFIG_INPUT_GT911_MAX_POINTS;
  dev->lower.control = gt911_control;

  ret = touch_register(&dev->lower, devpath, CONFIG_INPUT_GT911_SAMPLE_CACHES);
  if (ret < 0)
    {
      ierr("ERROR: GT911 touch_register(%s) failed: %d\n", devpath, ret);
      goto err_power;
    }

  /* Start sampling before the interrupt is unmasked, so an edge that arrives
   * during bring-up is remembered in the work queue instead of being lost.
   *
   * This is deliberately the last step that unwinds on failure: once the work
   * item has been queued, the worker owns the device state, and there is no
   * way to recall a run that may already be executing -- so nothing below here
   * may free it.
   */

  ret = work_queue(LPWORK, &dev->work, gt911_worker, dev, 0);
  if (ret < 0)
    {
      ierr("ERROR: GT911 sampling could not be started: %d\n", ret);
      touch_unregister(&dev->lower, devpath);
      goto err_power;
    }

  /* The INT line is optional: the poll keeps sampling the controller without
   * it, at the cost of latency, so a board that cannot attach the interrupt
   * gets a warning rather than no touchscreen at all.
   */

  if (board->irq_attach != NULL)
    {
      ret = board->irq_attach(board, gt911_isr, dev);
      if (ret < 0)
        {
          iwarn("WARNING: GT911 interrupt attach failed (%d), falling back "
                "to polling only\n",
                ret);
        }
      else
        {
          dev->irq = true;
        }
    }

  if (dev->irq && board->irq_enable != NULL)
    {
      board->irq_enable(board, true);
    }

  iinfo("GT911 registered as %s (%u points, poll %d ms, %s)\n", devpath,
        (unsigned int)CONFIG_INPUT_GT911_MAX_POINTS,
        CONFIG_INPUT_GT911_POLL_MS, dev->irq ? "interrupt" : "polling only");
  return OK;

err_power:
  if (board->set_power != NULL)
    {
      board->set_power(board, false);
    }

err_free:
  kmm_free(dev->contacts);
  kmm_free(dev);
  return ret;
}

#endif /* CONFIG_INPUT_GT911 */
