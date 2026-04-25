/*
 * sht31.h — SHT31 온습도 센서 드라이버
 *
 * I2C1 (hi2c1): PB8(SCL/D15), PB9(SDA/D14)
 * 센서 주소: 0x45 (ADDR핀 3.3V) / 0x44 (ADDR핀 GND)
 */

#ifndef SHT31_H
#define SHT31_H

#include <stdint.h>

typedef enum {
    SHT31_OK    = 0,
    SHT31_ERROR = 1,
} SHT31_Status;

typedef struct {
    float temperature;  /* 섭씨 */
    float humidity;     /* % RH */
} SHT31_Data;

SHT31_Status SHT31_Read(SHT31_Data *out);

#endif /* SHT31_H */
