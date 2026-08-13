/****************************************************************************
 * vendor/drivers/drivers/rk806/rk806_hw.h
 *
 * Private hardware definitions for the Rockchip RK806 PMIC regulator
 * driver (lower-half).
 *
 * This header collects all RK806 hardware-specific constants: I2C bus
 * parameters, register map, field masks, and voltage-table metadata.  It is
 * private to the rk806/ driver directory and is NOT installed into the
 * public include path.
 ****************************************************************************/

#ifndef __DRIVERS_RK806_HW_H
#define __DRIVERS_RK806_HW_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK806 I2C bus parameters */

#define RK806_I2C_ADDRESS (0x23)   /* 7-bit slave address */
#define RK806_BUS_SPEED   (400000) /* 400 kHz */

/* RK806 chip identity */
#define RK806_CHIP_NAME 0x806

/* RK806 register map (common subset) */

#define RK806_REG_POWER_EN0         0x00
#define RK806_REG_POWER_EN1         0x01
#define RK806_REG_POWER_EN2         0x02
#define RK806_REG_POWER_EN3         0x03
#define RK806_REG_POWER_EN4         0x04
#define RK806_REG_POWER_EN5         0x05
#define RK806_REG_POWER_SLP_EN0     0x06
#define RK806_REG_POWER_SLP_EN1     0x07
#define RK806_REG_POWER_SLP_EN2     0x08
#define RK806_REG_POWER_DISCHRG_EN0 0x09
#define RK806_REG_POWER_DISCHRG_EN1 0x0a
#define RK806_REG_POWER_DISCHRG_EN2 0x0b
#define RK806_REG_BUCK_FB_CONFIG    0x0c
#define RK806_REG_SLP_LP_CONFIG     0x0d
#define RK806_REG_POWER_FPWM_EN0    0x0e
#define RK806_REG_POWER_FPWM_EN1    0x0f
#define RK806_REG_BUCK1_CONFIG      0x10
#define RK806_REG_BUCK2_CONFIG      0x11
#define RK806_REG_BUCK3_CONFIG      0x12
#define RK806_REG_BUCK4_CONFIG      0x13
#define RK806_REG_BUCK5_CONFIG      0x14
#define RK806_REG_BUCK6_CONFIG      0x15
#define RK806_REG_BUCK7_CONFIG      0x16
#define RK806_REG_BUCK8_CONFIG      0x17
#define RK806_REG_BUCK9_CONFIG      0x18
#define RK806_REG_BUCK10_CONFIG     0x19
#define RK806_REG_BUCK1_ON_VSEL     0x1a
#define RK806_REG_BUCK2_ON_VSEL     0x1b
#define RK806_REG_BUCK3_ON_VSEL     0x1c
#define RK806_REG_BUCK4_ON_VSEL     0x1d
#define RK806_REG_BUCK5_ON_VSEL     0x1e
#define RK806_REG_BUCK6_ON_VSEL     0x1f
#define RK806_REG_BUCK7_ON_VSEL     0x20
#define RK806_REG_BUCK8_ON_VSEL     0x21
#define RK806_REG_BUCK9_ON_VSEL     0x22
#define RK806_REG_BUCK10_ON_VSEL    0x23
#define RK806_REG_BUCK1_SLP_VSEL    0x24
#define RK806_REG_BUCK2_SLP_VSEL    0x25
#define RK806_REG_BUCK3_SLP_VSEL    0x26
#define RK806_REG_BUCK4_SLP_VSEL    0x27
#define RK806_REG_BUCK5_SLP_VSEL    0x28
#define RK806_REG_BUCK6_SLP_VSEL    0x29
#define RK806_REG_BUCK7_SLP_VSEL    0x2a
#define RK806_REG_BUCK8_SLP_VSEL    0x2b
#define RK806_REG_BUCK9_SLP_VSEL    0x2c
#define RK806_REG_BUCK10_SLP_VSEL   0x2d
#define RK806_REG_BUCK_DEBUG13      0x3c
#define RK806_REG_BUCK_DEBUG14      0x3d
#define RK806_REG_BUCK_DEBUG15      0x3e
#define RK806_REG_BUCK_DEBUG16      0x3f
#define RK806_REG_BUCK_DEBUG17      0x40
#define RK806_REG_NLDO_IMAX         0x42
#define RK806_REG_NLDO1_ON_VSEL     0x43
#define RK806_REG_NLDO2_ON_VSEL     0x44
#define RK806_REG_NLDO3_ON_VSEL     0x45
#define RK806_REG_NLDO4_ON_VSEL     0x46
#define RK806_REG_NLDO5_ON_VSEL     0x47
#define RK806_REG_NLDO1_SLP_VSEL    0x48
#define RK806_REG_NLDO2_SLP_VSEL    0x49
#define RK806_REG_NLDO3_SLP_VSEL    0x4a
#define RK806_REG_NLDO4_SLP_VSEL    0x4b
#define RK806_REG_NLDO5_SLP_VSEL    0x4c
#define RK806_REG_PLDO_IMAX         0x4d
#define RK806_REG_PLDO1_ON_VSEL     0x4e
#define RK806_REG_PLDO2_ON_VSEL     0x4f
#define RK806_REG_PLDO3_ON_VSEL     0x50
#define RK806_REG_PLDO4_ON_VSEL     0x51
#define RK806_REG_PLDO5_ON_VSEL     0x52
#define RK806_REG_PLDO6_ON_VSEL     0x53
#define RK806_REG_PLDO1_SLP_VSEL    0x54
#define RK806_REG_PLDO2_SLP_VSEL    0x55
#define RK806_REG_PLDO3_SLP_VSEL    0x56
#define RK806_REG_PLDO4_SLP_VSEL    0x57
#define RK806_REG_PLDO5_SLP_VSEL    0x58
#define RK806_REG_PLDO6_SLP_VSEL    0x59
#define RK806_REG_CHIP_NAME         0x5a
#define RK806_REG_CHIP_VER          0x5b
#define RK806_REG_OTP_VER           0x5c
#define RK806_REG_SYS_STS           0x5d
#define RK806_REG_SYS_CFG0          0x5e
#define RK806_REG_SYS_CFG1          0x5f
#define RK806_REG_SYS_OPTION        0x61
#define RK806_REG_PWRCTRL_CONFIG0   0x62
#define RK806_REG_PWRCTRL_CONFIG1   0x63
#define RK806_REG_VSEL_CTR_SEL0     0x64
#define RK806_REG_VSEL_CTR_SEL1     0x65
#define RK806_REG_VSEL_CTR_SEL2     0x66
#define RK806_REG_VSEL_CTR_SEL3     0x67
#define RK806_REG_VSEL_CTR_SEL4     0x68
#define RK806_REG_VSEL_CTR_SEL5     0x69
#define RK806_REG_DVS_CTRL_SEL0     0x6a
#define RK806_REG_DVS_CTRL_SEL1     0x6b
#define RK806_REG_DVS_CTRL_SEL2     0x6c
#define RK806_REG_DVS_CTRL_SEL3     0x6d
#define RK806_REG_DVS_CTRL_SEL4     0x6e
#define RK806_REG_DVS_START_CTRL    0x70
#define RK806_REG_PWRCTRL_GPIO      0x71
#define RK806_REG_SYS_CFG3          0x72
#define RK806_REG_WDT_REG           0x73
#define RK806_REG_ON_SOURCE         0x74
#define RK806_REG_OFF_SOURCE        0x75
#define RK806_REG_PWRON_KEY         0x76
#define RK806_REG_INT_STS0          0x77
#define RK806_REG_INT_MSK0          0x78
#define RK806_REG_INT_STS1          0x79
#define RK806_REG_INT_MSK1          0x7a
#define RK806_REG_GPIO_INT_CONFIG   0x7b
#define RK806_REG_DATA_REG0         0x7c
#define RK806_REG_DATA_REG1         0x7d
#define RK806_REG_DATA_REG2         0x7e
#define RK806_REG_DATA_REG3         0x7f
#define RK806_REG_DATA_REG4         0x80
#define RK806_REG_DATA_REG5         0x81
#define RK806_REG_DATA_REG6         0x82
#define RK806_REG_DATA_REG7         0x83
#define RK806_REG_DATA_REG8         0x84
#define RK806_REG_DATA_REG9         0x85
#define RK806_REG_DATA_REG10        0x86
#define RK806_REG_DATA_REG11        0x87
#define RK806_REG_DATA_REG12        0x88
#define RK806_REG_DATA_REG13        0x89
#define RK806_REG_DATA_REG14        0x8a
#define RK806_REG_DATA_REG15        0x8b
#define RK806_REG_BUCK_SEQ_REG0     0xB2
#define RK806_REG_BUCK_SEQ_REG1     0xB3
#define RK806_REG_BUCK_SEQ_REG2     0xB4
#define RK806_REG_BUCK_SEQ_REG3     0xB5
#define RK806_REG_BUCK_SEQ_REG4     0xB6
#define RK806_REG_BUCK_SEQ_REG5     0xB7
#define RK806_REG_BUCK_SEQ_REG6     0xB8
#define RK806_REG_BUCK_SEQ_REG7     0xB9
#define RK806_REG_BUCK_SEQ_REG8     0xBA
#define RK806_REG_BUCK_SEQ_REG9     0xBB
#define RK806_REG_BUCK_SEQ_REG10    0xBC
#define RK806_REG_BUCK_SEQ_REG11    0xBD
#define RK806_REG_BUCK_SEQ_REG12    0xBE
#define RK806_REG_BUCK_SEQ_REG13    0xBF
#define RK806_REG_BUCK_SEQ_REG14    0xC0
#define RK806_REG_BUCK_SEQ_REG15    0xC1
#define RK806_REG_BUCK_SEQ_REG16    0xC2
#define RK806_REG_BUCK_SEQ_REG17    0xC3
#define RK806_REG_BACKUP_REG7       0xDC
#define RK806_REG_BACKUP_REG6       0xE6
#define RK806_REG_BACKUP_REG5       0xE7
#define RK806_REG_BACKUP_REG1       0xE8
#define RK806_REG_BACKUP_REG2       0xE9
#define RK806_REG_BACKUP_REG3       0xEA
#define RK806_REG_BACKUP_REG4       0xEB
#define RK806_REG_BUCK_RSERVE_REG3  0xFD
#define RK806_REG_BUCK_RSERVE_REG4  0xFE

/* RK806 regulator output counts */

#define RK806_NUM_BUCKS 10
#define RK806_NUM_NLDOS 5
#define RK806_NUM_PLDOS 6
#define RK806_NUM_REGULATORS \
  (RK806_NUM_BUCKS + RK806_NUM_NLDOS + RK806_NUM_PLDOS)

/* VSEL register field width, per output family.
 * The vsel field is 8 bits wide (selector 0..255) for every family, and the
 * resulting voltage is a piecewise-linear function of that selector (see
 * the decode helpers in rk806.c).
 */

#define RK806_BUCK_VSEL_MASK 0xff
#define RK806_NLDO_VSEL_MASK 0xff
#define RK806_PLDO_VSEL_MASK 0xff

/* Enable-control register and state bit for each output.
 *
 * The POWER_EN registers use a write-mask protocol: the low 4 bits hold the
 * output state and the high 4 bits hold the corresponding update mask
 * (bit0<->bit4, bit1<->bit5, bit2<->bit6, bit3<->bit7).  Only bits whose
 * mask is set are modified by a write.
 *
 * The *_EN_MASK below is therefore the LOW-4-BIT STATE BIT of the output
 * (ONE of bit0..bit3); the matching mask bit used when writing is derived
 * automatically as (state bit << 4) by the driver, so it is NOT declared
 * here.
 */

#define RK806_BUCK1_EN_REG   RK806_REG_POWER_EN0
#define RK806_BUCK1_EN_MASK  (1u << 0)
#define RK806_BUCK2_EN_REG   RK806_REG_POWER_EN0
#define RK806_BUCK2_EN_MASK  (1u << 1)
#define RK806_BUCK3_EN_REG   RK806_REG_POWER_EN0
#define RK806_BUCK3_EN_MASK  (1u << 2)
#define RK806_BUCK4_EN_REG   RK806_REG_POWER_EN0
#define RK806_BUCK4_EN_MASK  (1u << 3)

#define RK806_BUCK5_EN_REG   RK806_REG_POWER_EN1
#define RK806_BUCK5_EN_MASK  (1u << 0)
#define RK806_BUCK6_EN_REG   RK806_REG_POWER_EN1
#define RK806_BUCK6_EN_MASK  (1u << 1)
#define RK806_BUCK7_EN_REG   RK806_REG_POWER_EN1
#define RK806_BUCK7_EN_MASK  (1u << 2)
#define RK806_BUCK8_EN_REG   RK806_REG_POWER_EN1
#define RK806_BUCK8_EN_MASK  (1u << 3)

#define RK806_BUCK9_EN_REG   RK806_REG_POWER_EN2
#define RK806_BUCK9_EN_MASK  (1u << 0)
#define RK806_BUCK10_EN_REG  RK806_REG_POWER_EN2
#define RK806_BUCK10_EN_MASK (1u << 1)

#define RK806_NLDO1_EN_REG   RK806_REG_POWER_EN3
#define RK806_NLDO1_EN_MASK  (1u << 0)
#define RK806_NLDO2_EN_REG   RK806_REG_POWER_EN3
#define RK806_NLDO2_EN_MASK  (1u << 1)
#define RK806_NLDO3_EN_REG   RK806_REG_POWER_EN3
#define RK806_NLDO3_EN_MASK  (1u << 2)
#define RK806_NLDO4_EN_REG   RK806_REG_POWER_EN3
#define RK806_NLDO4_EN_MASK  (1u << 3)

#define RK806_PLDO6_EN_REG   RK806_REG_POWER_EN4
#define RK806_PLDO6_EN_MASK  (1u << 0)
#define RK806_PLDO1_EN_REG   RK806_REG_POWER_EN4
#define RK806_PLDO1_EN_MASK  (1u << 1)
#define RK806_PLDO2_EN_REG   RK806_REG_POWER_EN4
#define RK806_PLDO2_EN_MASK  (1u << 2)
#define RK806_PLDO3_EN_REG   RK806_REG_POWER_EN4
#define RK806_PLDO3_EN_MASK  (1u << 3)

#define RK806_PLDO4_EN_REG   RK806_REG_POWER_EN5
#define RK806_PLDO4_EN_MASK  (1u << 0)
#define RK806_PLDO5_EN_REG   RK806_REG_POWER_EN5
#define RK806_PLDO5_EN_MASK  (1u << 1)
#define RK806_NLDO5_EN_REG   RK806_REG_POWER_EN5
#define RK806_NLDO5_EN_MASK  (1u << 2)

#endif /* __DRIVERS_RK806_HW_H */
