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

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>

#include "chip.h"
#include "hardware/rk3576_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO pinset encoding (32-bit word):
 *
 *  Bit [4:0]   - Pin number within port (0-31)
 *  Bit [7:5]   - Port number (0-4, same as Linux GPIO0-GPIO4)
 *  Bit [9:8]   - Mode (Input/Output/AF/Analog)
 *  Bit [10]    - (reserved — was GPIO_OPENDRAIN, RK3576 unsupported)
 *  Bit [12:11] - Pull-up/Pull-down
 *  Bit [14:13] - Output speed (reserved — IOC-controlled)
 *  Bit [18:15] - Alternate function number (0-15)
 *  Bit [19]    - Initial output value (GPIO_OUTPUT_SET)
 *  Bit [20]    - EXTI interrupt enable
 *  Bit [21]    - Interrupt type
 *  Bit [22]    - Interrupt polarity
 *  Bit [23]    - Schmitt trigger enable
 *
 * Pin naming follows Linux pinctrl-rockchip convention:
 *   RK_GPIO0_A0 = GPIO_PORT0 | GPIO_PIN_A0
 *   RK_GPIO4_D7 = GPIO_PORT4 | GPIO_PIN_D7
 */

/* Pin encoding ***************************************************************/

#define GPIO_PIN_SHIFT         (0)           /* Bits 0-4: GPIO pin number */
#define GPIO_PIN_MASK          (0x1f << GPIO_PIN_SHIFT)
/* Group A (pins 0-7) */
#  define GPIO_PIN_A0          (0 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_A1          (1 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_A2          (2 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_A3          (3 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_A4          (4 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_A5          (5 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_A6          (6 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_A7          (7 << GPIO_PIN_SHIFT)
/* Group B (pins 8-15) */
#  define GPIO_PIN_B0          (8 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_B1          (9 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_B2          (10 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_B3          (11 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_B4          (12 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_B5          (13 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_B6          (14 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_B7          (15 << GPIO_PIN_SHIFT)
/* Group C (pins 16-23) */
#  define GPIO_PIN_C0          (16 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_C1          (17 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_C2          (18 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_C3          (19 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_C4          (20 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_C5          (21 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_C6          (22 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_C7          (23 << GPIO_PIN_SHIFT)
/* Group D (pins 24-31) */
#  define GPIO_PIN_D0          (24 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_D1          (25 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_D2          (26 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_D3          (27 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_D4          (28 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_D5          (29 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_D6          (30 << GPIO_PIN_SHIFT)
#  define GPIO_PIN_D7          (31 << GPIO_PIN_SHIFT)

/* Port encoding *************************************************************/

#define GPIO_PORT_SHIFT        (5)           /* Bits 5-7: GPIO port number */
#define GPIO_PORT_MASK         (0x7 << GPIO_PORT_SHIFT)
#  define GPIO_PORT0           (0 << GPIO_PORT_SHIFT)  /* GPIO0 */
#  define GPIO_PORT1           (1 << GPIO_PORT_SHIFT)  /* GPIO1 */
#  define GPIO_PORT2           (2 << GPIO_PORT_SHIFT)  /* GPIO2 */
#  define GPIO_PORT3           (3 << GPIO_PORT_SHIFT)  /* GPIO3 */
#  define GPIO_PORT4           (4 << GPIO_PORT_SHIFT)  /* GPIO4 */

/* Mode encoding *************************************************************/

#define GPIO_MODE_SHIFT        (8)           /* Bits 8-9: Pin mode */
#define GPIO_MODE_MASK         (0x3 << GPIO_MODE_SHIFT)
#  define GPIO_INPUT           (0 << GPIO_MODE_SHIFT)  /* Input mode */
#  define GPIO_OUTPUT          (1 << GPIO_MODE_SHIFT)  /* Output mode */
#  define GPIO_ALT             (2 << GPIO_MODE_SHIFT)  /* Alternate function */
#  define GPIO_ANALOG          (3 << GPIO_MODE_SHIFT)  /* Analog mode */

/* Pull-up/Pull-down **********************************************************/

#define GPIO_PUPD_SHIFT        (11)          /* Bits 11-12: Pull-up/pull-down */
#define GPIO_PUPD_MASK         (0x3 << GPIO_PUPD_SHIFT)
#  define GPIO_FLOAT           (0 << GPIO_PUPD_SHIFT)  /* No pull */
#  define GPIO_PULLUP          (1 << GPIO_PUPD_SHIFT)  /* Pull-up */
#  define GPIO_PULLDOWN        (2 << GPIO_PUPD_SHIFT)  /* Pull-down */

/* Speed encoding (for output and AF pins) ************************************/

#define GPIO_SPEED_SHIFT       (13)          /* Bits 13-14: Output speed */
#define GPIO_SPEED_MASK        (0x3 << GPIO_SPEED_SHIFT)
#  define GPIO_SPEED_LOW       (0 << GPIO_SPEED_SHIFT)  /* Low speed */
#  define GPIO_SPEED_MED       (1 << GPIO_SPEED_SHIFT)  /* Medium speed */
#  define GPIO_SPEED_HIGH      (2 << GPIO_SPEED_SHIFT)  /* High speed */
#  define GPIO_SPEED_VHIGH     (3 << GPIO_SPEED_SHIFT)  /* Very high speed */

/* Alternate function encoding ************************************************/

#define GPIO_AF_SHIFT          (15)          /* Bits 15-18: AF number (0-15) */
#define GPIO_AF_MASK           (0xf << GPIO_AF_SHIFT)
#  define GPIO_AF0             (0 << GPIO_AF_SHIFT)
#  define GPIO_AF1             (1 << GPIO_AF_SHIFT)
#  define GPIO_AF2             (2 << GPIO_AF_SHIFT)
#  define GPIO_AF3             (3 << GPIO_AF_SHIFT)
#  define GPIO_AF4             (4 << GPIO_AF_SHIFT)
#  define GPIO_AF5             (5 << GPIO_AF_SHIFT)
#  define GPIO_AF6             (6 << GPIO_AF_SHIFT)
#  define GPIO_AF7             (7 << GPIO_AF_SHIFT)

/* Initial output value *******************************************************/

#define GPIO_OUTPUT_SET        (1 << 19)     /* Bit 19: Initial output high */

/* Interrupt configuration ****************************************************/

#define GPIO_EXTI              (1 << 20)     /* Bit 20: Enable EXTI interrupt */

#define GPIO_INTTYPE_SHIFT     (21)          /* Bit 21: Interrupt type */
#define GPIO_INTTYPE_MASK      (0x1 << GPIO_INTTYPE_SHIFT)
#  define GPIO_INT_LEVEL       (0 << GPIO_INTTYPE_SHIFT)  /* Level triggered */
#  define GPIO_INT_EDGE        (1 << GPIO_INTTYPE_SHIFT)  /* Edge triggered */

#define GPIO_INTPOL_SHIFT      (22)          /* Bit 22: Interrupt polarity */
#define GPIO_INTPOL_MASK       (0x1 << GPIO_INTPOL_SHIFT)
#  define GPIO_INT_LOW_FALLING (0 << GPIO_INTPOL_SHIFT)
#  define GPIO_INT_HIGH_RISING (1 << GPIO_INTPOL_SHIFT)

/* Schmitt trigger (kernel-internal use, automatically set by config_gpio
 * based on pin mode; user programs use GPIOC_SETPINTYPE ioctl instead)
 */

#define GPIO_SCHMITT           (1 << 23)     /* Bit 23: Enable schmitt trigger */

/* Convenience macros for common pin configurations ***************************/

#define GPIO_INPUT_PULLUP      (GPIO_INPUT | GPIO_PULLUP)
#define GPIO_INPUT_PULLDOWN    (GPIO_INPUT | GPIO_PULLDOWN)
#define GPIO_OUTPUT_PUSHPULL   (GPIO_OUTPUT)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* GPIO pin configuration encoded as a 32-bit value */

typedef uint32_t gpio_pinset_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_config_gpio
 *
 * Description:
 *   Configure a GPIO pin based on bit-encoded description of the pin.
 *
 * Input Parameters:
 *   pinset - Bit-encoded description of the GPIO pin
 *
 * Returned Value:
 *   OK on success; A negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_config_gpio(gpio_pinset_t pinset);

/****************************************************************************
 * Name: rk3576_gpio_write
 *
 * Description:
 *   Write one or zero to the selected GPIO pin.
 *
 * Input Parameters:
 *   pinset - Bit-encoded description of the GPIO pin
 *   value  - The value to write (true = high, false = low)
 *
 ****************************************************************************/

void rk3576_gpio_write(gpio_pinset_t pinset, bool value);

/****************************************************************************
 * Name: rk3576_gpio_read
 *
 * Description:
 *   Read the current value of the selected GPIO pin.
 *
 * Input Parameters:
 *   pinset - Bit-encoded description of the GPIO pin
 *
 * Returned Value:
 *   True if the pin is high, false if low.
 *
 ****************************************************************************/

bool rk3576_gpio_read(gpio_pinset_t pinset);

#ifdef CONFIG_DEV_GPIO
/****************************************************************************
 * Name: rk3576_gpio_init
 *
 * Description:
 *   Initialize the GPIO driver and register GPIO pin devices.
 *
 * Returned Value:
 *   OK on success; A negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_gpio_init(void);

int rk3576_gpio_register_input(gpio_pinset_t pinset);
int rk3576_gpio_register_output(gpio_pinset_t pinset);
int rk3576_gpio_register_interrupt(gpio_pinset_t pinset);
#endif

#endif /* __VENDOR_ROCKCHIP_RK3576_GPIO_H */
