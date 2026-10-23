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
#include <stddef.h>

#include <nuttx/i2c/i2c_master.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* RK806 output IDs.  The order MUST match the chip-side hardware table in
 * rk806.c and the board configuration table passed to rk806_initialize().
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

/* How the voltage of a rail is known to software.
 *
 * READBACK - the voltage is controlled and read through the RK806 VSEL
 *            register; the normal regulator operations are used.
 * NOMINAL  - the rail is read-only and its voltage is a board nominal (for
 *            example an external-feedback rail).  Regulator reads return the
 *            nominal; voltage writes are refused and enable/disable do not
 *            touch the hardware.
 * UNKNOWN  - the rail is read-only and software cannot know its voltage
 *            (external feedback with no usable register).  Regulator reads
 *            return -ENODATA; voltage writes are refused and enable/disable
 *            do not touch the hardware.
 */

enum rk806_voltage_source_e
{
  RK806_VOLTAGE_READBACK = 0,
  RK806_VOLTAGE_NOMINAL,
  RK806_VOLTAGE_UNKNOWN,
};

/* Board-specific per-output configuration.  The chip's full voltage range is
 * described by the decode tables in rk806_hw.h; min_uv/max_uv here are the
 * board constraints for this particular rail.
 *
 * For NOMINAL rails min_uv == max_uv and the value is the board nominal.
 * UNKNOWN rails carry no voltage: min_uv and max_uv must be zero.
 */

struct rk806_rail_cfg
{
  FAR const char *name;
  FAR const char *supply;
  int min_uv;
  int max_uv;
  enum rk806_voltage_source_e voltage_source;
  bool boot_on;
  bool always_on;
};

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
 *   The board supplies the complete per-rail description: names, voltage
 *   limits, parent supplies, reporting policy and boot flags.  The driver
 *   only carries the chip-side register and voltage-table metadata.
 *
 * Input parameters:
 *   i2c   - The I2C lower-half driver instance the RK806 is attached to.
 *   rails - Board table indexed by RK806_ID_*, exactly RK806_NUM_OUTPUTS
 *           entries.
 *   n_rails - Number of entries in rails; must equal RK806_NUM_OUTPUTS.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk806_initialize(FAR struct i2c_master_s *i2c,
                     FAR const struct rk806_rail_cfg *rails, size_t n_rails);

#endif /* __DRIVERS_INCLUDE_RK806_H */
