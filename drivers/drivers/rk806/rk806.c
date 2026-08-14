/****************************************************************************
 * vendor/drivers/drivers/rk806/rk806.c
 *
 * Rockchip RK806 PMIC regulator driver (lower-half).
 *
 * RK806 is a multi-channel PMIC used on the kickpi-k7 (RK3576) board:
 * several BUCK (DCDC) and LDO outputs, each independently switchable and
 * voltage-adjustable (discrete voltage tables).
 *
 * This file provides the lower-half regulator implementation registered
 * onto the NuttX regulator upper-half framework
 * (include/nuttx/power/regulator.h). Each BUCK/LDO output gets its own
 * struct regulator_desc_s + regulator_ops_s and is registered via
 * regulator_register().
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <nuttx/config.h>

#ifdef CONFIG_REGULATOR_RK806

#include <debug.h>
#include <errno.h>
#include <string.h>

#include <nuttx/devicetree/util_macro.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/kmalloc.h>
#include <nuttx/power/regulator.h>

#include "rk806.h"
#include "rk806_hw.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Output channel IDs.  The enum value doubles as the selector-family
 * discriminator used by the voltage decode helpers (BUCK vs LDO), and it is
 * stored in each regulator_desc_s.id by the RK806_*_REG descriptor macros.
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

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The RK806 driver private state.
 *
 * A single global instance is used: the RK806 is a chip-level device, and
 * this driver is expected to be initialized exactly once during board
 * bring-up.  The instance holds the I2C lower-half pointer and the resolved
 * bus parameters, and is shared by all BUCK/LDO regulator callbacks (which
 * receive it back through the regulator framework's `priv` pointer).
 */

struct rk806_dev_s
{
  FAR struct i2c_master_s *i2c; /* I2C lower-half driver instance */
  uint16_t addr;                /* I2C slave address */
  uint32_t frequency;           /* I2C bus frequency */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Global, statically-initialized private state for the single RK806 device.
 *
 * The per-output regulator descriptors and ops are declared as
 * file-scope static tables below (g_rk806_regulators[] and g_rk806_ops);
 * this instance is shared by all of them through the regulator
 * framework's `priv` pointer.
 */

static struct rk806_dev_s g_rk806 = {
  NULL,              /* i2c */
  RK806_I2C_ADDRESS, /* addr */
  RK806_BUS_SPEED,   /* frequency */
};

/* Fold an empty string into NULL for the desc->supply_name field.  The
 * Kconfig *_SUPPLY symbol is a string; when a board leaves it empty it
 * means the rail's input is a fixed (always-on) supply with no independent
 * parent regulator, so the framework must not try to resolve/enable it.
 */

#define RK806_SUPPLY_OR_NULL(s) ((s)[0] ? (s) : NULL)

/****************************************************************************
 * Name: rk806_decode_buck_voltage
 *
 * Description:
 *   Return the microvolt value for a BUCK output selector (piecewise).
 *
 ****************************************************************************/

static uint32_t rk806_decode_buck_voltage(unsigned int selector)
{
  if (selector < RK806_BUCK_SEL_END0)
    {
      return RK806_BUCK_VOLT_MIN0 + selector * RK806_BUCK_VOLT_STEP0;
    }
  else if (selector < RK806_BUCK_SEL_END1)
    {
      return RK806_BUCK_VOLT_MIN1 +
             (selector - RK806_BUCK_SEL_END0) * RK806_BUCK_VOLT_STEP1;
    }
  else
    {
      return RK806_BUCK_VOLT_MAX1;
    }
}

/****************************************************************************
 * Name: rk806_decode_ldo_voltage
 *
 * Description:
 *   Return the microvolt value for an LDO (NLDO/PLDO) output selector.
 *
 ****************************************************************************/

static uint32_t rk806_decode_ldo_voltage(unsigned int selector)
{
  if (selector < RK806_LDO_SEL_END0)
    {
      return RK806_LDO_VOLT_MIN0 + selector * RK806_LDO_VOLT_STEP0;
    }
  else
    {
      return RK806_LDO_VOLT_MAX0;
    }
}

/****************************************************************************
 * Name: rk806_get_voltage_count
 *
 * Description:
 *   Return the number of selectors for the given output family.
 *
 ****************************************************************************/

static unsigned int rk806_get_voltage_count(unsigned int id)
{
  if (id >= RK806_ID_NLDO1)
    {
      return RK806_LDO_SEL_COUNT;
    }
  else
    {
      return RK806_BUCK_SEL_COUNT;
    }
}

/****************************************************************************
 * Regulator descriptors.
 *
 * One descriptor per output.  All outputs (BUCK and LDO) share the same
 * adjustable range (RK806_MIN_UV .. RK806_MAX_UV, in microvolts).
 * Set boot_on / always_on as required by the kickpi-k7 power design
 * (default: off / not forced-on).

 */

#define RK806_BUCK_REG(_id, _enable_reg, _enable_mask)                    \
  {                                                                       \
    .name = CONFIG_RK806_BUCK##_id##_NAME, .id = RK806_ID_BUCK##_id,      \
    .n_voltages = RK806_BUCK_SEL_COUNT,                                   \
    .vsel_reg = RK806_REG_BUCK##_id##_ON_VSEL,                            \
    .vsel_mask = RK806_BUCK_VSEL_MASK, .enable_reg = _enable_reg,         \
    .enable_mask = _enable_mask, .min_uv = RK806_MIN_UV,                  \
    .max_uv = RK806_MAX_UV,                                               \
    .boot_on = IS_ENABLED(CONFIG_RK806_BUCK##_id##_BOOT_ON),              \
    .always_on = IS_ENABLED(CONFIG_RK806_BUCK##_id##_ALWAYS_ON),          \
    .supply_name = RK806_SUPPLY_OR_NULL(CONFIG_RK806_BUCK##_id##_SUPPLY), \
  }

#define RK806_NLDO_REG(_id, _enable_reg, _enable_mask, _supply)      \
  {                                                                  \
    .name = CONFIG_RK806_NLDO##_id##_NAME, .id = RK806_ID_NLDO##_id, \
    .n_voltages = RK806_LDO_SEL_COUNT,                               \
    .vsel_reg = RK806_REG_NLDO##_id##_ON_VSEL,                       \
    .vsel_mask = RK806_NLDO_VSEL_MASK, .enable_reg = _enable_reg,    \
    .enable_mask = _enable_mask, .min_uv = RK806_MIN_UV,             \
    .max_uv = RK806_MAX_UV,                                          \
    .boot_on = IS_ENABLED(CONFIG_RK806_NLDO##_id##_BOOT_ON),         \
    .always_on = IS_ENABLED(CONFIG_RK806_NLDO##_id##_ALWAYS_ON),     \
    .supply_name = RK806_SUPPLY_OR_NULL(_supply),                    \
  }

#define RK806_PLDO_REG(_id, _enable_reg, _enable_mask, _supply)      \
  {                                                                  \
    .name = CONFIG_RK806_PLDO##_id##_NAME, .id = RK806_ID_PLDO##_id, \
    .n_voltages = RK806_LDO_SEL_COUNT,                               \
    .vsel_reg = RK806_REG_PLDO##_id##_ON_VSEL,                       \
    .vsel_mask = RK806_PLDO_VSEL_MASK, .enable_reg = _enable_reg,    \
    .enable_mask = _enable_mask, .min_uv = RK806_MIN_UV,             \
    .max_uv = RK806_MAX_UV,                                          \
    .boot_on = IS_ENABLED(CONFIG_RK806_PLDO##_id##_BOOT_ON),         \
    .always_on = IS_ENABLED(CONFIG_RK806_PLDO##_id##_ALWAYS_ON),     \
    .supply_name = RK806_SUPPLY_OR_NULL(_supply),                    \
  }

static const struct regulator_desc_s
    g_rk806_regulators[RK806_NUM_REGULATORS] = {
      RK806_BUCK_REG(1, RK806_BUCK1_EN_REG, RK806_BUCK1_EN_MASK),
      RK806_BUCK_REG(2, RK806_BUCK2_EN_REG, RK806_BUCK2_EN_MASK),
      RK806_BUCK_REG(3, RK806_BUCK3_EN_REG, RK806_BUCK3_EN_MASK),
      RK806_BUCK_REG(4, RK806_BUCK4_EN_REG, RK806_BUCK4_EN_MASK),
      RK806_BUCK_REG(5, RK806_BUCK5_EN_REG, RK806_BUCK5_EN_MASK),
      RK806_BUCK_REG(6, RK806_BUCK6_EN_REG, RK806_BUCK6_EN_MASK),
      RK806_BUCK_REG(7, RK806_BUCK7_EN_REG, RK806_BUCK7_EN_MASK),
      RK806_BUCK_REG(8, RK806_BUCK8_EN_REG, RK806_BUCK8_EN_MASK),
      RK806_BUCK_REG(9, RK806_BUCK9_EN_REG, RK806_BUCK9_EN_MASK),
      RK806_BUCK_REG(10, RK806_BUCK10_EN_REG, RK806_BUCK10_EN_MASK),

      RK806_NLDO_REG(1, RK806_NLDO1_EN_REG, RK806_NLDO1_EN_MASK,
                     CONFIG_RK806_NLDO1_2_3_SUPPLY),
      RK806_NLDO_REG(2, RK806_NLDO2_EN_REG, RK806_NLDO2_EN_MASK,
                     CONFIG_RK806_NLDO1_2_3_SUPPLY),
      RK806_NLDO_REG(3, RK806_NLDO3_EN_REG, RK806_NLDO3_EN_MASK,
                     CONFIG_RK806_NLDO1_2_3_SUPPLY),
      RK806_NLDO_REG(4, RK806_NLDO4_EN_REG, RK806_NLDO4_EN_MASK,
                     CONFIG_RK806_NLDO4_5_SUPPLY),
      RK806_NLDO_REG(5, RK806_NLDO5_EN_REG, RK806_NLDO5_EN_MASK,
                     CONFIG_RK806_NLDO4_5_SUPPLY),

      RK806_PLDO_REG(1, RK806_PLDO1_EN_REG, RK806_PLDO1_EN_MASK,
                     CONFIG_RK806_PLDO1_2_3_SUPPLY),
      RK806_PLDO_REG(2, RK806_PLDO2_EN_REG, RK806_PLDO2_EN_MASK,
                     CONFIG_RK806_PLDO1_2_3_SUPPLY),
      RK806_PLDO_REG(3, RK806_PLDO3_EN_REG, RK806_PLDO3_EN_MASK,
                     CONFIG_RK806_PLDO1_2_3_SUPPLY),
      RK806_PLDO_REG(4, RK806_PLDO4_EN_REG, RK806_PLDO4_EN_MASK,
                     CONFIG_RK806_PLDO4_5_SUPPLY),
      RK806_PLDO_REG(5, RK806_PLDO5_EN_REG, RK806_PLDO5_EN_MASK,
                     CONFIG_RK806_PLDO4_5_SUPPLY),
      RK806_PLDO_REG(6, RK806_PLDO6_EN_REG, RK806_PLDO6_EN_MASK,
                     CONFIG_RK806_PLDO6_SUPPLY),
    };

#undef RK806_PLDO_REG
#undef RK806_NLDO_REG
#undef RK806_BUCK_REG

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk806_read_reg
 *
 * Description:
 *   Read a single 8-bit register from the RK806 over I2C.
 *
 ****************************************************************************/

static int rk806_read_reg(FAR struct rk806_dev_s *priv, uint8_t reg,
                          FAR uint8_t *value)
{
  struct i2c_msg_s msg[2];
  uint8_t regbuf[1];
  int ret;

  regbuf[0] = reg;

  /* First message: write the 8-bit register address (no stop). */

  msg[0].frequency = priv->frequency;
  msg[0].addr = priv->addr;
  msg[0].flags = 0;
  msg[0].buffer = regbuf;
  msg[0].length = 1;

  /* Second message: read back one byte. */

  msg[1].frequency = priv->frequency;
  msg[1].addr = priv->addr;
  msg[1].flags = I2C_M_READ;
  msg[1].buffer = value;
  msg[1].length = 1;

  ret = I2C_TRANSFER(priv->i2c, msg, 2);
  if (ret < 0)
    {
      _err("ERROR: rk806_read_reg(0x%02x) failed: %d\n", reg, ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: rk806_write_reg
 *
 * Description:
 *   Write a single 8-bit register to the RK806 over I2C.
 *
 ****************************************************************************/
static int rk806_write_reg(FAR struct rk806_dev_s *priv, uint8_t reg,
                           uint8_t value)
{
  struct i2c_msg_s msg;
  uint8_t buffer[2];
  int ret;

  buffer[0] = reg;
  buffer[1] = value;

  msg.frequency = priv->frequency;
  msg.addr = priv->addr;
  msg.flags = 0;
  msg.buffer = buffer;
  msg.length = 2;

  ret = I2C_TRANSFER(priv->i2c, &msg, 1);
  if (ret < 0)
    {
      _err("ERROR: rk806_write_reg(0x%02x) failed: %d\n", reg, ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: rk806_list_voltage
 *
 * Description:
 *   Return the microvolt value for a given vsel selector of the output
 *   identified by rdev->desc->id.
 *
 ****************************************************************************/

static int rk806_list_voltage(FAR struct regulator_dev_s *rdev,
                              unsigned int selector)
{
  unsigned int id = rdev->desc->id;

  if (id >= RK806_NUM_OUTPUTS || selector >= rk806_get_voltage_count(id))
    {
      return -EINVAL;
    }

  if (id >= RK806_ID_NLDO1)
    {
      return (int)rk806_decode_ldo_voltage(selector);
    }
  else
    {
      return (int)rk806_decode_buck_voltage(selector);
    }
}

/****************************************************************************
 * Name: rk806_set_voltage_sel
 *
 * Description:
 *   Program the ON_VSEL register of the given output with `selector`.
 *
 ****************************************************************************/

static int rk806_set_voltage_sel(FAR struct regulator_dev_s *rdev,
                                 unsigned int selector)
{
  FAR struct rk806_dev_s *priv = rdev->priv;
  FAR const struct regulator_desc_s *reg = rdev->desc;

  if (selector >= rk806_get_voltage_count(reg->id))
    {
      return -EINVAL;
    }

  return rk806_write_reg(priv, reg->vsel_reg, selector & reg->vsel_mask);
}

/****************************************************************************
 * Name: rk806_get_voltage_sel
 *
 * Description:
 *   Read the current vsel selector from the ON_VSEL register.
 *
 ****************************************************************************/

static int rk806_get_voltage_sel(FAR struct regulator_dev_s *rdev)
{
  FAR struct rk806_dev_s *priv = rdev->priv;
  FAR const struct regulator_desc_s *reg = rdev->desc;
  uint8_t val;
  int ret;

  ret = rk806_read_reg(priv, reg->vsel_reg, &val);
  if (ret < 0)
    {
      return ret;
    }

  return val & reg->vsel_mask;
}

/****************************************************************************
 * Name: rk806_enable
 *
 * Description:
 *   Enable (power up) the given output.
 *
 *   The POWER_EN register uses a write-mask protocol: the low 4 bits are
 *   the output state and the high 4 bits are the corresponding update mask
 *   (bit0<->bit4, bit1<->bit5, bit2<->bit6, bit3<->bit7).  Only bits whose
 *   mask is set are updated by the write, so this is NOT a read-modify-write
 *   operation.
 *
 *   reg->enable_mask holds the low-4-bit state bit of the output.  To drive
 *   it on we must write both the state bit (1) and its matching mask bit
 *   (state bit << 4).
 *
 ****************************************************************************/

static int rk806_enable(FAR struct regulator_dev_s *rdev)
{
  FAR struct rk806_dev_s *priv = rdev->priv;
  FAR const struct regulator_desc_s *reg = rdev->desc;
  uint8_t value;

  /* state bit set (enable) + corresponding mask bit = (state << 4) */

  value = (uint8_t)(reg->enable_mask | (reg->enable_mask << 4));
  return rk806_write_reg(priv, reg->enable_reg, value);
}

/****************************************************************************
 * Name: rk806_is_enabled
 *
 * Description:
 *   Return 1 if the output is enabled, 0 if disabled.
 *
 *   Only the low 4 bits of POWER_EN hold output state (the high 4 bits are
 *   the write mask), so we test the state bit against the low nibble.
 *
 ****************************************************************************/

static int rk806_is_enabled(FAR struct regulator_dev_s *rdev)
{
  FAR struct rk806_dev_s *priv = rdev->priv;
  FAR const struct regulator_desc_s *reg = rdev->desc;
  uint8_t val;
  int ret;

  ret = rk806_read_reg(priv, reg->enable_reg, &val);
  if (ret < 0)
    {
      return ret;
    }

  return (val & reg->enable_mask) ? 1 : 0;
}

/****************************************************************************
 * Name: rk806_disable
 *
 * Description:
 *   Disable (power down) the given output.
 *
 *   See rk806_enable() for the POWER_EN write-mask protocol.  To drive the
 *   output off we write its state bit as 0 but still set the matching mask
 *   bit so the write actually takes effect.
 *
 ****************************************************************************/

static int rk806_disable(FAR struct regulator_dev_s *rdev)
{
  FAR struct rk806_dev_s *priv = rdev->priv;
  FAR const struct regulator_desc_s *reg = rdev->desc;
  uint8_t value;

  /* state bit cleared (disable) + corresponding mask bit = (state << 4) */

  value = (uint8_t)(reg->enable_mask << 4);
  return rk806_write_reg(priv, reg->enable_reg, value);
}

/****************************************************************************
 * Regulator operations (shared by every output).
 ****************************************************************************/

static const struct regulator_ops_s g_rk806_ops = {
  rk806_list_voltage,    /* list_voltage     */
  NULL,                  /* set_voltage      */
  rk806_set_voltage_sel, /* set_voltage_sel  */
  NULL,                  /* get_voltage      */
  rk806_get_voltage_sel, /* get_voltage_sel  */
  rk806_enable,          /* enable           */
  rk806_is_enabled,      /* is_enabled       */
  rk806_disable,         /* disable          */
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk806_initialize
 *
 * Description:
 *   Register the RK806 PMIC regulators onto the regulator framework.
 *   Called from board initialization.
 *
 ****************************************************************************/

int rk806_initialize(FAR struct i2c_master_s *i2c)
{
  int ret;
  int i;
  int failed = false;
  struct rk806_dev_s *priv = &g_rk806;

  if (i2c == NULL)
    {
      return -ENODEV;
    }

  /* Store the I2C lower-half pointer in the global device state. */

  priv->i2c = i2c;

  uint8_t name_h, name_l;
  ret = rk806_read_reg(priv, RK806_REG_CHIP_NAME, &name_h);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk806_read_reg(priv, RK806_REG_CHIP_VER, &name_l);
  if (ret < 0)
    {
      return ret;
    }

  uint16_t chip_name = (uint16_t)(name_h << 4) + (name_l >> 4);
  uint16_t chip_version = name_l & 0x0f;

  if (chip_name != RK806_CHIP_NAME)
    {
      _err("ERROR: Invalid chip name 0x%X, expected 0x%X\n", chip_name,
           RK806_CHIP_NAME);
      return -ENODEV;
    }

  _info("Read rk806 chip name: 0x%X, chip version 0x%X\n", chip_name,
        chip_version);

  /* Register every BUCK/LDO output onto the regulator framework.  All
   * outputs share the same ops; per-output state (registers, voltage
   * table) is carried in the descriptor and reached through desc->id.
   */

  for (i = 0; i < RK806_NUM_REGULATORS; i++)
    {
      FAR struct regulator_dev_s *rdev;

      rdev = regulator_register(&g_rk806_regulators[i], &g_rk806_ops, priv);
      if (rdev == NULL)
        {
          _err("ERROR: failed to register %s\n", g_rk806_regulators[i].name);
          failed = true;
        }
      else
        {
          _info("Registered regulator %s\n", g_rk806_regulators[i].name);
        }
    }

  return failed ? -ENODEV : OK;
}

#endif /* CONFIG_REGULATOR_RK806 */
