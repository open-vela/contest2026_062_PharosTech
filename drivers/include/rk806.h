/****************************************************************************
 * vendor/drivers/include/rk806.h
 *
 * Public interface for the Rockchip RK806 PMIC regulator driver.
 *
 * This header is the single public interface exposed to the kernel/board
 * side (e.g. kickpi_k7_appinit.c). Driver-internal private declarations
 * stay inside the rk806/ driver directory.
 ****************************************************************************/

#ifndef __DRIVERS_INCLUDE_RK806_H
#define __DRIVERS_INCLUDE_RK806_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk806_initialize
 *
 * Description:
 *   Register the RK806 PMIC regulators onto the NuttX regulator framework.
 *   Each BUCK/LDO output is registered as a separate regulator device.
 *
 * Input parameters:
 *   i2c - The I2C lower-half driver instance the RK806 is attached to.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk806_initialize(FAR struct i2c_master_s *i2c);

#endif /* __DRIVERS_INCLUDE_RK806_H */
