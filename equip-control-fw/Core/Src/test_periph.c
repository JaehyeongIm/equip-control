#ifdef TEST_MODE
/*
 * test_periph.c — 주변장치 연결 테스트 (TEST_MODE 전용)
 *
 * HAL ADC/I2C 드라이버 소스가 프로젝트에 없으므로
 * ADC·I2C 는 CMSIS 레지스터를 직접 조작한다.
 * GPIO·UART·IWDG 는 기존 HAL 을 그대로 사용한다.
 *
 * 사용법:
 *   STM32CubeIDE > Project > Properties > C/C++ Build > Settings >
 *   MCU GCC Compiler > Preprocessor > Define symbols:  TEST_MODE
 *   빌드 후 플래시 → UART2 115200 터미널에서 결과 확인
 *
 * IWDG 주의:
 *   IWDG 타임아웃 ≈ 512 ms (Prescaler=4, Reload=4095, LSI=32kHz)
 *   test_delay_ms() 가 400 ms 마다 IWDG 를 갱신하므로 리셋 없이 동작한다.
 */

#include "test_periph.h"
#include "main.h"
#include "usart.h"
#include "iwdg.h"
#include "i2c.h"
#include "sht31.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ── 핀 정의 ──────────────────────────────────────────────────── */
#define BUZZER_PIN    GPIO_PIN_10   /* PB10  D6  */
#define BUZZER_PORT   GPIOB

#define FAN_PIN       GPIO_PIN_4    /* PB4   D5  */
#define FAN_PORT      GPIOB

/* ── INA219 I2C 주소 (A0=GND, A1=GND → 0x40, 8비트 = 0x80) ──── */
#define INA219_ADDR   0x80U         /* 0x40 << 1 */

#define INA219_REG_CONFIG  0x00U
#define INA219_REG_SHUNT   0x01U
#define INA219_REG_BUS     0x02U
#define INA219_REG_CURRENT 0x04U
#define INA219_REG_CALIB   0x05U

/* ─────────────────────────────────────────────────────────────── */
/*  공통 유틸                                                      */
/* ─────────────────────────────────────────────────────────────── */

static void tprint(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)strlen(buf), 200U);
}

/* IWDG 갱신 내장 딜레이 */
static void test_delay_ms(uint32_t ms)
{
    uint32_t end = HAL_GetTick() + ms;
    while (HAL_GetTick() < end) {
        HAL_IWDG_Refresh(&hiwdg);
        uint32_t left = end - HAL_GetTick();
        HAL_Delay(left > 400U ? 400U : left);
    }
}

/* ─────────────────────────────────────────────────────────────── */
/*  GPIO 초기화 (부저·스위치·팬)                                  */
/* ─────────────────────────────────────────────────────────────── */

static void gpio_periph_init(void)
{
    GPIO_InitTypeDef g = {0};

    /* 부저 PB10 → Output PP */
    g.Pin   = BUZZER_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BUZZER_PORT, &g);
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);

    /* 팬(IRF520 SIG) PB4 → Output PP */
    g.Pin   = FAN_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(FAN_PORT, &g);
    HAL_GPIO_WritePin(FAN_PORT, FAN_PIN, GPIO_PIN_RESET);
}

/* ─────────────────────────────────────────────────────────────── */
/*  ADC1 CH0 (PA0 / A0) — 직접 레지스터 접근                      */
/* ─────────────────────────────────────────────────────────────── */

static void adc1_init_raw(void)
{
    /* ADC1 클럭 활성화 */
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    /* PA0 → Analog 모드 (MODER bits [1:0] = 11) */
    GPIOA->MODER |= (3U << 0U);

    /* ADC1 설정: 소프트웨어 트리거, 12비트, 단일 변환 */
    ADC1->CR1  = 0U;
    ADC1->CR2  = 0U;
    ADC1->SQR1 = 0U;               /* 변환 수 = 1 (L=0) */
    ADC1->SQR3 = 0U;               /* 첫 번째 변환 = CH0 */
    ADC1->SMPR2 = (4U << 0U);      /* CH0 샘플링 시간 = 84 사이클 */

    ADC1->CR2 |= ADC_CR2_ADON;    /* ADC 활성화 */
    HAL_Delay(2U);                 /* 안정화 대기 */
}

static uint32_t adc1_read_raw(void)
{
    ADC1->SR  &= ~ADC_SR_EOC;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    uint32_t end = HAL_GetTick() + 10U;
    while (!(ADC1->SR & ADC_SR_EOC)) {
        if (HAL_GetTick() >= end) return 0xFFFFU;
    }
    return ADC1->DR & 0xFFFU;
}


/* ─────────────────────────────────────────────────────────────── */
/*  Test 1: 부저 (PB10 / D6)                                      */
/* ─────────────────────────────────────────────────────────────── */

static void test_buzzer(void)
{
    tprint("\r\n=== [1/5] BUZZER TEST  (PB10 / D6) ===\r\n");
    tprint("부저 모듈 I/O → 220Ω → PB10,  VCC → 3.3V\r\n");

    for (int i = 1; i <= 3; i++) {
        tprint("  BEEP %d  ON\r\n", i);
        HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);
        test_delay_ms(500U);
        tprint("  BEEP %d  OFF\r\n", i);
        HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
        test_delay_ms(500U);
    }
    tprint(">>> 소리가 3번 났으면 OK\r\n");
}

/* ─────────────────────────────────────────────────────────────── */
/*  Test 2: 포텐셔미터 ADC (PA0 / A0)                             */
/* ─────────────────────────────────────────────────────────────── */

static void test_potentiometer(void)
{
    tprint("\r\n=== [2/5] POTENTIOMETER ADC TEST  (PA0 / A0) ===\r\n");
    tprint("포텐 SIG → PA0,  VCC → 3.3V,  GND → GND\r\n");
    tprint("포텐을 천천히 돌려보세요 — 8회 측정\r\n");

    for (int i = 1; i <= 8; i++) {
        uint32_t raw = adc1_read_raw();
        uint32_t vol_i = raw * 330U / 4095U;   /* 0~330 (×0.01V) */
        tprint("  [%d] raw=%4lu  %lu.%02lu V\r\n", i,
               (unsigned long)raw, (unsigned long)(vol_i / 100U),
               (unsigned long)(vol_i % 100U));
        test_delay_ms(700U);
    }
    tprint(">>> 포텐 돌릴 때 0~4095 사이로 변하면 OK\r\n");
}

/* ─────────────────────────────────────────────────────────────── */
/*  Test 3: SHT31 온습도 센서 (I2C1 — PB9 SDA / PB8 SCL)         */
/* ─────────────────────────────────────────────────────────────── */

static void test_sht31(void)
{
    tprint("\r\n=== [3/5] SHT31 TEMP/HUMI TEST  (PB9 SDA D14 / PB8 SCL D15) ===\r\n");
    tprint("SHT31 SDA → D14, SCL → D15, VDD → 3.3V, ADDR → GND\r\n");
    tprint("4.7kΩ 풀업×2 → 3.3V 연결 필요\r\n");

    /* 0x44 / 0x45 둘 다 스캔 */
    uint8_t sht31_addr = 0U;
    if (HAL_I2C_IsDeviceReady(&hi2c1, (0x44U << 1), 1, 10) == HAL_OK) {
        sht31_addr = 0x44U;
        tprint("  I2C 스캔  OK  (addr=0x44)\r\n");
    } else if (HAL_I2C_IsDeviceReady(&hi2c1, (0x45U << 1), 1, 10) == HAL_OK) {
        sht31_addr = 0x45U;
        tprint("  I2C 스캔  OK  (addr=0x45) — ADDR핀이 3.3V에 연결됨\r\n");
    } else {
        tprint(">>> FAIL: I2C 응답 없음 (0x44, 0x45 모두 시도)\r\n");
        tprint("    VDD/납땜/풀업 재확인\r\n");
        return;
    }
    (void)sht31_addr;

    /* 5회 측정 (1초 간격) */
    tprint("  측정 5회 (1초 간격)\r\n");
    for (int i = 1; i <= 5; i++) {
        SHT31_Data d;
        if (SHT31_Read(&d) == SHT31_OK) {
            int t_i = (int)d.temperature;
            int t_d = (int)(d.temperature * 10.0f) % 10;
            int h_i = (int)d.humidity;
            int h_d = (int)(d.humidity * 10.0f) % 10;
            tprint("  [%d] Temp=%d.%d C  Humi=%d.%d %%\r\n",
                   i, t_i, t_d, h_i, h_d);
        } else {
            tprint("  [%d] READ ERROR\r\n", i);
        }
        test_delay_ms(1000U);
    }
    tprint(">>> 온도/습도 값이 출력되면 OK\r\n");
}

/* ─────────────────────────────────────────────────────────────── */
/*  Test 4: INA219 전류 센서 (I2C1 — PB9 SDA / PB8 SCL)          */
/* ─────────────────────────────────────────────────────────────── */

static void test_ina219(void)
{
    tprint("\r\n=== [4/5] INA219 I2C TEST  (PB9 SDA D14 / PB8 SCL D15) ===\r\n");
    tprint("4.7kΩ 풀업×2 → 3.3V 연결 필요\r\n");
    tprint("INA219 IN+/IN- 는 측정할 전류 경로에 직렬 삽입\r\n");

    /* 장치 응답 확인 */
    if (HAL_I2C_IsDeviceReady(&hi2c1, INA219_ADDR, 1, 10) != HAL_OK) {
        tprint(">>> FAIL: I2C 응답 없음 (addr=0x40)\r\n");
        tprint("    배선/풀업/VCC 재확인\r\n");
        return;
    }
    tprint("  I2C 스캔  OK  (addr=0x40)\r\n");

    /* Config 레지스터 기본값 확인 (0x399F) */
    uint8_t  rbuf[2];
    uint16_t cfg = 0U;
    HAL_I2C_Mem_Read(&hi2c1, INA219_ADDR, INA219_REG_CONFIG,
                     I2C_MEMADD_SIZE_8BIT, rbuf, 2, 10);
    cfg = ((uint16_t)rbuf[0] << 8) | rbuf[1];
    tprint("  Config = 0x%04X  %s\r\n", cfg,
           cfg == 0x399FU ? "(기본값 OK)" : "(기본값 아님 — 주의)");

    /*
     * 캘리브레이션 설정
     *   shunt = 0.1 Ω (모듈 내장)
     *   Current_LSB = 100 µA
     *   Cal = 0.04096 / (100e-6 × 0.1) = 4096 = 0x1000
     */
    uint8_t wbuf[2] = {0x10U, 0x00U};
    HAL_I2C_Mem_Write(&hi2c1, INA219_ADDR, INA219_REG_CALIB,
                      I2C_MEMADD_SIZE_8BIT, wbuf, 2, 10);

    /* 3회 측정 출력 */
    tprint("  측정 3회 (1초 간격)\r\n");
    for (int i = 1; i <= 3; i++) {
        uint8_t  tmp[2];
        uint16_t bus_raw  = 0U;
        uint16_t shnt_raw = 0U;
        uint16_t curr_raw = 0U;

        HAL_I2C_Mem_Read(&hi2c1, INA219_ADDR, INA219_REG_BUS,
                         I2C_MEMADD_SIZE_8BIT, tmp, 2, 10);
        bus_raw = ((uint16_t)tmp[0] << 8) | tmp[1];

        HAL_I2C_Mem_Read(&hi2c1, INA219_ADDR, INA219_REG_SHUNT,
                         I2C_MEMADD_SIZE_8BIT, tmp, 2, 10);
        shnt_raw = ((uint16_t)tmp[0] << 8) | tmp[1];

        HAL_I2C_Mem_Read(&hi2c1, INA219_ADDR, INA219_REG_CURRENT,
                         I2C_MEMADD_SIZE_8BIT, tmp, 2, 10);
        curr_raw = ((uint16_t)tmp[0] << 8) | tmp[1];

        /* bus voltage: bits[15:3] × 4 mV */
        uint32_t vbus_mv  = (bus_raw >> 3U) * 4U;
        /* shunt voltage: signed × 10 µV → 0.01 mV 단위 */
        int32_t  vsh_uv   = (int32_t)(int16_t)shnt_raw * 10;
        /* current: signed × 100 µA */
        int32_t  curr_ua  = (int32_t)(int16_t)curr_raw * 100;

        tprint("  [%d] Vbus=%lu.%03lu V  Vshunt=%ld uV  I=%ld uA\r\n",
               i,
               (unsigned long)(vbus_mv / 1000U),
               (unsigned long)(vbus_mv % 1000U),
               (long)vsh_uv,
               (long)curr_ua);
        test_delay_ms(1000U);
    }
    tprint(">>> 전압값이 출력되면 OK\r\n");
}

/* ─────────────────────────────────────────────────────────────── */
/*  Test 5: IRF520 + 쿨링 팬 (PB4 / D5)                           */
/* ─────────────────────────────────────────────────────────────── */

static void test_fan(void)
{
    tprint("\r\n=== [5/5] FAN (IRF520) TEST  (PB4 / D5) ===\r\n");
    tprint("PB4 → 1kΩ → IRF520 SIG\r\n");
    tprint("팬+ → 5V,  팬- → IRF520 M+,  M- → GND\r\n");

    tprint("  >> FAN ON  (3초)\r\n");
    HAL_GPIO_WritePin(FAN_PORT, FAN_PIN, GPIO_PIN_SET);
    test_delay_ms(3000U);

    tprint("  >> FAN OFF (2초)\r\n");
    HAL_GPIO_WritePin(FAN_PORT, FAN_PIN, GPIO_PIN_RESET);
    test_delay_ms(2000U);

    tprint("  >> FAN ON  (3초)\r\n");
    HAL_GPIO_WritePin(FAN_PORT, FAN_PIN, GPIO_PIN_SET);
    test_delay_ms(3000U);

    HAL_GPIO_WritePin(FAN_PORT, FAN_PIN, GPIO_PIN_RESET);
    tprint(">>> 팬이 돌→멈춤→돌면 OK\r\n");
}

/* ─────────────────────────────────────────────────────────────── */
/*  진입점                                                         */
/* ─────────────────────────────────────────────────────────────── */

void test_periph_run(void)
{
    test_delay_ms(3000U);  /* screen 접속 대기 */

    tprint("\r\n########################################\r\n");
    tprint("  PERIPH TEST MODE\r\n");
    tprint("  UART2  115200 8N1\r\n");
    tprint("########################################\r\n");

    gpio_periph_init();
    adc1_init_raw();
    /* I2C1: MX_I2C1_Init()이 main.c에서 이미 초기화함 */

    test_buzzer();
    test_potentiometer();
    test_sht31();
    test_ina219();
    test_fan();

    tprint("\r\n########################################\r\n");
    tprint("  ALL TESTS DONE\r\n");
    tprint("########################################\r\n");
}

#endif /* TEST_MODE */
