/*
 * ina219.c — INA219 전류/전압 센서 드라이버 (HAL I2C)
 *
 * 캘리브레이션:
 *   R_shunt = 0.1Ω, Current_LSB = 0.1mA
 *   Cal = 0.04096 / (0.0001A × 0.1Ω) = 4096
 *
 * 측정 절차:
 *   1. INA219_Init(): 캘리브레이션 레지스터 기록
 *   2. INA219_Read(): 버스 전압, 전류 레지스터 읽기
 */

#include "ina219.h"
#include "i2c.h"

#define INA219_ADDR      (0x40U << 1)   /* HAL 8-bit 주소: 0x80, A0/A1=GND */

#define REG_CONFIG       0x00U
#define REG_BUS_VOLTAGE  0x02U
#define REG_CURRENT      0x04U
#define REG_CALIBRATION  0x05U

#define CAL_VALUE        4096U
#define CURRENT_LSB_mA   0.1f          /* mA/LSB */
#define BUS_VOLTAGE_LSB  0.004f        /* V/LSB (4mV) */

static HAL_StatusTypeDef write_reg(uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFFU)};
    return HAL_I2C_Master_Transmit(&hi2c1, INA219_ADDR, buf, 3, 10);
}

static HAL_StatusTypeDef read_reg(uint8_t reg, uint16_t *value)
{
    uint8_t buf[2];
    if (HAL_I2C_Master_Transmit(&hi2c1, INA219_ADDR, &reg, 1, 10) != HAL_OK)
        return HAL_ERROR;
    if (HAL_I2C_Master_Receive(&hi2c1, INA219_ADDR, buf, 2, 10) != HAL_OK)
        return HAL_ERROR;
    *value = ((uint16_t)buf[0] << 8) | buf[1];
    return HAL_OK;
}

INA219_Status INA219_Init(void)
{
    if (write_reg(REG_CALIBRATION, CAL_VALUE) != HAL_OK)
        return INA219_ERROR;
    return INA219_OK;
}

INA219_Status INA219_Read(INA219_Data *out)
{
    uint16_t raw;

    /* 버스 전압: bits[15:3] = 전압값, 4mV/LSB */
    if (read_reg(REG_BUS_VOLTAGE, &raw) != HAL_OK)
        return INA219_ERROR;
    out->voltage_V = (float)(raw >> 3) * BUS_VOLTAGE_LSB;

    /* 전류: signed 16-bit, 0.1mA/LSB */
    if (read_reg(REG_CURRENT, &raw) != HAL_OK)
        return INA219_ERROR;
    out->current_mA = (float)(int16_t)raw * CURRENT_LSB_mA;

    return INA219_OK;
}
