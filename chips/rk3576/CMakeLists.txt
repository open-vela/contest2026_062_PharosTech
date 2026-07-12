# SPDX-License-Identifier: Apache-2.0
set(SRCS rk3576_boot.c rk3576_serial.c rk3576_cru.c)

if(CONFIG_RK3576_GPIO)
  list(APPEND SRCS rk3576_gpio.c)
endif()

if(CONFIG_ARCH_EARLY_PRINT)
  list(APPEND SRCS rk3576_lowputc.S)
endif()

if(CONFIG_RK3576_SDMMC)
  list(APPEND SRCS rk3576_sdmmc.c)
endif()

if(CONFIG_RK3576_I2C)
  list(APPEND SRCS rk3576_i2c.c)
endif()

if(CONFIG_RK3576_PWM)
  list(APPEND SRCS rk3576_pwm.c)
endif()

target_sources(arch PRIVATE ${SRCS})
