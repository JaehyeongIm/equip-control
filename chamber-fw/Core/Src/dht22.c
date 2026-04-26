#include "dht22.h"

#define DHT_PORT  DHT22_DATA_GPIO_Port
#define DHT_PIN   DHT22_DATA_Pin

static void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (HAL_RCC_GetHCLKFreq() / 1000000U);
    while ((DWT->CYCCNT - start) < ticks);
}

static void pin_output(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin   = DHT_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT_PORT, &g);
}

static void pin_input(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin  = DHT_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(DHT_PORT, &g);
}

void DHT22_Init(void)
{
    /* DWT 카운터 활성화 (us 딜레이용) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

DHT22_Status DHT22_Read(DHT22_Data *out)
{
    uint8_t  data[5] = {0};
    uint32_t timeout;

    /* 시작 신호: 1ms LOW 후 릴리즈 */
    pin_output();
    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_SET);
    delay_us(30);

    pin_input();

    /* DHT22 응답 대기: LOW ~80us */
    timeout = 10000;
    while (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) == GPIO_PIN_SET)
        if (--timeout == 0) return DHT22_ERROR;

    /* HIGH ~80us */
    timeout = 10000;
    while (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) == GPIO_PIN_RESET)
        if (--timeout == 0) return DHT22_ERROR;

    timeout = 10000;
    while (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) == GPIO_PIN_SET)
        if (--timeout == 0) return DHT22_ERROR;

    /* 40비트 수신 */
    for (int i = 0; i < 40; i++) {
        /* 비트 시작 LOW (~50us) 끝날 때까지 대기 */
        timeout = 10000;
        while (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) == GPIO_PIN_RESET)
            if (--timeout == 0) return DHT22_ERROR;

        /* 40us 후 레벨 확인: HIGH이면 1, LOW이면 0 */
        delay_us(40);
        data[i / 8] <<= 1;
        if (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) == GPIO_PIN_SET) {
            data[i / 8] |= 1;
            timeout = 10000;
            while (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) == GPIO_PIN_SET)
                if (--timeout == 0) return DHT22_ERROR;
        }
    }

    /* 체크섬 검증 */
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF))
        return DHT22_ERROR;

    uint16_t raw_h = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_t = ((uint16_t)data[2] << 8) | data[3];

    out->humidity    = raw_h / 10.0f;
    out->temperature = (raw_t & 0x8000U)
                       ? -(float)(raw_t & 0x7FFFU) / 10.0f
                       :  (float)raw_t / 10.0f;

    return DHT22_OK;
}
