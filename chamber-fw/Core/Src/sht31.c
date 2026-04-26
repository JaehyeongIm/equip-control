/*
 * sht31.c — SHT31 온습도 센서 드라이버 (HAL I2C)
 *
 * 측정 절차 (Single-Shot High Repeatability, No Clock Stretch):
 *   1. Write {0x24, 0x00}
 *   2. 15 ms 대기
 *   3. Read 6 bytes: [T_MSB, T_LSB, T_CRC, H_MSB, H_LSB, H_CRC]
 *
 * 변환 공식:
 *   temp = -45 + 175 * raw_t / 65535
 *   humi = 100 * raw_h / 65535
 */

#include "sht31.h"
#include "main.h"

#define SHT31_ADDR  (0x44U << 1)  /* ADDR핀 GND → 0x44, HAL 8-bit 주소: 0x88 */

extern I2C_HandleTypeDef hi2c1;

SHT31_Status SHT31_Read(SHT31_Data *out)
{
    uint8_t cmd[2] = {0x24U, 0x00U};

    if (HAL_I2C_Master_Transmit(&hi2c1, SHT31_ADDR, cmd, 2, 10) != HAL_OK)
        return SHT31_ERROR;

    HAL_Delay(15U);

    uint8_t buf[6];
    if (HAL_I2C_Master_Receive(&hi2c1, SHT31_ADDR, buf, 6, 20) != HAL_OK)
        return SHT31_ERROR;

    uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];

    out->temperature = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
    out->humidity    = 100.0f * ((float)raw_h / 65535.0f);

    return SHT31_OK;
}
