/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_gpio.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <nuttx/atomic.h>
#include <nuttx/ioexpander/gpio.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>

#include "arm64_arch.h"
#include "hardware/rk3576_gpio.h"
#include "rk3576_gpio.h"

/***************************************************************************
 * Pre-processor Definitions
 ***************************************************************************/

/* Pin numbers */

#define RK_PIN_A0 0x00
#define RK_PIN_A1 0x01
#define RK_PIN_A2 0x02
#define RK_PIN_A3 0x03
#define RK_PIN_A4 0x04
#define RK_PIN_A5 0x05
#define RK_PIN_A6 0x06
#define RK_PIN_A7 0x07
#define RK_PIN_B0 0x08
#define RK_PIN_B1 0x09
#define RK_PIN_B2 0x0a
#define RK_PIN_B3 0x0b
#define RK_PIN_B4 0x0c
#define RK_PIN_B5 0x0d
#define RK_PIN_B6 0x0e
#define RK_PIN_B7 0x0f
#define RK_PIN_C0 0x10
#define RK_PIN_C1 0x11
#define RK_PIN_C2 0x12
#define RK_PIN_C3 0x13
#define RK_PIN_C4 0x14
#define RK_PIN_C5 0x15
#define RK_PIN_C6 0x16
#define RK_PIN_C7 0x17
#define RK_PIN_D0 0x18
#define RK_PIN_D1 0x19
#define RK_PIN_D2 0x1a
#define RK_PIN_D3 0x1b
#define RK_PIN_D4 0x1c
#define RK_PIN_D5 0x1d
#define RK_PIN_D6 0x1e
#define RK_PIN_D7 0x1f

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* GPIO bank base addresses.
 * Banks are NOT uniformly spaced: GPIO0 is at a different base range
 * than GPIO1-4. Use this lookup table instead of arithmetic.
 */

const uint32_t g_gpio_base[RK3576_GPIO_NPORTS] = {
  RK3576_GPIO0_ADDR, RK3576_GPIO1_ADDR, RK3576_GPIO2_ADDR,
  RK3576_GPIO3_ADDR, RK3576_GPIO4_ADDR,
};

/* IOMUX register base offsets for each 8-pin group per bank. */

const struct rk3576_iomux_group
  g_iomux_groups[RK3576_GPIO_NPORTS][4] =
{
  /* GPIO0: offsets */
  [0] = {
    { .offset = 0x0000 },  /* group A: pins 0-7 */
    { .offset = 0x0008 },  /* group B: pins 8-15 (B4-B7 add 0x1FF4) */
    { .offset = 0x2004 },  /* group C: pins 16-23 */
    { .offset = 0x200C },  /* group D: pins 24-31 */
  },

  /* GPIO1: offsets */
  [1] = {
    { .offset = 0x4020 },
    { .offset = 0x4028 },
    { .offset = 0x4030 },
    { .offset = 0x4038 },
  },

  /* GPIO2: offsets */
  [2] = {
    { .offset = 0x4040 },
    { .offset = 0x4048 },
    { .offset = 0x4050 },
    { .offset = 0x4058 },
  },

  /* GPIO3: offsets */
  [3] = {
    { .offset = 0x4060 },
    { .offset = 0x4068 },
    { .offset = 0x4070 },
    { .offset = 0x4078 },
  },

  /* GPIO4: offsets */
  [4] = {
    { .offset = 0x4080 },
    { .offset = 0x4088 },
    { .offset = 0xA390 },
    { .offset = 0xB398 },
  },
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Stateful GPIO handle.  The NuttX gpio_dev_s base lives at the head so the
 * same handle can be passed straight to gpio_pin_register() by the upper
 * half (board layer); RK3576-specific state trails it (the NuttX gpio_dev_s
 * doc permits "lower-half information" to follow).
 */

struct rk3576_gpio_dev_s
{
  struct gpio_dev_s gpio;       /* MUST be first */
  gpio_pinset_t pinset;         /* Port|pin identity only (no function bits) */
  unsigned int port;            /* Cached port (0-4) */
  unsigned int pin;             /* Cached pin  (0-31) */
  enum rk3576_gpio_mode_e mode; /* Last direction set via set_mode() */
  enum rk3576_gpio_pull_e pull; /* Last pull set via set_pull() */
  rk3576_gpio_irq_callback_t callback; /* Interrupt callback (driver-level) */
  bool irq_enabled;                    /* Interrupt currently unmasked */

  /* Reference count guarding lifetime against concurrent put() while an ISR
   * is still dispatching to this handle.
   *
   *   - rk3576_gpio_get() sets it to 1 (owned by the handle's caller).
   *   - The ISR borrows a reference (atomic +1) under g_gpio_lock before
   *     touching this handle, and releases it (atomic -1) afterwards, so a
   *     concurrent rk3576_gpio_put() can never free the struct while the ISR
   *     is still using it.
   *   - The final release (1 -> 0) frees the struct, on whichever thread
   *     dropped the last reference (ISR or put()).
   */

  atomic_t refs;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static spinlock_t g_gpio_lock = SP_UNLOCKED;

/* Exclusive-claim marker per pin.  This is decoupled from g_gpio_devs so
 * the claim check can be done and committed in a tiny critical section,
 * while the actual kmm_zalloc() and hardware configuration happen outside
 * the spin-lock (they are slow and may context-switch).
 */

static bool g_gpio_claimed[RK3576_GPIO_NPORTS][RK3576_GPIO_NPINS];

/* Single claim/dispatch/state table: slot[port][pin] -> handle.
 *  - NULL        : pin not claimed
 *  - non-NULL    : pin claimed
 * The ISR uses this table to dispatch the callback for a pin.  Exclusivity
 * itself is enforced by g_gpio_claimed above, NOT by this pointer's value.
 */

static struct rk3576_gpio_dev_s
    *g_gpio_devs[RK3576_GPIO_NPORTS][RK3576_GPIO_NPINS];

/* Per-bank GIC interrupt lines: 4 groups (A/B/C/D) per bank.  Each group
 * covers 8 pins.  Attached/enabled on demand (not all at boot).
 */

static const unsigned int g_gpio_group_irqs[RK3576_GPIO_NPORTS][4] = {
  { RK3576_IRQ_GPIO0_0, RK3576_IRQ_GPIO0_1, RK3576_IRQ_GPIO0_2,
    RK3576_IRQ_GPIO0_3 },
  { RK3576_IRQ_GPIO1_0, RK3576_IRQ_GPIO1_1, RK3576_IRQ_GPIO1_2,
    RK3576_IRQ_GPIO1_3 },
  { RK3576_IRQ_GPIO2_0, RK3576_IRQ_GPIO2_1, RK3576_IRQ_GPIO2_2,
    RK3576_IRQ_GPIO2_3 },
  { RK3576_IRQ_GPIO3_0, RK3576_IRQ_GPIO3_1, RK3576_IRQ_GPIO3_2,
    RK3576_IRQ_GPIO3_3 },
  { RK3576_IRQ_GPIO4_0, RK3576_IRQ_GPIO4_1, RK3576_IRQ_GPIO4_2,
    RK3576_IRQ_GPIO4_3 },
};

/* Whether each group's GIC IRQ has been attached (irq_attach) yet. */

static bool g_gpio_group_irq_attached[RK3576_GPIO_NPORTS][4];

/* Per-group GIC-line reference count: number of pins in that 8-pin group
 * currently enabled (INTMASK cleared).  When it drops to 0 the group's GIC
 * line is disabled (up_disable_irq); when it rises from 0 the line is
 * enabled (up_enable_irq).
 */

static int g_gpio_group_irq_refcount[RK3576_GPIO_NPORTS][4];

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_gpio_isr(int irq, void *context, void *arg);
static void rk3576_gpio_update_pintype(FAR struct rk3576_gpio_dev_s *dev);
static void
rk3576_gpio_irq_disable_internal(FAR struct rk3576_gpio_dev_s *dev);

static int rk3576_gpio_op_read(FAR struct gpio_dev_s *dev, FAR bool *value);
static int rk3576_gpio_op_write(FAR struct gpio_dev_s *dev, bool value);
static int rk3576_gpio_op_attach(FAR struct gpio_dev_s *dev,
                                 pin_interrupt_t callback);
static int rk3576_gpio_op_enable(FAR struct gpio_dev_s *dev, bool enable);
static int rk3576_gpio_op_setpintype(FAR struct gpio_dev_s *dev,
                                     enum gpio_pintype_e pintype);

/* Pin interface vtable installed on every claimed gpio_dev_s handle. */

static const struct gpio_operations_s g_rk3576_gpio_operations = {
  .go_read = rk3576_gpio_op_read,
  .go_write = rk3576_gpio_op_write,
  .go_attach = rk3576_gpio_op_attach,
  .go_enable = rk3576_gpio_op_enable,
  .go_setpintype = rk3576_gpio_op_setpintype,
  .go_setdebounce = NULL,
  .go_setmask = NULL,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_gpio_dirout
 *
 * Description:
 *   Set the direction of a GPIO pin to output.
 *
 ****************************************************************************/

static inline void rk3576_gpio_dirout(unsigned int port, unsigned int pin)
{
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_SWPORTA_DDR(port), pin, 1);
}

/****************************************************************************
 * Name: rk3576_gpio_dirin
 *
 * Description:
 *   Set the direction of a GPIO pin to input.
 *
 ****************************************************************************/

static inline void rk3576_gpio_dirin(unsigned int port, unsigned int pin)
{
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_SWPORTA_DDR(port), pin, 0);
}

/****************************************************************************
 * Name: rk3576_iomux_set
 *
 * Description:
 *   Set the IOMUX function for a single pin.
 *
 *   RK3576 uses 4-bit wide IOMUX registers with hiword-mask write scheme.
 *   Each 8-pin group spans 2 registers (4 pins each).
 *
 *   Special case: GPIO0B pins 12-15 (RK_PB4-RK_PB7) have an additional
 *   0x1FF4 offset applied to their register.
 *
 * Input Parameters:
 *   ioc_base - IOC register base address
 *   port     - GPIO bank number (0-4)
 *   pin      - Pin number within bank (0-31)
 *   mux      - IOMUX function select value (0-15)
 *
 ****************************************************************************/

static void rk3576_iomux_set(uint32_t ioc_base, unsigned int port,
                             unsigned int pin, unsigned int mux)
{
  unsigned int group = pin / 8;
  uint32_t reg;
  uint32_t data;
  unsigned int local_pin;
  unsigned int bit;

  reg = ioc_base + g_iomux_groups[port][group].offset;

  /* GPIO0B (bank 0, pins 8-15): pins >= 12 (B4-B7) use extra offset */
  if (port == 0 && pin >= RK_PIN_B4 && pin <= RK_PIN_B7)
    {
      reg += 0x1ff4; /* GPIO0_IOC_GPIO0B_IOMUX_SEL_H */
    }

  /* Each 8-pin group: pins 0-3 in first reg, pins 4-7 in reg+4 */
  if ((pin % 8) >= 4)
    {
      reg += 4;
      local_pin = (pin % 8) - 4;
    }
  else
    {
      local_pin = pin % 8;
    }

  /* 4 bits per pin */
  bit = local_pin * 4;

  /* Hiword-mask write: upper 16 bits = mask, lower 16 bits = value */
  data = RK3576_WRITE_MASK(bit + 3, bit, mux);

  putreg32(data, reg);
}

/****************************************************************************
 * Name: rk3576_pull_set
 *
 * Description:
 *   Set pull-up/pull-down for a GPIO pin via IOC registers.
 *
 *   RK3576 uses 2 bits/pin, 8 pins/reg, PULL_TYPE_IO_1V8_ONLY.
 *   Values: 0=disable, 1=pull-down, 3=pull-up.
 *
 * Input Parameters:
 *   ioc_base - IOC register base address
 *   port     - GPIO bank number (0-4)
 *   pin      - Pin number within bank (0-31)
 *   pull     - Pull value (RK3576_PULL_DISABLE/PULL_DOWN/PULL_UP)
 *
 ****************************************************************************/

static void rk3576_pull_set(uint32_t ioc_base, unsigned int port,
                            unsigned int pin, unsigned int pull)
{
  uint32_t reg;
  unsigned int reg_offset;
  unsigned int bit;
  uint32_t data;

  /* Determine the pull register offset for this bank/pin. */
  if (port == 0 && pin < RK_PIN_B4)
    {
      reg_offset = 0x0020; /* GPIO0_AH */
    }
  else if (port == 0)
    {
      reg_offset = 0x2028 - 0x4; /* GPIO0_BH, offset adjusted */
    }
  else if (port == 1)
    {
      reg_offset = 0x6110;
    }
  else if (port == 2)
    {
      reg_offset = 0x6120;
    }
  else if (port == 3)
    {
      reg_offset = 0x6130;
    }
  else if (port == 4 && pin < 16)
    {
      reg_offset = 0x6140;
    }
  else if (port == 4 && pin < 24)
    {
      reg_offset = 0xA148 - 0x8;
    }
  else /* port == 4, pin >= RK_PIN_D0 */
    {
      reg_offset = 0xB14C - 0xc;
    }

  /* 8 pins per register, 2 bits per pin.
   * Add offset for the register within the bank (pin/8 * 4).
   */
  reg_offset += (pin / RK3576_PULL_PINS_PER_REG) * 4;
  bit = (pin % RK3576_PULL_PINS_PER_REG) * RK3576_PULL_BITS_PER_PIN;

  reg = ioc_base + reg_offset;

  /* Hiword-mask write */
  data = RK3576_WRITE_MASK(bit + 1, bit, pull);

  putreg32(data, reg);
}

/****************************************************************************
 * Name: rk3576_drive_to_hw
 *
 * Description:
 *   Convert a public drive strength (enum rk3576_gpio_drive_e) to the
 *   hardware register value for the given GPIO class (4-level or 6-level).
 *
 *   RK3576_GPIO_DRIVE_DEFAULT resolves to the hardware reset value:
 *     4-level GPIOs → 50 ohms, 6-level GPIOs → 40 ohms.
 *
 *   4-level GPIOs (GPIO0_A, GPIO0_B0~3, GPIO4_D0~1):
 *     100 ohms → 0b0000
 *      50 ohms → 0b0010
 *      33 ohms → 0b0001
 *      25 ohms → 0b0011
 *      66/40 ohms → ERROR (not supported on 4-level GPIOs)
 *
 *   6-level GPIOs (GPIO0_B4~7, GPIO1, GPIO2, GPIO3, GPIO4_A/B/C):
 *     100 ohms → 0b0000
 *      66 ohms → 0b0100
 *      50 ohms → 0b0010
 *      40 ohms → 0b0110
 *      33 ohms → 0b0001
 *      25 ohms → 0b0101
 *
 * Input Parameters:
 *   is_4level - true if the GPIO uses 4-level drive, false for 6-level
 *   drive     - Drive strength (enum rk3576_gpio_drive_e)
 *   hw_val    - Output: 4-bit hardware register value
 *
 * Returned Value:
 *   true on success, false if the drive strength is not valid for this
 *   GPIO class.
 *
 ****************************************************************************/

static bool rk3576_drive_to_hw(bool is_4level, enum rk3576_gpio_drive_e drive,
                               uint32_t *hw_val)
{
  if (is_4level)
    {
      /* 4-level GPIO: 66/40 ohms unsupported, DEFAULT = 50 ohms */
      switch (drive)
        {
          case RK3576_GPIO_DRIVE_100OHM:
            *hw_val = 0x0;
            return true; /* 0b0000 — 100 ohms */
          case RK3576_GPIO_DRIVE_50OHM:
            *hw_val = 0x2;
            return true; /* 0b0010 —  50 ohms */
          case RK3576_GPIO_DRIVE_33OHM:
            *hw_val = 0x1;
            return true; /* 0b0001 —  33 ohms */
          case RK3576_GPIO_DRIVE_25OHM:
            *hw_val = 0x3;
            return true; /* 0b0011 —  25 ohms */
          case RK3576_GPIO_DRIVE_DEFAULT:
            *hw_val = 0x2;
            return true; /* 0b0010 —  50 ohms (reset) */
          case RK3576_GPIO_DRIVE_66OHM:
          case RK3576_GPIO_DRIVE_40OHM:
            gpioerr("ERROR: drive %u not supported on 4-level GPIO\n",
                    (unsigned int)drive);
            return false;
          default:
            gpioerr("ERROR: invalid drive enum %u\n", (unsigned int)drive);
            return false;
        }
    }
  else
    {
      /* 6-level GPIO */
      switch (drive)
        {
          case RK3576_GPIO_DRIVE_100OHM:
            *hw_val = 0x0;
            return true; /* 0b0000 — 100 ohms */
          case RK3576_GPIO_DRIVE_66OHM:
            *hw_val = 0x4;
            return true; /* 0b0100 —  66 ohms */
          case RK3576_GPIO_DRIVE_50OHM:
            *hw_val = 0x2;
            return true; /* 0b0010 —  50 ohms */
          case RK3576_GPIO_DRIVE_40OHM:
            *hw_val = 0x6;
            return true; /* 0b0110 —  40 ohms */
          case RK3576_GPIO_DRIVE_33OHM:
            *hw_val = 0x1;
            return true; /* 0b0001 —  33 ohms */
          case RK3576_GPIO_DRIVE_25OHM:
            *hw_val = 0x5;
            return true; /* 0b0101 —  25 ohms */
          case RK3576_GPIO_DRIVE_DEFAULT:
            *hw_val = 0x6;
            return true; /* 0b0110 —  40 ohms (reset) */
          default:
            gpioerr("ERROR: invalid drive enum %u\n", (unsigned int)drive);
            return false;
        }
    }
}

/****************************************************************************
 * Name: rk3576_drive_set
 *
 * Description:
 *   Set drive strength for a GPIO pin via IOC registers.
 *
 *   RK3576 uses 4 bits/pin, 4 pins/reg.
 *   Supports 4-level and 6-level drive strength GPIOs; automatically
 *   selects the correct hardware encoding via rk3576_drive_to_hw().
 *
 *   Takes the public enum rk3576_gpio_drive_e (RK3576_GPIO_DRIVE_*) and
 *   converts it internally (resolving DEFAULT per GPIO class).  This lets
 *   the handle-based setter rk3576_gpio_set_drive() pass its enum straight
 *   through, and lets rk3576_gpio_get() request the default drive strength
 *   directly.
 *
 * Input Parameters:
 *   ioc_base - IOC register base address
 *   port     - GPIO bank number (0-4)
 *   pin      - Pin number within bank (0-31)
 *   drive    - Drive strength (enum rk3576_gpio_drive_e)
 *
 * Returned Value:
 *   OK on success, -EINVAL if the drive strength is invalid for this GPIO.
 *
 ****************************************************************************/

static int rk3576_drive_set(uint32_t ioc_base, unsigned int port,
                            unsigned int pin, enum rk3576_gpio_drive_e drive)
{
  uint32_t reg;
  unsigned int reg_offset;
  unsigned int bit;
  uint32_t data;
  uint32_t hw_val;
  bool is_4level;

  /* Determine the drive register offset for this bank/pin. */
  if (port == 0 && pin < RK_PIN_B4)
    {
      reg_offset = 0x0010; /* GPIO0_A + GPIO0_B[0:3]: pins 0-11 */
    }
  else if (port == 0)
    {
      reg_offset = 0x2008; /* GPIO0_B[4:7] + GPIO0_C + GPIO0_D: pins 12-31 */
    }
  else if (port == 1)
    {
      reg_offset = 0x6020;
    }
  else if (port == 2)
    {
      reg_offset = 0x6040;
    }
  else if (port == 3)
    {
      reg_offset = 0x6060;
    }
  else if (port == 4 && pin < RK_PIN_C0)
    {
      reg_offset = 0x6080; /* GPIO4_A + GPIO4_B: pins 0-15 */
    }
  else if (port == 4 && pin < RK_PIN_D0)
    {
      reg_offset = 0xA080; /* GPIO4_C: pins 16-23 */
    }
  else /* port == 4, pin >= RK_PIN_D0 */
    {
      reg_offset = 0xB080; /* GPIO4_D: pins 24-31 */
    }

  /* 4 pins per register, 4 bits per pin */
  reg_offset += (pin / RK3576_DRV_PINS_PER_REG) * 4;
  bit = (pin % RK3576_DRV_PINS_PER_REG) * RK3576_DRV_BITS_PER_PIN;

  /* Convert the drive enum to the hardware encoding for this GPIO class. */

  is_4level = RK3576_DRIVE_IS_4LEVEL(port, pin);
  if (!rk3576_drive_to_hw(is_4level, drive, &hw_val))
    {
      return -EINVAL;
    }

  reg = ioc_base + reg_offset;

  /* Hiword-mask write */
  data = RK3576_WRITE_MASK(bit + 3, bit, hw_val);

  putreg32(data, reg);

  return OK;
}

/****************************************************************************
 * Name: rk3576_schmitt_set
 *
 * Description:
 *   Enable or disable schmitt trigger for a GPIO pin via IOC registers.
 *
 *   RK3576 uses 1 bit/pin, 8 pins/reg.
 *
 * Input Parameters:
 *   ioc_base - IOC register base address
 *   port     - GPIO bank number (0-4)
 *   pin      - Pin number within bank (0-31)
 *   enable   - true to enable schmitt trigger
 *
 ****************************************************************************/

static void rk3576_schmitt_set(uint32_t ioc_base, unsigned int port,
                               unsigned int pin, bool enable)
{
  uint32_t reg;
  unsigned int reg_offset;
  unsigned int bit;
  uint32_t data;

  /* Determine the schmitt register offset for this bank/pin. */
  if (port == 0 && pin < RK_PIN_B4)
    {
      reg_offset = 0x0030; /* GPIO0_AL */
    }
  else if (port == 0)
    {
      reg_offset = 0x2040 - 0x4; /* GPIO0_BH */
    }
  else if (port == 1)
    {
      reg_offset = 0x6210;
    }
  else if (port == 2)
    {
      reg_offset = 0x6220;
    }
  else if (port == 3)
    {
      reg_offset = 0x6230;
    }
  else if (port == 4 && pin < RK_PIN_C0)
    {
      reg_offset = 0x6240;
    }
  else if (port == 4 && pin < RK_PIN_D0)
    {
      reg_offset = 0xA248 - 0x8;
    }
  else /* port == 4, pin >= RK_PIN_D0 */
    {
      reg_offset = 0xB24C - 0xc;
    }

  /* 8 pins per register, 1 bit per pin */
  reg_offset += (pin / RK3576_SMT_PINS_PER_REG) * 4;
  bit = (pin % RK3576_SMT_PINS_PER_REG) * RK3576_SMT_BITS_PER_PIN;

  reg = ioc_base + reg_offset;

  /* Hiword-mask write: single bit */
  data = RK3576_WRITE_BIT(bit, enable ? 1 : 0);

  putreg32(data, reg);
}

/****************************************************************************
 * Name: rk3576_gpio_isr
 *
 * Description:
 *   Per-group GIC ISR.  Each bank has 4 interrupt lines (one per 8-pin
 *   group A/B/C/D).  Reads the bank's INT_STATUS, dispatches to the claimed
 *   handle's callback via the single table, then writes PORTA_EOI.
 *
 *   arg encoding: low 3 bits = port, bits [4:3] = group (0-3).
 *
 ****************************************************************************/

static int rk3576_gpio_isr(int irq, void *context, void *arg)
{
  unsigned int encoded = (unsigned int)(uintptr_t)arg;
  unsigned int port = encoded & 0x7;
  unsigned int group = (encoded >> 3) & 0x3;
  unsigned int pin_start = group * 8;
  unsigned int pin_end = pin_start + 8;
  uint32_t status;
  int pin;

  if (port >= RK3576_GPIO_NPORTS)
    {
      gpioerr("Error: Invalid GPIO port: %u\n", port);
      return -EINVAL;
    }

  status = getreg32(RK3576_GPIO_INT_STATUS(port));

  /* Limit to this group's 8-pin range */

  status &= ((1u << pin_end) - 1) ^ ((1u << pin_start) - 1);

  while (status != 0)
    {
      FAR struct rk3576_gpio_dev_s *idev;
      irqstate_t flags;

      pin = __builtin_ctz(status);

      /* Borrow a reference under the same lock that publishes/removes the
       * handle.  This closes the race with rk3576_gpio_put(): either we grab
       * the handle and +1 its refcount (so a concurrent put() cannot free it
       * underneath us), or put() has already removed it and we read NULL and
       * simply skip it.  We take the spin-lock with interrupts disabled
       * (spin_lock_irqsave) because we are in interrupt context.
       */

      flags = spin_lock_irqsave(&g_gpio_lock);
      idev = g_gpio_devs[port][pin];
      if (idev != NULL)
        {
          atomic_add(&idev->refs, 1);
        }

      spin_unlock_irqrestore(&g_gpio_lock, flags);

      if (idev != NULL)
        {
          if (idev->callback != NULL && idev->irq_enabled)
            {
              idev->callback(&idev->gpio, (uint8_t)pin);
            }

          /* Release our borrowed reference; 1 -> 0 means we were the last
           * user and must free the handle.  kmm_free() is safe in this
           * interrupt context (NuttX heap supports interrup-safe free).
           */

          if (atomic_sub(&idev->refs, 1) == 1)
            {
              kmm_free(idev);
            }
        }

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_PORTA_EOI(port), pin, 1);

      status &= ~RK3576_GPIO_PIN_BIT(pin);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_gpio_update_pintype
 *
 * Description:
 *   Re-sync the handle's NuttX gp_pintype with the pin's real direction
 *   (dev->mode) and pull setting, so the /dev/gpioN upper half's ioctl
 *   permission checks (GPIOC_WRITE, GPIOC_PINTYPE, ...) and its
 *   DEBUGASSERT(dev->gp_pintype == pintype) in GPIOC_SETPINTYPE stay
 *   correct.  Called whenever the direction or pull of a claimed pin is
 *   changed via rk3576_gpio_set_mode()/set_pull()/setpintype().
 *
 *   Direction is authoritative: an output maps to GPIO_OUTPUT_PIN (drive
 *   strength is configured separately); an input maps to one of the three
 *   GPIO_INPUT_PIN variants depending on the pull resistor.
 *
 ****************************************************************************/

static void rk3576_gpio_update_pintype(FAR struct rk3576_gpio_dev_s *dev)
{
  if (dev->mode == RK3576_GPIO_OUTPUT)
    {
      /* Output: only OUTPUT pin types exist (no pull variant). */

      dev->gpio.gp_pintype = GPIO_OUTPUT_PIN;
    }
  else
    {
      /* Input: pick the pull variant to match the configured pull. */

      switch (dev->pull)
        {
          case RK3576_GPIO_PULLUP:
            dev->gpio.gp_pintype = GPIO_INPUT_PIN_PULLUP;
            break;
          case RK3576_GPIO_PULLDOWN:
            dev->gpio.gp_pintype = GPIO_INPUT_PIN_PULLDOWN;
            break;
          default:
            dev->gpio.gp_pintype = GPIO_INPUT_PIN;
            break;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_config_gpio
 *
 * Description:
 *   Deprecated stateless pinset-based GPIO configuration entry point
 *   (retained for compatibility with legacy board pin-mux callers).
 *   New code should use rk3576_gpio_get()/put() plus the fine-grained
 *   handle-based setters.
 *
 *   The pinset encoding is defined in rk3576_gpio.h and includes:
 *   - Port and pin number
 *   - Mode (input/output/alternate)
 *   - Pull-up/pull-down
 *   - Drive strength (for output/AF pins)
 *   - Alternate function number
 *
 *   NOTE: the deprecated function-bit macros (GPIO_INPUT/GPIO_ALT/GPIO_AF*,
 *   GPIO_DRV_STRENGTH_*, ...) are still honored here for compatibility; the
 *   pinset passed to rk3576_gpio_get() ignores them (identity only).
 *
 * Returned Value:
 *   OK on success
 *   A negated errno value on invalid port or pin.
 *
 ****************************************************************************/

int rk3576_config_gpio(gpio_pinset_t pinset)
{
  unsigned int port;
  unsigned int pin;
  unsigned int mode;
  enum rk3576_gpio_drive_e drive;
  int ret;
  irqstate_t flags;

  /* Extract port and pin from the configuration */

  port = (pinset & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT;
  pin = (pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;
  mode = pinset & GPIO_MODE_MASK;

  /* Map pinset drive strength bits to the public drive enum.
   * DRV_STRENGTH is a contiguous 3-bit field at bits [14:12].
   * GPIO_DRV_STRENGTH_DEFAULT resolves to the hardware reset value
   * (per GPIO class) inside rk3576_drive_set().
   */

  uint32_t drv = (pinset & GPIO_DRV_STRENGTH_MASK);
  switch (drv)
    {
      case GPIO_DRV_STRENGTH_DEFAULT:
        drive = RK3576_GPIO_DRIVE_DEFAULT;
        break;
      case GPIO_DRV_STRENGTH_100OHM:
        drive = RK3576_GPIO_DRIVE_100OHM;
        break;
      case GPIO_DRV_STRENGTH_66OHM:
        drive = RK3576_GPIO_DRIVE_66OHM;
        break;
      case GPIO_DRV_STRENGTH_50OHM:
        drive = RK3576_GPIO_DRIVE_50OHM;
        break;
      case GPIO_DRV_STRENGTH_40OHM:
        drive = RK3576_GPIO_DRIVE_40OHM;
        break;
      case GPIO_DRV_STRENGTH_33OHM:
        drive = RK3576_GPIO_DRIVE_33OHM;
        break;
      case GPIO_DRV_STRENGTH_25OHM:
        drive = RK3576_GPIO_DRIVE_25OHM;
        break;
      default:
        gpioerr("ERROR: Invalid GPIO drive strength: 0x%lx\n",
                (unsigned long)drv);
        return -EINVAL;
    }

  /* Verify that this hardware supports the selected GPIO port */

  if (port >= RK3576_GPIO_NPORTS)
    {
      gpioerr("ERROR: Invalid GPIO port: %u\n", port);
      return -EINVAL;
    }

  /* Verify pin number */

  if (pin >= RK3576_GPIO_NPINS)
    {
      gpioerr("ERROR: Invalid GPIO pin: %u\n", pin);
      return -EINVAL;
    }

  /* Disable interrupts for mutually exclusive register access */

  ret = OK;
  flags = spin_lock_irqsave(&g_gpio_lock);

  switch (mode)
    {
      case GPIO_INPUT:
        {
          /* Set IOMUX to GPIO function (AF=0) before configuring direction.
           * Bootloader may have left IOMUX in a non-GPIO state.
           */

          rk3576_iomux_set(RK3576_IOC_ADDR, port, pin,
                           (GPIO_AF0 >> GPIO_AF_SHIFT));

          /* Configure as input */

          rk3576_gpio_dirin(port, pin);

          /* Enable schmitt trigger if requested (default for clean input) */

          rk3576_schmitt_set(RK3576_IOC_ADDR, port, pin,
                             (pinset & GPIO_SCHMITT) != 0);

          /* Configure pull-up/pull-down via IOC if specified */

          if ((pinset & GPIO_PUPD_MASK) == GPIO_PULLUP)
            {
              rk3576_pull_set(RK3576_IOC_ADDR, port, pin, RK3576_PULL_UP);
            }
          else if ((pinset & GPIO_PUPD_MASK) == GPIO_PULLDOWN)
            {
              rk3576_pull_set(RK3576_IOC_ADDR, port, pin, RK3576_PULL_DOWN);
            }
          else
            {
              rk3576_pull_set(RK3576_IOC_ADDR, port, pin, RK3576_PULL_DISABLE);
            }
        }
        break;

      case GPIO_OUTPUT:
        {
          /* Set IOMUX to GPIO function (AF=0) before configuring direction.
           * Bootloader may have left IOMUX in a non-GPIO state.
           */

          rk3576_iomux_set(RK3576_IOC_ADDR, port, pin,
                           (GPIO_AF0 >> GPIO_AF_SHIFT));

          /* Set the initial output value before changing direction */

          rk3576_gpio_write(pinset, (pinset & GPIO_OUTPUT_SET) != 0);

          /* Configure as output */

          rk3576_gpio_dirout(port, pin);

          /* Disable schmitt trigger for output pins */

          rk3576_schmitt_set(RK3576_IOC_ADDR, port, pin, false);

          /* Configure drive strength */

          ret = rk3576_drive_set(RK3576_IOC_ADDR, port, pin, drive);
          if (ret < 0)
            {
              goto errout;
            }

          /* Configure pull-up/pull-down if specified */

          if ((pinset & GPIO_PUPD_MASK) == GPIO_PULLUP)
            {
              rk3576_pull_set(RK3576_IOC_ADDR, port, pin, RK3576_PULL_UP);
            }
          else if ((pinset & GPIO_PUPD_MASK) == GPIO_PULLDOWN)
            {
              rk3576_pull_set(RK3576_IOC_ADDR, port, pin, RK3576_PULL_DOWN);
            }
        }
        break;

      case GPIO_ALT:
        {
          /* Configure alternate function via IOMUX.
           * The AF number is extracted from the pinset.
           */

          unsigned int af = (pinset & GPIO_AF_MASK) >> GPIO_AF_SHIFT;

          /* Set the pin to input first, then configure IOMUX.
           * The IOMUX setting determines the actual direction.
           */

          rk3576_gpio_dirin(port, pin);

          rk3576_iomux_set(RK3576_IOC_ADDR, port, pin, af);

          /* Enable schmitt trigger for AF input (e.g. UART RX, I2C) */

          rk3576_schmitt_set(RK3576_IOC_ADDR, port, pin,
                             (pinset & GPIO_SCHMITT) != 0);

          /* Configure drive strength */

          ret = rk3576_drive_set(RK3576_IOC_ADDR, port, pin, drive);
          if (ret < 0)
            {
              goto errout;
            }
        }
        break;

      default:
        gpioerr("ERROR: Invalid GPIO mode: %u\n", mode);
        ret = -EINVAL;
        goto errout;
    }

  /* Handle EXTI interrupt configuration */

  if ((pinset & GPIO_EXTI) != 0)
    {
      /* Per-pin interrupt-controller init: open INTEN (only INTMASK gates
       * delivery), keep INTMASK masked and clear pending until the pin is
       * actually enabled/used as an interrupt. */

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTEN(port), pin, 1);
      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTMASK(port), pin, 1);
      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_PORTA_EOI(port), pin, 1);

      /* Enable schmitt trigger to prevent spurious interrupts from noise */

      rk3576_schmitt_set(RK3576_IOC_ADDR, port, pin, true);

      /* Set interrupt type (level or edge).
       * All GPIO registers use hiword-mask writes.
       */

      RK3576_GPIO_V2_WRITE_BIT(
          RK3576_GPIO_INTTYPE_LEVEL(port), pin,
          (pinset & GPIO_INTTYPE_MASK) == GPIO_INT_EDGE ? 1 : 0);

      /* Set interrupt polarity (only meaningful when BOTHEDGE is not set) */

      RK3576_GPIO_V2_WRITE_BIT(
          RK3576_GPIO_INT_POLARITY(port), pin,
          (pinset & GPIO_INTPOL_MASK) == GPIO_INT_HIGH_RISING ? 1 : 0);

      /* Set both-edge trigger if requested (for edge interrupts only) */

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INT_BOTHEDGE(port), pin,
                               (pinset & GPIO_INT_BOTHEDGE) != 0 ? 1 : 0);

      /* Clear any pending interrupt before enabling */

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_PORTA_EOI(port), pin, 1);

      /* Unmask the interrupt */

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTMASK(port), pin, 0);
    }

errout:
  spin_unlock_irqrestore(&g_gpio_lock, flags);
  return ret;
}

/****************************************************************************
 * Name: rk3576_gpio_write
 *
 * Description:
 *   Write one or zero to the selected GPIO pin.
 *
 ****************************************************************************/

void rk3576_gpio_write(gpio_pinset_t pinset, bool value)
{
  unsigned int port;
  unsigned int pin;

  port = (pinset & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT;
  pin = (pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;

  if (port >= RK3576_GPIO_NPORTS)
    {
      return;
    }

  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_SWPORTA_DR(port), pin, value);
}

/****************************************************************************
 * Name: rk3576_gpio_read
 *
 * Description:
 *   Read one or zero from the selected GPIO pin.
 *
 ****************************************************************************/

bool rk3576_gpio_read(gpio_pinset_t pinset)
{
  unsigned int port;
  unsigned int pin;

  port = (pinset & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT;
  pin = (pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;

  if (port >= RK3576_GPIO_NPORTS)
    {
      return false;
    }

  return (getreg32(RK3576_GPIO_EXT_PORT(port)) & RK3576_GPIO_PIN_BIT(pin)) !=
         0;
}

/****************************************************************************
 * Name: rk3576_gpio_get
 *
 * Description:
 *   Claim (occupy) a GPIO pin and return a stateful handle.
 *
 ****************************************************************************/

int rk3576_gpio_get(gpio_pinset_t pinset, FAR struct gpio_dev_s **handle)
{
  FAR struct rk3576_gpio_dev_s *dev;
  unsigned int port;
  unsigned int pin;
  irqstate_t flags;

  DEBUGASSERT(handle != NULL);

  *handle = NULL;

  port = (pinset & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT;
  pin = (pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;

  if (port >= RK3576_GPIO_NPORTS || pin >= RK3576_GPIO_NPINS)
    {
      gpioerr("ERROR: Invalid GPIO port/pin: %u/%u\n", port, pin);
      return -EINVAL;
    }

  /* Single-occupancy: test-and-set an exclusive marker in a small critical
   * section.  A second claim of the same pin fails fast with -EBUSY.  The
   * marker, not the g_gpio_devs pointer, is what enforces exclusivity, so
   * the slow kmm_zalloc()/hardware config below can run outside the lock
   * without a race window.
   */

  flags = spin_lock_irqsave(&g_gpio_lock);

  if (g_gpio_claimed[port][pin])
    {
      spin_unlock_irqrestore(&g_gpio_lock, flags);
      gpioerr("ERROR: GPIO%u_P%u already claimed\n", port, pin);
      return -EBUSY;
    }

  g_gpio_claimed[port][pin] = true;

  spin_unlock_irqrestore(&g_gpio_lock, flags);

  /* Allocate and initialize the handle outside the critical section:
   * kmm_zalloc() may take a while and can context-switch.
   */

  dev = kmm_zalloc(sizeof(struct rk3576_gpio_dev_s));
  if (dev == NULL)
    {
      /* Release the marker so the pin can be claimed again. */

      flags = spin_lock_irqsave(&g_gpio_lock);
      g_gpio_claimed[port][pin] = false;
      spin_unlock_irqrestore(&g_gpio_lock, flags);
      return -ENOMEM;
    }

  atomic_set(&dev->refs, 1); /* owner's reference */

  dev->pinset = pinset;
  dev->port = port;
  dev->pin = pin;
  dev->mode = RK3576_GPIO_INPUT;
  dev->pull = RK3576_GPIO_FLOAT;
  dev->callback = NULL;
  dev->irq_enabled = false;
  dev->gpio.gp_ops = &g_rk3576_gpio_operations;
  dev->gpio.gp_pintype = GPIO_INPUT_PIN;

  /* Configure a safe initial state: input, no pull, GPIO function (AF0),
   * schmitt trigger on, and drive strength reset to the per-class default.
   * This selects GPIO function (AF0) + direction/pull but does NOT switch
   * the pin to an alternate function; callers set up any AF afterwards via
   * rk3576_gpio_set_af()/set_drive()/etc.  Keeping the default as input
   * (rather than output) matches the fact that get() itself must not
   * silently enable a driver.
   */

  rk3576_iomux_set(RK3576_IOC_ADDR, port, pin, 0); /* AF0: GPIO */
  rk3576_gpio_dirin(port, pin);
  rk3576_pull_set(RK3576_IOC_ADDR, port, pin, RK3576_PULL_DISABLE);
  rk3576_schmitt_set(RK3576_IOC_ADDR, dev->port, dev->pin, true);
  rk3576_drive_set(RK3576_IOC_ADDR, port, pin, RK3576_GPIO_DRIVE_DEFAULT);

  /* Reset the interrupt trigger to a safe, deterministic default:
   * edge-triggered, rising edge (not both-edge).  The interrupt is NOT
   * enabled here — INTMASK stays masked until rk3576_gpio_irq_enable() —
   * so this only fixes the trigger shape for a later attach/enable. */

  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTTYPE_LEVEL(port), pin,
                           RK3576_GPIO_INTTYPE_EDGE_VAL);
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INT_BOTHEDGE(port), pin, 0);
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INT_POLARITY(port), pin, 1);
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTMASK(port), pin, 1);
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_PORTA_EOI(port), pin, 1);

  /* Publish the handle.  No other task can be claiming this pin now (the
   * marker still holds), so this write is safe without extra locking.
   */

  g_gpio_devs[port][pin] = dev;

  *handle = &dev->gpio;
  return OK;
}

/****************************************************************************
 * Name: rk3576_gpio_put
 *
 * Description:
 *   Release a previously claimed GPIO pin.
 *
 ****************************************************************************/

int rk3576_gpio_put(FAR struct gpio_dev_s *handle)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);
  DEBUGASSERT(g_gpio_devs[dev->port][dev->pin] == dev);

  flags = spin_lock_irqsave(&g_gpio_lock);

  /* Fully teardown the pin's IRQ accounting before freeing (mask
   * INTMASK, clear irq_enabled, drop the group's GIC-line refcount,
   * disabling the line on 1->0).  Called with the lock already held.
   */

  rk3576_gpio_irq_disable_internal(dev);

  /* Unpublish the handle FIRST so new ISR lookups see NULL, then drop the
   * owner's reference.  If an ISR is concurrently using this handle it has
   * already borrowed a reference, so the refcount will not reach 0 here and
   * the ISR (not us) will perform the final free after it is done.  If no
   * ISR is in flight, dropping the owner's reference (1 -> 0) frees here.
   */

  g_gpio_devs[dev->port][dev->pin] = NULL;
  g_gpio_claimed[dev->port][dev->pin] = false;

  if (atomic_sub(&dev->refs, 1) == 1)
    {
      spin_unlock_irqrestore(&g_gpio_lock, flags);
      kmm_free(dev);
    }
  else
    {
      spin_unlock_irqrestore(&g_gpio_lock, flags);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_gpio_read_bit / rk3576_gpio_write_bit
 *
 * Description:
 *   Stateful read/write of a single pin bit through a claimed handle.
 *
 ****************************************************************************/

bool rk3576_gpio_read_bit(FAR struct gpio_dev_s *handle)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;

  DEBUGASSERT(dev != NULL);

  return (getreg32(RK3576_GPIO_EXT_PORT(dev->port)) &
          RK3576_GPIO_PIN_BIT(dev->pin)) != 0;
}

void rk3576_gpio_write_bit(FAR struct gpio_dev_s *handle, bool value)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;

  DEBUGASSERT(dev != NULL);

  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_SWPORTA_DR(dev->port), dev->pin, value);
}

/****************************************************************************
 * Name: rk3576_gpio_set_pull
 *
 * Description:
 *   Set only the pull resistor of a claimed pin.
 *
 ****************************************************************************/

void rk3576_gpio_set_pull(FAR struct gpio_dev_s *handle,
                          enum rk3576_gpio_pull_e pull)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  unsigned int hw_pull;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);

  switch (pull)
    {
      case RK3576_GPIO_PULLUP:
        hw_pull = RK3576_PULL_UP;
        break;
      case RK3576_GPIO_PULLDOWN:
        hw_pull = RK3576_PULL_DOWN;
        break;
      case RK3576_GPIO_FLOAT:
        hw_pull = RK3576_PULL_DISABLE;
        break;
      default:
        /* Invalid enum value: flag it in DEBUG builds, and in release
         * builds simply refuse the change so the pin keeps its previous
         * pull configuration (hardware, dev->pull, and gp_pintype all stay
         * consistent).
         */

        DEBUGASSERT(0);
        return;
    }

  flags = spin_lock_irqsave(&g_gpio_lock);
  rk3576_pull_set(RK3576_IOC_ADDR, dev->port, dev->pin, hw_pull);

  /* Record the new pull and keep gp_pintype in sync. */

  dev->pull = pull;
  rk3576_gpio_update_pintype(dev);
  spin_unlock_irqrestore(&g_gpio_lock, flags);
}

/****************************************************************************
 * Name: rk3576_gpio_set_drive
 *
 * Description:
 *   Set only the drive strength of a claimed pin.
 *
 ****************************************************************************/

int rk3576_gpio_set_drive(FAR struct gpio_dev_s *handle,
                          enum rk3576_gpio_drive_e drive)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  irqstate_t flags;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  /* Pass the public drive enum straight through; rk3576_drive_set() maps it
   * to a logical level (resolving DEFAULT per GPIO class) internally.
   */

  flags = spin_lock_irqsave(&g_gpio_lock);
  ret = rk3576_drive_set(RK3576_IOC_ADDR, dev->port, dev->pin, drive);
  spin_unlock_irqrestore(&g_gpio_lock, flags);

  return ret;
}

/****************************************************************************
 * Name: rk3576_gpio_set_schmitt
 *
 * Description:
 *   Enable/disable only the schmitt trigger of a claimed pin.
 *
 ****************************************************************************/

void rk3576_gpio_set_schmitt(FAR struct gpio_dev_s *handle, bool enable)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);

  flags = spin_lock_irqsave(&g_gpio_lock);
  rk3576_schmitt_set(RK3576_IOC_ADDR, dev->port, dev->pin, enable);
  spin_unlock_irqrestore(&g_gpio_lock, flags);
}

/****************************************************************************
 * Name: rk3576_gpio_set_af
 *
 * Description:
 *   Set only the IOMUX alternate function (AF number) of a claimed pin.
 *   AF 0 selects plain GPIO.
 *
 ****************************************************************************/

void rk3576_gpio_set_af(FAR struct gpio_dev_s *handle, unsigned int af)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);
  DEBUGASSERT(af <= RK3576_AF_MAX);

  flags = spin_lock_irqsave(&g_gpio_lock);
  rk3576_iomux_set(RK3576_IOC_ADDR, dev->port, dev->pin, af);
  spin_unlock_irqrestore(&g_gpio_lock, flags);
}

/****************************************************************************
 * Name: rk3576_gpio_set_mode
 *
 * Description:
 *   Set only the GPIO direction (input/output) of a claimed pin.  Sets the
 *   DDR bit and selects GPIO (IOMUX AF0).
 *
 ****************************************************************************/

void rk3576_gpio_set_mode(FAR struct gpio_dev_s *handle,
                          enum rk3576_gpio_mode_e mode)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);
  DEBUGASSERT(mode == RK3576_GPIO_INPUT || mode == RK3576_GPIO_OUTPUT);

  flags = spin_lock_irqsave(&g_gpio_lock);

  /* Select GPIO function (AF0) then set direction. */

  rk3576_iomux_set(RK3576_IOC_ADDR, dev->port, dev->pin, 0);

  if (mode == RK3576_GPIO_OUTPUT)
    {
      rk3576_gpio_dirout(dev->port, dev->pin);
    }
  else
    {
      rk3576_gpio_dirin(dev->port, dev->pin);
    }

  /* Record the new direction and keep gp_pintype in sync. */

  dev->mode = mode;
  rk3576_gpio_update_pintype(dev);

  spin_unlock_irqrestore(&g_gpio_lock, flags);
}

/****************************************************************************
 * Name: rk3576_gpio_set_int_type
 *
 * Description:
 *   Set only the interrupt trigger type (level vs. edge) of a claimed pin.
 *   The INT_EDGE bit in INTTYPE_LEVEL controls whether the interrupt is
 *   level- or edge-triggered; only the INTMASK/INTEN path decides whether
 *   the interrupt is actually delivered.  All other pin parameters are left
 *   unchanged.
 *
 *   Note: RK3576_GPIO_INT_BOTH_EDGE (via rk3576_gpio_set_int_pol()) is only
 *   meaningful for edge-triggered pins, so set int_type to
 *   RK3576_GPIO_INT_EDGE before requesting both-edge triggering.
 *
 * Input Parameters:
 *   handle   - Handle from rk3576_gpio_get().
 *   int_type - enum rk3576_gpio_int_type_e value.
 *
 ****************************************************************************/

void rk3576_gpio_set_int_type(FAR struct gpio_dev_s *handle,
                              enum rk3576_gpio_int_type_e int_type)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);
  DEBUGASSERT(int_type == RK3576_GPIO_INT_LEVEL ||
              int_type == RK3576_GPIO_INT_EDGE);

  flags = spin_lock_irqsave(&g_gpio_lock);

  /* INTTYPE_LEVEL: 1 = edge, 0 = level.  Hiword-mask write touches only
   * this pin's bit. */

  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTTYPE_LEVEL(dev->port), dev->pin,
                           int_type == RK3576_GPIO_INT_EDGE ? 1 : 0);

  spin_unlock_irqrestore(&g_gpio_lock, flags);
}

/****************************************************************************
 * Name: rk3576_gpio_set_int_pol
 *
 * Description:
 *   Set only the interrupt polarity (active level) / trigger edge of a
 *   claimed pin.
 *
 *   - RK3576_GPIO_INT_LOW_FALLING: low level (level-trigger) or falling
 *     edge (edge-trigger) → INT_POLARITY = 0, BOTHEDGE = 0.
 *   - RK3576_GPIO_INT_HIGH_RISING: high level / rising edge →
 *     INT_POLARITY = 1, BOTHEDGE = 0.
 *   - RK3576_GPIO_INT_BOTH_EDGE: both edges → BOTHEDGE = 1 (edge-only;
 *     the pin must be edge-triggered via rk3576_gpio_set_int_type()).
 *
 *   All other pin parameters are left unchanged.  In particular this setter
 *   does not touch INTTYPE_LEVEL; both-edge and polarity only apply to a
 *   pin whose trigger type is already configured appropriately.
 *
 * Input Parameters:
 *   handle  - Handle from rk3576_gpio_get().
 *   int_pol - enum rk3576_gpio_int_pol_e value.
 *
 ****************************************************************************/

void rk3576_gpio_set_int_pol(FAR struct gpio_dev_s *handle,
                             enum rk3576_gpio_int_pol_e int_pol)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);
  DEBUGASSERT(int_pol >= RK3576_GPIO_INT_LOW_FALLING &&
              int_pol <= RK3576_GPIO_INT_BOTH_EDGE);

  flags = spin_lock_irqsave(&g_gpio_lock);

  if (int_pol == RK3576_GPIO_INT_BOTH_EDGE)
    {
      /* Both edges → set BOTHEDGE; polarity is irrelevant. */

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INT_BOTHEDGE(dev->port), dev->pin,
                               1);
    }
  else
    {
      /* Single edge/level: clear BOTHEDGE and set the polarity bit. */

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INT_BOTHEDGE(dev->port), dev->pin,
                               0);

      /* INT_POLARITY: 1 = high level / rising edge, 0 = low / falling. */

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INT_POLARITY(dev->port), dev->pin,
                               int_pol == RK3576_GPIO_INT_HIGH_RISING ? 1 : 0);
    }

  spin_unlock_irqrestore(&g_gpio_lock, flags);
}

/****************************************************************************
 * Name: rk3576_gpio_irq_attach
 *
 * Description:
 *   Attach/detach a driver-level interrupt callback on a claimed pin.
 *   Attaching a non-NULL callback lazily brings up the group's GIC IRQ.
 *
 ****************************************************************************/

int rk3576_gpio_irq_attach(FAR struct gpio_dev_s *handle,
                           rk3576_gpio_irq_callback_t callback)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  unsigned int group;
  irqstate_t flags;
  int ret;

  DEBUGASSERT(dev != NULL);

  group = dev->pin / 8;

  /* On a genuine attach (non-NULL callback) where this group's GIC vector is
   * not yet installed, install it BEFORE committing the callback.  This way,
   * if irq_attach() fails, dev->callback is left untouched and the pin stays
   * in its previous consistent state (no half-set callback).  The check
   * (attached flag -> irq_attach -> set flag) stays in one critical section
   * so it cannot race with a concurrent attach of another pin in the same
   * 8-pin group.  irq_attach() merely writes the g_irqvector[] table and is
   * safe to call while holding the spin-lock.
   */

  flags = spin_lock_irqsave(&g_gpio_lock);

  if (callback != NULL && !g_gpio_group_irq_attached[dev->port][group])
    {
      int irq = g_gpio_group_irqs[dev->port][group];
      uintptr_t arg = (uintptr_t)((group << 3) | dev->port);

      ret = irq_attach(irq, rk3576_gpio_isr, (void *)arg);
      if (ret < 0)
        {
          spin_unlock_irqrestore(&g_gpio_lock, flags);
          gpioerr("ERROR: Failed to attach IRQ %u (GPIO%u_%u): %d\n", irq,
                  dev->port, group, ret);
          return ret;
        }

      g_gpio_group_irq_attached[dev->port][group] = true;
    }

  /* Commit the callback only after the GIC vector is in place (or was
   * already in place), so a failed irq_attach() never leaves dev->callback
   * pointing at a callback whose vector was not installed. */

  dev->callback = callback;

  spin_unlock_irqrestore(&g_gpio_lock, flags);

  return OK;
}

/****************************************************************************
 * Name: rk3576_gpio_irq_enable
 *
 * Description:
 *   Unmask the interrupt on a claimed pin.  Unmasks INTMASK and enables the
 *   group's GIC line (on the 0->1 refcount transition); the caller must have
 *   attached the GIC vector via rk3576_gpio_irq_attach() beforehand.
 *
 ****************************************************************************/

void rk3576_gpio_irq_enable(FAR struct gpio_dev_s *handle)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  unsigned int group;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);

  group = dev->pin / 8;

  if (dev->irq_enabled)
    {
      return;
    }

  flags = spin_lock_irqsave(&g_gpio_lock);

  /* Bring up the GIC line on the 0->1 refcount transition. */

  if (g_gpio_group_irq_refcount[dev->port][group] == 0)
    {
      up_enable_irq(g_gpio_group_irqs[dev->port][group]);
    }

  g_gpio_group_irq_refcount[dev->port][group]++;

  /* Open INTEN for this pin (never touched elsewhere; only INTMASK gates
   * delivery) and clear any stale pending interrupt before unmasking.
   */

  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTEN(dev->port), dev->pin, 1);
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_PORTA_EOI(dev->port), dev->pin, 1);
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTMASK(dev->port), dev->pin, 0);
  dev->irq_enabled = true;

  spin_unlock_irqrestore(&g_gpio_lock, flags);
}

/****************************************************************************
 * Name: rk3576_gpio_irq_disable_internal
 *
 * Description:
 *   Core IRQ-teardown for a single claimed pin, executed with g_gpio_lock
 *   already held: mask INTMASK, clear irq_enabled, drop the 8-pin group's
 *   GIC-line reference count and disable that line on the 1->0 transition.
 *   Assumes dev->irq_enabled is true; no-op otherwise.
 *
 *   Shared by rk3576_gpio_irq_disable() (which takes the lock itself) and
 *   rk3576_gpio_put() (which already holds the lock), so the accounting
 *   cannot drift between the two teardown paths.
 *
 ****************************************************************************/

static void rk3576_gpio_irq_disable_internal(FAR struct rk3576_gpio_dev_s *dev)
{
  unsigned int group;

  if (!dev->irq_enabled)
    {
      return;
    }

  group = dev->pin / 8;

  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTMASK(dev->port), dev->pin, 1);
  dev->irq_enabled = false;

  DEBUGASSERT(g_gpio_group_irq_refcount[dev->port][group] > 0);
  g_gpio_group_irq_refcount[dev->port][group]--;

  /* Drop the GIC line on the 1->0 refcount transition. */

  if (g_gpio_group_irq_refcount[dev->port][group] == 0)
    {
      up_disable_irq(g_gpio_group_irqs[dev->port][group]);
    }
}

/****************************************************************************
 * Name: rk3576_gpio_irq_disable
 *
 * Description:
 *   Mask the interrupt on a claimed pin.  Masks INTMASK and, when no pin in
 *   the same 8-pin group remains enabled, disables the group's GIC line.
 *
 ****************************************************************************/

void rk3576_gpio_irq_disable(FAR struct gpio_dev_s *handle)
{
  FAR struct rk3576_gpio_dev_s *dev = (FAR struct rk3576_gpio_dev_s *)handle;
  irqstate_t flags;

  DEBUGASSERT(dev != NULL);

  if (!dev->irq_enabled)
    {
      return;
    }

  flags = spin_lock_irqsave(&g_gpio_lock);

  rk3576_gpio_irq_disable_internal(dev);

  spin_unlock_irqrestore(&g_gpio_lock, flags);
}

/****************************************************************************
 * NuttX upper-half (dev/gpioN) vtable wrappers.
 *
 * The /dev/gpioN upper half invokes these go_*() callbacks through the
 * gpio_pin_register() flow.  They adapt the NuttX gpio_operations_s
 * signatures to the RK3576 low-level API.
 ****************************************************************************/

static int rk3576_gpio_op_read(FAR struct gpio_dev_s *dev, FAR bool *value)
{
  DEBUGASSERT(value);

  *value = rk3576_gpio_read_bit(dev);
  return OK;
}

static int rk3576_gpio_op_write(FAR struct gpio_dev_s *dev, bool value)
{
  rk3576_gpio_write_bit(dev, value);
  return OK;
}

static int rk3576_gpio_op_attach(FAR struct gpio_dev_s *dev,
                                 pin_interrupt_t callback)
{
  /* pin_interrupt_t and rk3576_gpio_irq_callback_t share the identical
   * (dev, uint8_t pin) signature, so a plain assignment is valid.
   */

  return rk3576_gpio_irq_attach(dev, callback);
}

static int rk3576_gpio_op_enable(FAR struct gpio_dev_s *dev, bool enable)
{
  if (enable)
    {
      rk3576_gpio_irq_enable(dev);
    }
  else
    {
      rk3576_gpio_irq_disable(dev);
    }

  return OK;
}

static int rk3576_gpio_op_setpintype(FAR struct gpio_dev_s *dev,
                                     enum gpio_pintype_e pintype)
{
  switch (pintype)
    {
      case GPIO_INPUT_PIN:
      case GPIO_INPUT_PIN_PULLUP:
      case GPIO_INPUT_PIN_PULLDOWN:
        {
          enum rk3576_gpio_pull_e pull = RK3576_GPIO_FLOAT;

          if (pintype == GPIO_INPUT_PIN_PULLUP)
            {
              pull = RK3576_GPIO_PULLUP;
            }
          else if (pintype == GPIO_INPUT_PIN_PULLDOWN)
            {
              pull = RK3576_GPIO_PULLDOWN;
            }

          rk3576_gpio_set_pull(dev, pull);
          rk3576_gpio_set_mode(dev, RK3576_GPIO_INPUT);

          return OK;
        }

      case GPIO_OUTPUT_PIN:
        rk3576_gpio_set_mode(dev, RK3576_GPIO_OUTPUT);
        return OK;

      default:
        return -ENOTSUP;
    }
}
