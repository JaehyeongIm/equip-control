/*
 * ina219.h — INA219 전류/전압 센서 드라이버 (HAL I2C)
 *
 * I2C1 (hi2c1): PB8(SCL/D15), PB9(SDA/D14)
 * 센서 주소: 0x40 (A0, A1 핀 GND)
 * Shunt 저항: 0.1Ω (모듈 내장)
 */

#ifndef INA219_H
#define INA219_H

#include <stdint.h>

typedef enum {
    INA219_OK    = 0,
    INA219_ERROR = 1,
} INA219_Status;

typedef struct {
    float current_mA;
    float voltage_V;
} INA219_Data;

INA219_Status INA219_Init(void);
INA219_Status INA219_Read(INA219_Data *out);

#endif /* INA219_H */
