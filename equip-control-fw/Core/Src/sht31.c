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
#include "i2c.h"
#include "main.h"

#define SHT31_ADDR    (0x45U << 1)  /* HAL 8-bit 주소: 0x8A  (ADDR핀=3.3V) */

SHT31_Status SHT31_Read(SHT31_Data *out)
{
    uint8_t cmd[2] = {0x24U, 0x00U};

    /* 측정 명령 전송 */
    if (HAL_I2C_Master_Transmit(&hi2c1, SHT31_ADDR, cmd, 2, 10) != HAL_OK)
        return SHT31_ERROR;

    /* 측정 대기 (최대 15 ms) */
    HAL_Delay(15U);

    /* 6바이트 읽기 */
    uint8_t buf[6];
    if (HAL_I2C_Master_Receive(&hi2c1, SHT31_ADDR, buf, 6, 20) != HAL_OK)
        return SHT31_ERROR;

    /* 변환 (CRC 바이트 buf[2], buf[5] 는 무시) */
    uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];

    out->temperature = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
    out->humidity    = 100.0f * ((float)raw_h / 65535.0f);

    return SHT31_OK;
}
