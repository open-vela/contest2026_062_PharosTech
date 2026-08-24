/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_gpio.h
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

#ifndef __VENDOR_ROCKCHIP_RK3576_GPIO_H
#define __VENDOR_ROCKCHIP_RK3576_GPIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/* Forward declaration of the NuttX GPIO character-device base type.  The
 * lower half's stateful handle is a struct gpio_dev_s with private data
 * trailing it (see below).  We intentionally do NOT include
 * <nuttx/ioexpander/gpio.h> here to keep the hardware-control (lower half)
 * and the /dev/gpioN character-device (upper half) layers decoupled: the
 * upper half lives in the board layer and does the pin registration.
 */

struct gpio_dev_s;

/* Interrupt callback type for driver-level GPIO interrupts.  Mirrors the
 * NuttX pin_interrupt_t signature so the same callback shape serves both
 * driver-level consumers and the /dev/gpioN upper half.
 */

typedef CODE int (*rk3576_gpio_irq_callback_t)(FAR struct gpio_dev_s *dev,
                                               uint8_t pin);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO pinset encoding (32-bit word):
 *
 *  Bit [4:0]   - Pin number within port (0-31)
 *  Bit [7:5]   - Port number (0-4)
 *
 *  NOTE: Historically the pinset also packed function fields (mode, pull,
 *  drive, AF, interrupt config) in bits [24:8].  Those functional bit
 *  macros below are DEPRECATED: a pinset now expresses only the pin
 *  identity (port + pin).  Pin function must be set via the
 *  rk3576_gpio_set_*() setters, which take their own enums.
 *
 * Pin naming convention:
 *   RK_GPIO0_A0 = GPIO_PORT0 | GPIO_PIN_A0
 *   RK_GPIO4_D7 = GPIO_PORT4 | GPIO_PIN_D7
 */

/* Pin encoding
 * ***************************************************************/

#define GPIO_PIN_SHIFT (0) /* Bits 0-4: GPIO pin number */
#define GPIO_PIN_MASK  (0x1f << GPIO_PIN_SHIFT)
/* Group A (pins 0-7) */
#define GPIO_PIN_A0 (0 << GPIO_PIN_SHIFT)
#define GPIO_PIN_A1 (1 << GPIO_PIN_SHIFT)
#define GPIO_PIN_A2 (2 << GPIO_PIN_SHIFT)
#define GPIO_PIN_A3 (3 << GPIO_PIN_SHIFT)
#define GPIO_PIN_A4 (4 << GPIO_PIN_SHIFT)
#define GPIO_PIN_A5 (5 << GPIO_PIN_SHIFT)
#define GPIO_PIN_A6 (6 << GPIO_PIN_SHIFT)
#define GPIO_PIN_A7 (7 << GPIO_PIN_SHIFT)
/* Group B (pins 8-15) */
#define GPIO_PIN_B0 (8 << GPIO_PIN_SHIFT)
#define GPIO_PIN_B1 (9 << GPIO_PIN_SHIFT)
#define GPIO_PIN_B2 (10 << GPIO_PIN_SHIFT)
#define GPIO_PIN_B3 (11 << GPIO_PIN_SHIFT)
#define GPIO_PIN_B4 (12 << GPIO_PIN_SHIFT)
#define GPIO_PIN_B5 (13 << GPIO_PIN_SHIFT)
#define GPIO_PIN_B6 (14 << GPIO_PIN_SHIFT)
#define GPIO_PIN_B7 (15 << GPIO_PIN_SHIFT)
/* Group C (pins 16-23) */
#define GPIO_PIN_C0 (16 << GPIO_PIN_SHIFT)
#define GPIO_PIN_C1 (17 << GPIO_PIN_SHIFT)
#define GPIO_PIN_C2 (18 << GPIO_PIN_SHIFT)
#define GPIO_PIN_C3 (19 << GPIO_PIN_SHIFT)
#define GPIO_PIN_C4 (20 << GPIO_PIN_SHIFT)
#define GPIO_PIN_C5 (21 << GPIO_PIN_SHIFT)
#define GPIO_PIN_C6 (22 << GPIO_PIN_SHIFT)
#define GPIO_PIN_C7 (23 << GPIO_PIN_SHIFT)
/* Group D (pins 24-31) */
#define GPIO_PIN_D0 (24 << GPIO_PIN_SHIFT)
#define GPIO_PIN_D1 (25 << GPIO_PIN_SHIFT)
#define GPIO_PIN_D2 (26 << GPIO_PIN_SHIFT)
#define GPIO_PIN_D3 (27 << GPIO_PIN_SHIFT)
#define GPIO_PIN_D4 (28 << GPIO_PIN_SHIFT)
#define GPIO_PIN_D5 (29 << GPIO_PIN_SHIFT)
#define GPIO_PIN_D6 (30 << GPIO_PIN_SHIFT)
#define GPIO_PIN_D7 (31 << GPIO_PIN_SHIFT)

/* Port encoding *************************************************************/

#define GPIO_PORT_SHIFT (5) /* Bits 5-7: GPIO port number */
#define GPIO_PORT_MASK  (0x7 << GPIO_PORT_SHIFT)
#define GPIO_PORT0      (0 << GPIO_PORT_SHIFT) /* GPIO0 */
#define GPIO_PORT1      (1 << GPIO_PORT_SHIFT) /* GPIO1 */
#define GPIO_PORT2      (2 << GPIO_PORT_SHIFT) /* GPIO2 */
#define GPIO_PORT3      (3 << GPIO_PORT_SHIFT) /* GPIO3 */
#define GPIO_PORT4      (4 << GPIO_PORT_SHIFT) /* GPIO4 */

/* Mode encoding *************************************************************/

#define GPIO_MODE_SHIFT (8) /* Bits 8-9: Pin mode */
#define GPIO_MODE_MASK  (0x3 << GPIO_MODE_SHIFT)
#define GPIO_INPUT      (0 << GPIO_MODE_SHIFT) /* Input mode */
#define GPIO_OUTPUT     (1 << GPIO_MODE_SHIFT) /* Output mode */
#define GPIO_ALT        (2 << GPIO_MODE_SHIFT) /* Alternate function */

/* Pull-up/Pull-down
 * **********************************************************/

#define GPIO_PUPD_SHIFT (10) /* Bits 10-11: Pull-up/pull-down */
#define GPIO_PUPD_MASK  (0x3 << GPIO_PUPD_SHIFT)
#define GPIO_FLOAT      (0 << GPIO_PUPD_SHIFT) /* No pull */
#define GPIO_PULLUP     (1 << GPIO_PUPD_SHIFT) /* Pull-up */
#define GPIO_PULLDOWN   (2 << GPIO_PUPD_SHIFT) /* Pull-down */

/* Drive strength encoding — 3-bit field (for output and AF pins)
 *
 * Bit layout: DRV_STRENGTH[2:0] = bits [14:12]
 *
 *
 *   GPIO_DRV_STRENGTH_DEFAULT (0) — hardware reset value (50Ω for 4-level,
 *                                    40Ω for 6-level GPIOs)
 *   GPIO_DRV_STRENGTH_100OHM  (1) — 100 ohms — both 4-level and 6-level GPIOs
 *   GPIO_DRV_STRENGTH_66OHM   (2) —  66 ohms — 6-level GPIOs only (error on
 * 4-level) GPIO_DRV_STRENGTH_50OHM   (3) —  50 ohms — both 4-level and 6-level
 * GPIOs GPIO_DRV_STRENGTH_40OHM   (4) —  40 ohms — 6-level GPIOs only (error
 * on 4-level) GPIO_DRV_STRENGTH_33OHM   (5) —  33 ohms — both 4-level and
 * 6-level GPIOs GPIO_DRV_STRENGTH_25OHM   (6) —  25 ohms — both 4-level and
 * 6-level GPIOs
 *
 */

#define GPIO_DRV_STRENGTH_SHIFT (12) /* Bits 12-14: Drive strength */
#define GPIO_DRV_STRENGTH_MASK  (0x7 << GPIO_DRV_STRENGTH_SHIFT)
#define GPIO_DRV_STRENGTH_DEFAULT \
  (0 << GPIO_DRV_STRENGTH_SHIFT) /* hw reset: 50Ω(4-lv) / 40Ω(6-lv) */
#define GPIO_DRV_STRENGTH_100OHM             \
  (1 << GPIO_DRV_STRENGTH_SHIFT) /* 100 ohms \
                                  */
#define GPIO_DRV_STRENGTH_66OHM \
  (2 << GPIO_DRV_STRENGTH_SHIFT) /*  66 ohms — 6-level only */
#define GPIO_DRV_STRENGTH_50OHM (3 << GPIO_DRV_STRENGTH_SHIFT) /*  50 ohms */
#define GPIO_DRV_STRENGTH_40OHM \
  (4 << GPIO_DRV_STRENGTH_SHIFT) /*  40 ohms — 6-level only */
#define GPIO_DRV_STRENGTH_33OHM (5 << GPIO_DRV_STRENGTH_SHIFT) /*  33 ohms */
#define GPIO_DRV_STRENGTH_25OHM (6 << GPIO_DRV_STRENGTH_SHIFT) /*  25 ohms */

/* Alternate function encoding
 * ************************************************/

#define GPIO_AF_SHIFT (15) /* Bits 15-18: AF number (0-15) */
#define GPIO_AF_MASK  (0xf << GPIO_AF_SHIFT)
#define GPIO_AF0      (0 << GPIO_AF_SHIFT)
#define GPIO_AF1      (1 << GPIO_AF_SHIFT)
#define GPIO_AF2      (2 << GPIO_AF_SHIFT)
#define GPIO_AF3      (3 << GPIO_AF_SHIFT)
#define GPIO_AF4      (4 << GPIO_AF_SHIFT)
#define GPIO_AF5      (5 << GPIO_AF_SHIFT)
#define GPIO_AF6      (6 << GPIO_AF_SHIFT)
#define GPIO_AF7      (7 << GPIO_AF_SHIFT)
#define GPIO_AF8      (8 << GPIO_AF_SHIFT)
#define GPIO_AF9      (9 << GPIO_AF_SHIFT)
#define GPIO_AF10     (10 << GPIO_AF_SHIFT)
#define GPIO_AF11     (11 << GPIO_AF_SHIFT)
#define GPIO_AF12     (12 << GPIO_AF_SHIFT)
#define GPIO_AF13     (13 << GPIO_AF_SHIFT)
#define GPIO_AF14     (14 << GPIO_AF_SHIFT)
#define GPIO_AF15     (15 << GPIO_AF_SHIFT)

/* Initial output value
 * *******************************************************/

#define GPIO_OUTPUT_SET (1 << 19) /* Bit 19: Initial output high */

/* Interrupt configuration
 * ****************************************************/

#define GPIO_EXTI            (1 << 20) /* Bit 20: Enable EXTI interrupt */

#define GPIO_INTTYPE_SHIFT   (21) /* Bit 21: Interrupt type */
#define GPIO_INTTYPE_MASK    (0x1 << GPIO_INTTYPE_SHIFT)
#define GPIO_INT_LEVEL       (0 << GPIO_INTTYPE_SHIFT) /* Level triggered */
#define GPIO_INT_EDGE        (1 << GPIO_INTTYPE_SHIFT) /* Edge triggered */

#define GPIO_INTPOL_SHIFT    (22) /* Bit 22: Interrupt polarity */
#define GPIO_INTPOL_MASK     (0x1 << GPIO_INTPOL_SHIFT)
#define GPIO_INT_LOW_FALLING (0 << GPIO_INTPOL_SHIFT)
#define GPIO_INT_HIGH_RISING (1 << GPIO_INTPOL_SHIFT)

/* Schmitt trigger (kernel-internal use, automatically set by config_gpio
 * based on pin mode; user programs use GPIOC_SETPINTYPE ioctl instead)
 */

#define GPIO_SCHMITT      (1 << 23) /* Bit 23: Enable schmitt trigger */

#define GPIO_INT_BOTHEDGE (1 << 24) /* Bit 24: Both-edge trigger */

/* Convenience macros for common pin configurations
 * ***************************/

#define GPIO_INPUT_PULLUP    (GPIO_INPUT | GPIO_PULLUP)
#define GPIO_INPUT_PULLDOWN  (GPIO_INPUT | GPIO_PULLDOWN)
#define GPIO_OUTPUT_PUSHPULL (GPIO_OUTPUT)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* GPIO pin identity encoded as a 32-bit value: bits [4:0] = pin, [7:5] =
 * port.  (The bit-encoded function fields historically packed in a pinset
 * are deprecated; use the rk3576_gpio_set_*() setters with their own enums.)
 */

typedef uint32_t gpio_pinset_t;

/* GPIO direction.  Pass to rk3576_gpio_set_mode(). */

enum rk3576_gpio_mode_e
{
  RK3576_GPIO_INPUT = 0,  /* Input */
  RK3576_GPIO_OUTPUT = 1, /* Output */
};

/* Pull resistor.  Pass to rk3576_gpio_set_pull(). */

enum rk3576_gpio_pull_e
{
  RK3576_GPIO_FLOAT = 0,    /* No pull */
  RK3576_GPIO_PULLUP = 1,   /* Pull-up */
  RK3576_GPIO_PULLDOWN = 2, /* Pull-down */
};

/* Drive strength.  Pass to rk3576_gpio_set_drive(). */

enum rk3576_gpio_drive_e
{
  RK3576_GPIO_DRIVE_DEFAULT = 0, /* Hardware reset value */
  RK3576_GPIO_DRIVE_100OHM = 1,  /* 100 ohms */
  RK3576_GPIO_DRIVE_66OHM = 2,   /*  66 ohms (6-level only) */
  RK3576_GPIO_DRIVE_50OHM = 3,   /*  50 ohms */
  RK3576_GPIO_DRIVE_40OHM = 4,   /*  40 ohms (6-level only) */
  RK3576_GPIO_DRIVE_33OHM = 5,   /*  33 ohms */
  RK3576_GPIO_DRIVE_25OHM = 6,   /*  25 ohms */
};

/* Interrupt trigger type.  Pass to rk3576_gpio_set_int_type(). */

enum rk3576_gpio_int_type_e
{
  RK3576_GPIO_INT_LEVEL = 0, /* Level triggered */
  RK3576_GPIO_INT_EDGE = 1,  /* Edge triggered */
};

/* Interrupt polarity / trigger edge.  Pass to rk3576_gpio_set_int_pol().
 *
 * For level-triggered pins: LOW_FALLING = active-low, HIGH_RISING =
 * active-high.  For edge-triggered pins: LOW_FALLING = falling edge,
 * HIGH_RISING = rising edge, BOTH_EDGE = both edges (edge-trigger only).
 */

enum rk3576_gpio_int_pol_e
{
  RK3576_GPIO_INT_LOW_FALLING = 0, /* Low level / falling edge */
  RK3576_GPIO_INT_HIGH_RISING = 1, /* High level / rising edge */
  RK3576_GPIO_INT_BOTH_EDGE = 2,   /* Both edges (edge-trigger only) */
};

/* Maximum valid IOMUX alternate-function number.  AF is a 4-bit field,
 * so valid values are [0, RK3576_AF_MAX] = [0, 15].  Pass to
 * rk3576_gpio_set_af().
 */

#define RK3576_AF_MAX 15

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_gpio_get
 *
 * Description:
 *   Claim (occupy) a GPIO pin and return a stateful handle to it.
 *
 *   The handle is a NuttX `struct gpio_dev_s` with the RK3576 private state
 *   trailing it.  It is owned by the caller (typically board-level code),
 *   which may then either pass it to another kernel driver for direct
 *   read/write/interrupt use, or register it as a /dev/gpioN character
 *   device via gpio_pin_register().
 *
 *   Each GPIO may be claimed at most once: a second get() on the same
 *   (port, pin) returns -EBUSY.  This prevents board code from handing the
 *   same pin to two consumers.
 *
 *   get() does NOT configure the pin's function; it only claims the pin and
 *   returns the handle.  Use rk3576_gpio_set_mode()/set_pull()/set_drive()/
 *   set_af()/set_schmitt() to configure it afterward.
 *
 * Input Parameters:
 *   pinset - Bit-encoded pin identity (port + pin).  Any function bits are
 *            ignored.
 *   handle - Location to return the claimed handle.
 *
 * Returned Value:
 *   OK on success; a negated errno on failure (-EINVAL bad pinset,
 *   -EBUSY already claimed, -ENOMEM out of memory).
 *
 ****************************************************************************/

int rk3576_gpio_get(gpio_pinset_t pinset, FAR struct gpio_dev_s **handle);

/****************************************************************************
 * Name: rk3576_gpio_put
 *
 * Description:
 *   Release a GPIO pin previously claimed with rk3576_gpio_get().
 *
 *   Any pending interrupt is masked, the interrupt callback is cleared, and
 *   the single-table slot is freed so the pin may be claimed again.
 *
 * Input Parameters:
 *   handle - The handle returned by rk3576_gpio_get().
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_gpio_put(FAR struct gpio_dev_s *handle);

/****************************************************************************
 * Name: rk3576_gpio_read_bit / rk3576_gpio_write_bit
 *
 * Description:
 *   Read/write the level (a single bit) of a claimed GPIO pin through its
 *   handle.  These are the stateful counterparts of the deprecated
 *   pinset-based functions below; they carry the pin identity inside the
 *   handle instead of re-decoding a pinset each call.
 *
 ****************************************************************************/

bool rk3576_gpio_read_bit(FAR struct gpio_dev_s *handle);
void rk3576_gpio_write_bit(FAR struct gpio_dev_s *handle, bool value);

/****************************************************************************
 * Name: rk3576_gpio_irq_attach
 *
 * Description:
 *   Attach (or detach) a driver-level callback for GPIO interrupts on a
 *   claimed pin.  Thread-safe: the callback slot is guarded by the GPIO
 *   spinlock, so drivers must NOT write the gpio_dev_s fields directly.
 *
 *   Attaching a non-NULL callback also lazily attaches (irq_attach) and,
 *   on the first pin in the bank to attach, enables the bank's GIC
 *   interrupt line (GPIOx_0; see the driver core).  The pin's interrupt is
 *   not unmasked here — call rk3576_gpio_irq_enable() afterward.
 *
 *   Passing NULL detaches: the callback is cleared and, if this pin's
 *   interrupt was currently enabled, it is torn down as well (INTMASK set,
 *   irq_enabled cleared, and the bank's GIC-line reference count dropped,
 *   disabling the GIC line when the last pin in the bank is released).
 *   After a detach the pin produces no new interrupts.
 *
 *   Concurrency (detach): detach is ASYNCHRONOUS.  Memory safety is
 *   guaranteed by the ISR's borrowed reference (an in-flight ISR holds a +1
 *   refcount on the handle and can never touch freed memory), but detach
 *   only guarantees that no NEW callback invocation starts after it returns.
 *   An ISR that already snapshotted a non-NULL enabled callback before the
 *   detach may still call that old callback once afterwards.  Callers that
 *   free (or otherwise invalidate) state referenced by the callback MUST
 *   defer that teardown until any in-flight callback has returned, or ensure
 *   the state is safe to destroy lazily.  This matches NuttX's /dev/gpioN
 *   upper-half handler, which only performs asynchronous notification.
 *
 * Input Parameters:
 *   handle   - Handle from rk3576_gpio_get().
 *   callback - Interrupt callback, or NULL to detach.
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_gpio_irq_attach(FAR struct gpio_dev_s *handle,
                           rk3576_gpio_irq_callback_t callback);

/****************************************************************************
 * Name: rk3576_gpio_irq_enable
 *
 * Description:
 *   Enable (unmask) the GPIO interrupt on a claimed pin and enable the
 *   bank's GIC interrupt line.
 *
 *   The caller MUST call rk3576_gpio_irq_attach() (with a non-NULL callback)
 *   before the first rk3576_gpio_irq_enable() so the bank's GIC vector is
 *   installed; enable only unmasks the pin and brings up the GIC line.
 *
 *   Each call increments the bank's reference count and unmasks INTMASK for
 *   the pin.  Call rk3576_gpio_irq_disable() to reverse it.
 *
 * Input Parameters:
 *   handle - Handle from rk3576_gpio_get().
 *
 ****************************************************************************/

void rk3576_gpio_irq_enable(FAR struct gpio_dev_s *handle);

/****************************************************************************
 * Name: rk3576_gpio_irq_disable
 *
 * Description:
 *   Disable (mask) the GPIO interrupt on a claimed pin.
 *
 *   Masks INTMASK for the pin and decrements the bank's reference count.
 *   When no pin in the same bank remains enabled, the bank's GIC interrupt
 *   line is also disabled (up_disable_irq) to stop delivery at the GIC
 *   level.
 *
 * Input Parameters:
 *   handle - Handle from rk3576_gpio_get().
 *
 ****************************************************************************/

void rk3576_gpio_irq_disable(FAR struct gpio_dev_s *handle);

/****************************************************************************
 * Name: rk3576_gpio_set_pull
 *
 * Description:
 *   Set only the pull resistor (float/pull-up/pull-down) of a claimed pin.
 *   All other pin parameters are left unchanged.
 *
 * Input Parameters:
 *   handle - Handle from rk3576_gpio_get().
 *   pull   - enum rk3576_gpio_pull_e value.
 *
 ****************************************************************************/

void rk3576_gpio_set_pull(FAR struct gpio_dev_s *handle,
                          enum rk3576_gpio_pull_e pull);

/****************************************************************************
 * Name: rk3576_gpio_set_drive
 *
 * Description:
 *   Set only the drive strength of a claimed pin.  All other pin parameters
 *   are left unchanged.
 *
 * Input Parameters:
 *   handle - Handle from rk3576_gpio_get().
 *   drive  - enum rk3576_gpio_drive_e value.
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_gpio_set_drive(FAR struct gpio_dev_s *handle,
                          enum rk3576_gpio_drive_e drive);

/****************************************************************************
 * Name: rk3576_gpio_set_schmitt
 *
 * Description:
 *   Enable or disable only the schmitt trigger of a claimed pin.  All other
 *   pin parameters are left unchanged.
 *
 * Input Parameters:
 *   handle - Handle from rk3576_gpio_get().
 *   enable - true to enable schmitt trigger, false to disable.
 *
 ****************************************************************************/

void rk3576_gpio_set_schmitt(FAR struct gpio_dev_s *handle, bool enable);

/****************************************************************************
 * Name: rk3576_gpio_set_af
 *
 * Description:
 *   Set only the IOMUX alternate function (AF number) of a claimed pin.
 *   AF 0 selects plain GPIO, other values select the peripheral mux.  All
 *   other pin parameters are left unchanged.
 *
 * Input Parameters:
 *   handle - Handle from rk3576_gpio_get().
 *   af     - Alternate function number in [0, RK3576_AF_MAX].
 *
 ****************************************************************************/

void rk3576_gpio_set_af(FAR struct gpio_dev_s *handle, unsigned int af);

/****************************************************************************
 * Name: rk3576_gpio_set_mode
 *
 * Description:
 *   Set only the GPIO direction (input/output) of a claimed pin.  Sets the
 *   DDR bit.  All other pin parameters (including the IOMUX alternate
 *   function) are left unchanged.
 *
 *   NOTE: The DDR bit only takes effect while the pin is muxed as plain
 *   GPIO (IOMUX AF0).  If a non-zero AF is currently selected (via
 *   rk3576_gpio_set_af()), the direction is recorded but ignored by the
 *   hardware until the pin is switched back to AF0.
 *
 * Input Parameters:
 *   handle - Handle from rk3576_gpio_get().
 *   mode   - enum rk3576_gpio_mode_e value.
 *
 ****************************************************************************/

void rk3576_gpio_set_mode(FAR struct gpio_dev_s *handle,
                          enum rk3576_gpio_mode_e mode);

/****************************************************************************
 * Name: rk3576_gpio_set_int_type
 *
 * Description:
 *   Set only the interrupt trigger type (level vs. edge) of a claimed pin.
 *   All other pin parameters are left unchanged.
 *
 *   Note: BOTH_EDGE (set via rk3576_gpio_set_int_pol()) is only meaningful
 *   for edge-triggered pins, so set int_type to RK3576_GPIO_INT_EDGE before
 *   requesting both-edge triggering.
 *
 * Input Parameters:
 *   handle   - Handle from rk3576_gpio_get().
 *   int_type - enum rk3576_gpio_int_type_e value.
 *
 ****************************************************************************/

void rk3576_gpio_set_int_type(FAR struct gpio_dev_s *handle,
                              enum rk3576_gpio_int_type_e int_type);

/****************************************************************************
 * Name: rk3576_gpio_set_int_pol
 *
 * Description:
 *   Set only the interrupt polarity (active level) / trigger edge of a
 *   claimed pin.  All other pin parameters are left unchanged.
 *
 * Input Parameters:
 *   handle  - Handle from rk3576_gpio_get().
 *   int_pol - enum rk3576_gpio_int_pol_e value.
 *
 ****************************************************************************/

void rk3576_gpio_set_int_pol(FAR struct gpio_dev_s *handle,
                             enum rk3576_gpio_int_pol_e int_pol);

/****************************************************************************
 * Deprecated stateless APIs
 *
 * These operate on a bit-encoded pinset and re-decode port/pin on every
 * call.  They are retained for compatibility with existing callers (e.g.
 * board pin-mux setup) and their behavior is unchanged, but new code should
 * prefer rk3576_gpio_get()/put() with the associated handle-based accessors.
 * ****************************************************************************/

int rk3576_config_gpio(gpio_pinset_t pinset) deprecated_function;

void rk3576_gpio_write(gpio_pinset_t pinset, bool value) deprecated_function;

bool rk3576_gpio_read(gpio_pinset_t pinset) deprecated_function;

#endif /* __VENDOR_ROCKCHIP_RK3576_GPIO_H */
