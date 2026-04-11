/*
 * sht31.h — SHT31 온습도 센서 드라이버
 *
 * I2C1 (hi2c1, MX_I2C1_Init으로 초기화): PB8(SCL/D15), PB9(SDA/D14)
 * 센서 주소: 0x44 (ADDR핀 GND)
 */

#ifndef SHT31_H
#define SHT31_H

#include <stdint.h>

/* 반환 상태 */
typedef enum {
    SHT31_OK    = 0,
    SHT31_ERROR = 1,
} SHT31_Status;

/* 측정 결과 */
typedef struct {
    float temperature;  /* 섭씨 */
    float humidity;     /* % RH */
} SHT31_Data;

/**
 * @brief SHT31 측정 트리거 후 데이터 읽기
 * @param out  결과 저장 포인터
 * @return SHT31_OK / SHT31_ERROR
 */
SHT31_Status SHT31_Read(SHT31_Data *out);

#endif /* SHT31_H */
