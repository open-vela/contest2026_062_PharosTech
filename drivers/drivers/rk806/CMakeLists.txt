############################################################################
# boards/rk3576/drivers/drivers/rk806/CMakeLists.txt
#
# Rockchip RK806 PMIC regulator driver build (CMake mode).
#
# RK806 is a multi-channel PMIC: multiple BUCK (DCDC) + LDO outputs, each
# independently switchable and voltage-adjustable. It is registered onto the
# NuttX regulator framework (include/nuttx/power/regulator.h).
#
# This is a kernel-side driver: injected into the kernel `drivers` library
# target (libdrivers.a), not apps.
############################################################################

# Enable config (Kconfig-configured) for the rk806 driver.
if(CONFIG_REGULATOR_RK806)
  target_sources(drivers PRIVATE rk806.c)
endif()
