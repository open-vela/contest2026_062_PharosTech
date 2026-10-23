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

#include <stdbool.h>

#include <nuttx/i2c/i2c_master.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* RK806 output IDs.  The order MUST match the descriptor table in rk806.c
 * and the board configuration table passed to rk806_rail_configure().
 */

enum rk806_output_e
{
  RK806_ID_BUCK1 = 0,
  RK806_ID_BUCK2,
  RK806_ID_BUCK3,
  RK806_ID_BUCK4,
  RK806_ID_BUCK5,
  RK806_ID_BUCK6,
  RK806_ID_BUCK7,
  RK806_ID_BUCK8,
  RK806_ID_BUCK9,
  RK806_ID_BUCK10,
  RK806_ID_NLDO1,
  RK806_ID_NLDO2,
  RK806_ID_NLDO3,
  RK806_ID_NLDO4,
  RK806_ID_NLDO5,
  RK806_ID_PLDO1,
  RK806_ID_PLDO2,
  RK806_ID_PLDO3,
  RK806_ID_PLDO4,
  RK806_ID_PLDO5,
  RK806_ID_PLDO6,
  RK806_NUM_OUTPUTS,
};

/* Board-specific per-output configuration.  The chip's full voltage range is
 * described by the decode tables in rk806_hw.h; min_uv/max_uv here are the
 * board constraints for this particular rail.
 *
 * readonly selects a passive operations table: the rail can be queried but
 * not enabled, disabled, or voltage-set.  This is used for the DDR rails,
 * which the boot chain has already trained and which must not be touched.
 */

struct rk806_rail_cfg
{
  int min_uv;
  int max_uv;
  bool readonly;
  FAR const char *supply;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk806_rail_configure
 *
 * Description:
 *   Apply board-specific limits, read-only policy, and parent supply name to
 *   one RK806 output.  Must be called before rk806_initialize(), which
 *   registers the descriptors onto the regulator framework.
 *
 * Input parameters:
 *   id  - One of the RK806_ID_* output IDs.
 *   cfg - The board configuration for that output.
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL if the ID or configuration is invalid.
 *
 ****************************************************************************/

int rk806_rail_configure(unsigned int id,
                         FAR const struct rk806_rail_cfg *cfg);

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
