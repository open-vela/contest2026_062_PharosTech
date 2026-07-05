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

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/ioexpander/gpio.h>

#include "chip.h"
#include "arm64_internal.h"
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

#define RK3576_IOMUX_FUNC_GPIO 0

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* GPIO bank base addresses.
 * Banks are NOT uniformly spaced: GPIO0 is at a different base range
 * than GPIO1-4. Use this lookup table instead of arithmetic.
 */

const uint32_t g_gpio_base[RK3576_GPIO_NPORTS] =
{
  RK3576_GPIO0_ADDR,
  RK3576_GPIO1_ADDR,
  RK3576_GPIO2_ADDR,
  RK3576_GPIO3_ADDR,
  RK3576_GPIO4_ADDR,
};

/* IOMUX register base offsets for each 8-pin group per bank.
 *
 * From Linux pinctrl-rockchip RK3576 data (rk3576_pin_banks):
 *   RK3576_PIN_BANK(num, label, offset_A, offset_B, offset_C, offset_D)
 *   = PIN_BANK_IOMUX_FLAGS_OFFSET_PULL_FLAGS(num, 32, label,
 *       IOMUX_WIDTH_4BIT * 4, offset_A/B/C/D, PULL_TYPE_IO_1V8_ONLY * 4)
 */

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
 * Private Data
 ****************************************************************************/

static spinlock_t g_gpio_lock = SP_UNLOCKED;

/* IOC (I/O Control) base address for pin configuration registers. */

static const uint32_t g_ioc_base = RK3576_IOC_BASE;

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
  putreg32(RK3576_WRITE_BIT(pin, 1),
           RK3576_GPIO_SWPORTA_DDR(port));
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
  putreg32(RK3576_WRITE_BIT(pin, 0),
           RK3576_GPIO_SWPORTA_DDR(port));
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
      reg += 0x1ff4;  /* GPIO0_IOC_GPIO0B_IOMUX_SEL_H */
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

  /* Determine the pull register offset for this bank/pin.
   * From Linux rk3576_calc_pull_reg_and_bit:
   */
  if (port == 0 && pin < RK_PIN_B4)
    {
      reg_offset = 0x0020;  /* GPIO0_AH */
    }
  else if (port == 0)
    {
      reg_offset = 0x2028 - 0x4;  /* GPIO0_BH, offset adjusted */
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
 * Name: rk3576_drive_set
 *
 * Description:
 *   Set drive strength for a GPIO pin via IOC registers.
 *
 *   RK3576 uses 4 bits/pin, 4 pins/reg.
 *   Drive level encoding: hw = RK3576_DRIVE_LEVEL_TO_HW(level).
 *
 * Input Parameters:
 *   ioc_base - IOC register base address
 *   port     - GPIO bank number (0-4)
 *   pin      - Pin number within bank (0-31)
 *   level    - Drive level (0-7, see RK3576_DRIVE_LEVEL_*)
 *
 ****************************************************************************/

static void rk3576_drive_set(uint32_t ioc_base, unsigned int port,
                              unsigned int pin, unsigned int level)
{
  uint32_t reg;
  unsigned int reg_offset;
  unsigned int bit;
  uint32_t data;
  uint32_t hw_val;

  /* Determine the drive register offset for this bank/pin.
   * From Linux rk3576_calc_drv_reg_and_bit:
   */
  if (port == 0 && pin < RK_PIN_B4)
    {
      reg_offset = 0x0010;  /* GPIO0_AL */
    }
  else if (port == 0)
    {
      reg_offset = 0x2014 - 0xc;  /* GPIO0_BH */
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
      reg_offset = 0x6080;
    }
  else if (port == 4 && pin < RK_PIN_D0)
    {
      reg_offset = 0xA090 - 0x10;
    }
  else /* port == 4, pin >= RK_PIN_D0 */
    {
      reg_offset = 0xB098 - 0x18;
    }

  /* 4 pins per register, 4 bits per pin */
  reg_offset += (pin / RK3576_DRV_PINS_PER_REG) * 4;
  bit = (pin % RK3576_DRV_PINS_PER_REG) * RK3576_DRV_BITS_PER_PIN;

  /* Encode the drive level to hardware format */
  hw_val = RK3576_DRIVE_LEVEL_TO_HW(level);

  reg = ioc_base + reg_offset;

  /* Hiword-mask write */
  data = RK3576_WRITE_MASK(bit + 3, bit, hw_val);

  putreg32(data, reg);
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

  /* Determine the schmitt register offset for this bank/pin.
   * From Linux rk3576_calc_schmitt_reg_and_bit:
   */
  if (port == 0 && pin < RK_PIN_B4)
    {
      reg_offset = 0x0030;  /* GPIO0_AL */
    }
  else if (port == 0)
    {
      reg_offset = 0x2040 - 0x4;  /* GPIO0_BH */
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
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_config_gpio
 *
 * Description:
 *   Configure a GPIO pin based on bit-encoded description of the pin.
 *
 *   The pinset encoding is defined in rk3576_gpio.h and includes:
 *   - Port and pin number
 *   - Mode (input/output/alternate/analog)
 *   - Pull-up/pull-down
 *   - Speed (for output/AF pins)
 *   - Output type (push-pull/open-drain)
 *   - Alternate function number
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
  irqstate_t flags;

  /* Extract port and pin from the configuration */

  port = (pinset & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT;
  pin  = (pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;
  mode = pinset & GPIO_MODE_MASK;

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

  flags = spin_lock_irqsave(&g_gpio_lock);

  switch (mode)
    {
      case GPIO_INPUT:
        {
          /* Set IOMUX to GPIO function (AF=0) before configuring direction.
           * Bootloader may have left IOMUX in a non-GPIO state.
           */

          rk3576_iomux_set(g_ioc_base, port, pin, RK3576_IOMUX_FUNC_GPIO);

          /* Configure as input */

          rk3576_gpio_dirin(port, pin);

          /* Enable schmitt trigger if requested (default for clean input) */

          rk3576_schmitt_set(g_ioc_base, port, pin,
                             (pinset & GPIO_SCHMITT) != 0);

          /* Configure pull-up/pull-down via IOC if specified */

          if ((pinset & GPIO_PUPD_MASK) == GPIO_PULLUP)
            {
              rk3576_pull_set(g_ioc_base, port, pin,
                              RK3576_PULL_UP);
            }
          else if ((pinset & GPIO_PUPD_MASK) == GPIO_PULLDOWN)
            {
              rk3576_pull_set(g_ioc_base, port, pin,
                              RK3576_PULL_DOWN);
            }
          else
            {
              rk3576_pull_set(g_ioc_base, port, pin,
                              RK3576_PULL_DISABLE);
            }
        }
        break;

      case GPIO_OUTPUT:
        {
          /* Set IOMUX to GPIO function (AF=0) before configuring direction.
           * Bootloader may have left IOMUX in a non-GPIO state.
           */

          rk3576_iomux_set(g_ioc_base, port, pin, RK3576_IOMUX_FUNC_GPIO);

          /* Set the initial output value before changing direction */

          rk3576_gpio_write(pinset, (pinset & GPIO_OUTPUT_SET) != 0);

          /* Configure as output */

          rk3576_gpio_dirout(port, pin);

          /* Disable schmitt trigger for output pins */

          rk3576_schmitt_set(g_ioc_base, port, pin, false);

          /* Configure drive strength (default level 3 = 12mA) */

          rk3576_drive_set(g_ioc_base, port, pin,
                           RK3576_DRIVE_LEVEL_DEFAULT);

          /* Configure pull-up/pull-down if specified */

          if ((pinset & GPIO_PUPD_MASK) == GPIO_PULLUP)
            {
              rk3576_pull_set(g_ioc_base, port, pin,
                              RK3576_PULL_UP);
            }
          else if ((pinset & GPIO_PUPD_MASK) == GPIO_PULLDOWN)
            {
              rk3576_pull_set(g_ioc_base, port, pin,
                              RK3576_PULL_DOWN);
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

          rk3576_iomux_set(g_ioc_base, port, pin, af);

          /* Enable schmitt trigger for AF input (e.g. UART RX, I2C) */

          rk3576_schmitt_set(g_ioc_base, port, pin,
                             (pinset & GPIO_SCHMITT) != 0);

          /* Configure drive strength for the alternate function */

          rk3576_drive_set(g_ioc_base, port, pin,
                           RK3576_DRIVE_LEVEL_DEFAULT);
        }
        break;

      case GPIO_ANALOG:
        {
          /* Analog mode: configure pin as input (high-impedance).
           * The IOMUX should be set to the analog function.
           */

          rk3576_gpio_dirin(port, pin);

          rk3576_iomux_set(g_ioc_base, port, pin, RK3576_IOMUX_FUNC_GPIO);

          /* Disable pull-up/pull-down for analog */

          rk3576_pull_set(g_ioc_base, port, pin,
                          RK3576_PULL_DISABLE);

          /* Disable schmitt trigger for analog input */

          rk3576_schmitt_set(g_ioc_base, port, pin, false);
        }
        break;

      default:
        spin_unlock_irqrestore(&g_gpio_lock, flags);
        return -EINVAL;
    }

  /* Handle EXTI interrupt configuration */

  if ((pinset & GPIO_EXTI) != 0)
    {
      /* Enable schmitt trigger to prevent spurious interrupts from noise */

      rk3576_schmitt_set(g_ioc_base, port, pin, true);

      /* Set interrupt type (level or edge).
       * All GPIO registers use hiword-mask writes.
       */

      putreg32(RK3576_WRITE_BIT(pin,
               (pinset & GPIO_INTTYPE_MASK) == GPIO_INT_EDGE ? 1 : 0),
               RK3576_GPIO_INTTYPE_LEVEL(port));

      /* Set interrupt polarity */

      putreg32(RK3576_WRITE_BIT(pin,
               (pinset & GPIO_INTPOL_MASK) == GPIO_INT_HIGH_RISING ? 1 : 0),
               RK3576_GPIO_INT_POLARITY(port));

      /* Clear any pending interrupt */

      putreg32(RK3576_WRITE_BIT(pin, 1),
               RK3576_GPIO_PORTA_EOI(port));

      /* Unmask (clear mask bit = enable) and enable the interrupt */

      putreg32(RK3576_WRITE_BIT(pin, 0),
               RK3576_GPIO_INTMASK(port));

      putreg32(RK3576_WRITE_BIT(pin, 1),
               RK3576_GPIO_INTEN(port));

      /* GIC interrupt handlers are installed by rk3576_gpio_init().
       * Each bank has 4 interrupt lines (one per 8-pin group).
       */
    }

  spin_unlock_irqrestore(&g_gpio_lock, flags);
  return OK;
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
  uint32_t addr;
  uint32_t data;

  port = (pinset & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT;
  pin  = (pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;

  if (port >= RK3576_GPIO_NPORTS)
    {
      return;
    }

  addr = RK3576_GPIO_SWPORTA_DR(port);

  /* Hiword-mask write: upper 16 bits = write mask, lower 16 = value */

  data = RK3576_WRITE_BIT(pin, value);

  putreg32(data, addr);

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
  pin  = (pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;

  if (port >= RK3576_GPIO_NPORTS)
    {
      return false;
    }

  return (getreg32(RK3576_GPIO_EXT_PORTA(port)) & RK3576_GPIO_PIN_BIT(pin)) != 0;
}

#ifdef CONFIG_DEV_GPIO

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_gpio_dev_s
{
  struct gpio_dev_s gpio;
  gpio_pinset_t     pinset;
};

struct rk3576_gpint_dev_s
{
  struct rk3576_gpio_dev_s dev;
  pin_interrupt_t callback;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_gpio_read_cb(FAR struct gpio_dev_s *dev,
                               FAR bool *value);
static int rk3576_gpio_write_cb(FAR struct gpio_dev_s *dev, bool value);
static int rk3576_gpio_setpintype(FAR struct gpio_dev_s *dev,
                                  enum gpio_pintype_e pintype);
static int rk3576_gpio_attach(FAR struct gpio_dev_s *dev,
                              pin_interrupt_t callback);
static int rk3576_gpio_enable(FAR struct gpio_dev_s *dev, bool enable);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_gpio_global_minor = 0;  /* /dev/gpioN minor, globally unique */
static int g_gpio_int_count    = 0;  /* index into g_gpio_interrupts[] */

static const struct gpio_operations_s g_gpin_ops =
{
  .go_read       = rk3576_gpio_read_cb,
  .go_write      = NULL,
  .go_attach     = NULL,
  .go_enable     = NULL,
  .go_setpintype = rk3576_gpio_setpintype,
};

static const struct gpio_operations_s g_gpout_ops =
{
  .go_read       = rk3576_gpio_read_cb,
  .go_write      = rk3576_gpio_write_cb,
  .go_attach     = NULL,
  .go_enable     = NULL,
  .go_setpintype = rk3576_gpio_setpintype,
};

static const struct gpio_operations_s g_gpint_ops =
{
  .go_read       = rk3576_gpio_read_cb,
  .go_write      = NULL,
  .go_attach     = rk3576_gpio_attach,
  .go_enable     = rk3576_gpio_enable,
  .go_setpintype = rk3576_gpio_setpintype,
};

static struct rk3576_gpint_dev_s
  *g_gpio_interrupts[RK3576_GPIO_NPORTS * RK3576_GPIO_NPINS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int rk3576_gpio_read_cb(FAR struct gpio_dev_s *dev,
                               FAR bool *value)
{
  FAR struct rk3576_gpio_dev_s *rdev =
    (FAR struct rk3576_gpio_dev_s *)dev;

  DEBUGASSERT(rdev != NULL && value != NULL);

  *value = rk3576_gpio_read(rdev->pinset);
  return OK;
}

static int rk3576_gpio_write_cb(FAR struct gpio_dev_s *dev, bool value)
{
  FAR struct rk3576_gpio_dev_s *rdev =
    (FAR struct rk3576_gpio_dev_s *)dev;

  DEBUGASSERT(rdev != NULL);

  rk3576_gpio_write(rdev->pinset, value);
  return OK;
}

static int rk3576_gpio_setpintype(FAR struct gpio_dev_s *dev,
                                  enum gpio_pintype_e pintype)
{
  FAR struct rk3576_gpio_dev_s *rdev =
    (FAR struct rk3576_gpio_dev_s *)dev;
  gpio_pinset_t pinset;

  DEBUGASSERT(rdev != NULL);

  pinset = rdev->pinset & (GPIO_PORT_MASK | GPIO_PIN_MASK);

  switch (pintype)
    {
      case GPIO_INPUT_PIN:
        pinset |= GPIO_INPUT | GPIO_FLOAT | GPIO_SCHMITT;
        break;

      case GPIO_INPUT_PIN_PULLUP:
        pinset |= GPIO_INPUT | GPIO_PULLUP | GPIO_SCHMITT;
        break;

      case GPIO_INPUT_PIN_PULLDOWN:
        pinset |= GPIO_INPUT | GPIO_PULLDOWN | GPIO_SCHMITT;
        break;

      case GPIO_OUTPUT_PIN:
        pinset |= GPIO_OUTPUT;
        break;

      case GPIO_INTERRUPT_PIN:
      case GPIO_INTERRUPT_HIGH_PIN:
      case GPIO_INTERRUPT_LOW_PIN:
      case GPIO_INTERRUPT_RISING_PIN:
      case GPIO_INTERRUPT_FALLING_PIN:
      case GPIO_INTERRUPT_BOTH_PIN:
        pinset |= GPIO_INPUT | GPIO_FLOAT | GPIO_SCHMITT | GPIO_EXTI;
        break;

      case GPIO_INTERRUPT_PIN_WAKEUP:
      case GPIO_INTERRUPT_HIGH_PIN_WAKEUP:
      case GPIO_INTERRUPT_LOW_PIN_WAKEUP:
      case GPIO_INTERRUPT_RISING_PIN_WAKEUP:
      case GPIO_INTERRUPT_FALLING_PIN_WAKEUP:
      case GPIO_INTERRUPT_BOTH_PIN_WAKEUP:
        pinset |= GPIO_INPUT | GPIO_FLOAT | GPIO_SCHMITT | GPIO_EXTI;
        break;

      default:
        gpioerr("ERROR: Unsupported pintype: %u\n",
                (unsigned int)pintype);
        return -EINVAL;
    }

  rk3576_config_gpio(pinset);

  rdev->pinset = pinset;
  dev->gp_pintype = (uint8_t)pintype;

  return OK;
}

static int rk3576_gpio_attach(FAR struct gpio_dev_s *dev,
                              pin_interrupt_t callback)
{
  FAR struct rk3576_gpint_dev_s *rdev =
    (FAR struct rk3576_gpint_dev_s *)dev;

  DEBUGASSERT(rdev != NULL);

  gpioinfo("Attach callback %p\n", callback);
  rdev->callback = callback;
  return OK;
}

static int rk3576_gpio_enable(FAR struct gpio_dev_s *dev, bool enable)
{
  FAR struct rk3576_gpint_dev_s *rdev =
    (FAR struct rk3576_gpint_dev_s *)dev;
  unsigned int port;
  unsigned int pin;

  DEBUGASSERT(rdev != NULL);

  port = (rdev->dev.pinset & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT;
  pin  = (rdev->dev.pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;

  /* All GPIO registers use hiword-mask writes.
   * INTMASK: 0 = unmasked (enabled), 1 = masked (disabled).
   */

  if (enable)
    {
      putreg32(RK3576_WRITE_BIT(pin, 0),
               RK3576_GPIO_INTMASK(port));

      putreg32(RK3576_WRITE_BIT(pin, 1),
               RK3576_GPIO_INTEN(port));
    }
  else
    {
      putreg32(RK3576_WRITE_BIT(pin, 1),
               RK3576_GPIO_INTMASK(port));
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_gpio_isr
 *
 *   Per-group GIC ISR.  Each bank has 4 interrupt lines — one per 8-pin
 *   group (A/B/C/D).  Reads GPIO INTSTATUS and dispatches to callbacks.
 ****************************************************************************/

static int rk3576_gpio_isr(int irq, void *context, void *arg)
{
  unsigned int port = (unsigned int)(uintptr_t)arg;
  uint32_t status;
  int i;

  if (port >= RK3576_GPIO_NPORTS)
    {
      return -EINVAL;
    }

  status = getreg32(RK3576_GPIO_INT_STATUS(port));

  while (status != 0)
    {
      int pin = __builtin_ctz(status);

      for (i = 0; i < g_gpio_int_count; i++)
        {
          FAR struct rk3576_gpint_dev_s *idev = g_gpio_interrupts[i];
          unsigned int iport;
          unsigned int ipin;

          if (idev == NULL)
            {
              continue;
            }

          iport = (idev->dev.pinset & GPIO_PORT_MASK) >> GPIO_PORT_SHIFT;
          ipin  = (idev->dev.pinset & GPIO_PIN_MASK) >> GPIO_PIN_SHIFT;

          if (iport == port && ipin == pin && idev->callback != NULL)
            {
              idev->callback(&idev->dev.gpio, (uint8_t)pin);
              break;
            }
        }

      status &= ~RK3576_GPIO_PIN_BIT(pin);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_gpio_register_input(gpio_pinset_t pinset)
{
  FAR struct rk3576_gpio_dev_s *dev;
  int ret;
  int i;

  dev = kmm_zalloc(sizeof(*dev));
  if (dev == NULL)
    {
      return -ENOMEM;
    }

  dev->pinset          = pinset;
  dev->gpio.gp_pintype = GPIO_INPUT_PIN;
  dev->gpio.gp_ops     = &g_gpin_ops;

  i = g_gpio_global_minor;
  ret = gpio_pin_register(&dev->gpio, i);
  if (ret < 0)
    {
      kmm_free(dev);
      return ret;
    }

  rk3576_config_gpio(pinset);

  g_gpio_global_minor++;
  return OK;
}

int rk3576_gpio_register_output(gpio_pinset_t pinset)
{
  FAR struct rk3576_gpio_dev_s *dev;
  int ret;
  int i;

  dev = kmm_zalloc(sizeof(*dev));
  if (dev == NULL)
    {
      return -ENOMEM;
    }

  dev->pinset          = pinset;
  dev->gpio.gp_pintype = GPIO_OUTPUT_PIN;
  dev->gpio.gp_ops     = &g_gpout_ops;

  i = g_gpio_global_minor;
  ret = gpio_pin_register(&dev->gpio, i);
  if (ret < 0)
    {
      kmm_free(dev);
      return ret;
    }

  rk3576_gpio_write(pinset, false);
  rk3576_config_gpio(pinset);

  g_gpio_global_minor++;
  return OK;
}

int rk3576_gpio_register_interrupt(gpio_pinset_t pinset)
{
  FAR struct rk3576_gpint_dev_s *dev;
  int ret;
  int i;

  dev = kmm_zalloc(sizeof(*dev));
  if (dev == NULL)
    {
      return -ENOMEM;
    }

  dev->dev.pinset          = pinset;
  dev->dev.gpio.gp_pintype = GPIO_INTERRUPT_PIN;
  dev->dev.gpio.gp_ops     = &g_gpint_ops;
  dev->callback            = NULL;

  i = g_gpio_global_minor;
  ret = gpio_pin_register(&dev->dev.gpio, i);
  if (ret < 0)
    {
      kmm_free(dev);
      return ret;
    }

  rk3576_config_gpio(pinset);

  if (g_gpio_int_count >= RK3576_GPIO_NPORTS * RK3576_GPIO_NPINS)
    {
      kmm_free(dev);
      return -ENOMEM;
    }

  g_gpio_interrupts[g_gpio_int_count] = dev;
  g_gpio_int_count++;
  g_gpio_global_minor++;

  return OK;
}

int rk3576_gpio_init(void)
{
  /* Per-bank GIC interrupt handlers (4 groups per bank, 5 banks).
   *
   * Each bank's 32 pins are split into 4 groups (A=0-7, B=8-15,
   * C=16-23, D=24-31), each with its own GIC interrupt line.
   */

  static const unsigned int g_gpio_irqs[RK3576_GPIO_NPORTS][4] =
  {
    { RK3576_IRQ_GPIO0_0, RK3576_IRQ_GPIO0_1,
      RK3576_IRQ_GPIO0_2, RK3576_IRQ_GPIO0_3 },
    { RK3576_IRQ_GPIO1_0, RK3576_IRQ_GPIO1_1,
      RK3576_IRQ_GPIO1_2, RK3576_IRQ_GPIO1_3 },
    { RK3576_IRQ_GPIO2_0, RK3576_IRQ_GPIO2_1,
      RK3576_IRQ_GPIO2_2, RK3576_IRQ_GPIO2_3 },
    { RK3576_IRQ_GPIO3_0, RK3576_IRQ_GPIO3_1,
      RK3576_IRQ_GPIO3_2, RK3576_IRQ_GPIO3_3 },
    { RK3576_IRQ_GPIO4_0, RK3576_IRQ_GPIO4_1,
      RK3576_IRQ_GPIO4_2, RK3576_IRQ_GPIO4_3 },
  };

  unsigned int port;
  unsigned int group;

  for (port = 0; port < RK3576_GPIO_NPORTS; port++)
    {
      for (group = 0; group < 4; group++)
        {
          int irq = g_gpio_irqs[port][group];
          int ret;

          ret = irq_attach(irq, rk3576_gpio_isr,
                           (void *)(uintptr_t)port);
          if (ret < 0)
            {
              gpioerr("ERROR: Failed to attach IRQ %u (GPIO%u_%u): %d\n",
                      irq, port, group, ret);
              return ret;
            }

          up_enable_irq(irq);
        }
    }

  return OK;
}

#endif /* CONFIG_DEV_GPIO */
