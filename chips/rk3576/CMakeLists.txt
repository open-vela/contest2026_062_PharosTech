# SPDX-License-Identifier: Apache-2.0
set(SRCS rk3576_boot.c rk3576_serial.c rk3576_clk_tree.c)

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

if(CONFIG_RK3576_USB)
  list(APPEND SRCS rk3576_usb.c)
endif()

if(CONFIG_RK3576_USBHOST)
  list(APPEND SRCS rk3576_usbhost.c)
endif()

if(CONFIG_RK3576_EMMC)
  list(APPEND SRCS rk3576_emmc.c)
endif()

if(CONFIG_RK3576_DMA OR CONFIG_RK3576_USBHOST)
  list(APPEND SRCS rk3576_addrenv.c)
endif()

if(CONFIG_RK3576_DMA)
  list(APPEND SRCS rk3576_dma.c)
endif()

if(CONFIG_RK3576_DMA_ALLOC)
  list(APPEND SRCS rk3576_dma_alloc.c)
endif()

if(CONFIG_RK3576_SAI)
  list(APPEND SRCS rk3576_sai.c)
endif()

if(CONFIG_RK3576_FSPI)
  list(APPEND SRCS rk3576_fspi.c)
endif()

target_sources(arch PRIVATE ${SRCS})
