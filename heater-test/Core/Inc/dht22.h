#ifndef DHT22_H
#define DHT22_H

#include "main.h"

typedef enum { DHT22_OK = 0, DHT22_ERROR } DHT22_Status;

typedef struct {
    float temperature;  /* 섭씨 */
    float humidity;     /* % RH */
} DHT22_Data;

void        DHT22_Init(void);
DHT22_Status DHT22_Read(DHT22_Data *out);

#endif /* DHT22_H */
