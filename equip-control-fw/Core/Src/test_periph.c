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
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ── 핀 정의 ──────────────────────────────────────────────────── */
#define BUZZER_PIN    GPIO_PIN_10   /* PB10  D6  */
#define BUZZER_PORT   GPIOB

#define SWITCH_PIN    GPIO_PIN_5    /* PB5   D4  */
#define SWITCH_PORT   GPIOB

#define FAN_PIN       GPIO_PIN_4    /* PB4   D5  */
#define FAN_PORT      GPIOB

/* ── INA219 I2C 주소 (A0=GND, A1=GND → 0x40, 8비트 = 0x80) ──── */
#define INA219_ADDR   0x80U         /* 0x40 << 1 */

#define INA219_REG_CONFIG  0x00U
#define INA219_REG_SHUNT   0x01U
#define INA219_REG_BUS     0x02U
#define INA219_REG_CURRENT 0x04U
#define INA219_REG_CALIB   0x05U

/* ── I2C1 타이밍 상수 (APB1 = 42 MHz, 100 kHz 표준 모드) ──────── */
#define I2C1_FREQ       42U         /* FREQ 필드 값 */
#define I2C1_CCR        210U        /* 42 000 000 / (2 × 100 000) */
#define I2C1_TRISE      43U         /* 42 + 1 */

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

    /* 스위치 PA10 → Input Pull-Up */
    g.Pin   = SWITCH_PIN;
    g.Mode  = GPIO_MODE_INPUT;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SWITCH_PORT, &g);

    /* 팬(IRF520 SIG) PC7 → Output PP */
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
/*  I2C1 (PB8=SCL D15, PB9=SDA D14) — 직접 레지스터 접근         */
/* ─────────────────────────────────────────────────────────────── */

static void i2c1_init_raw(void)
{
    /* I2C1 클럭 활성화 */
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;

    /* PB8(SCL), PB9(SDA) → AF4, Open-Drain, Pull-Up, High-Speed */
    /* MODER: AF(10) for PB8 bits[17:16], PB9 bits[19:18] */
    GPIOB->MODER &= ~((3U << 16U) | (3U << 18U));
    GPIOB->MODER |=  ((2U << 16U) | (2U << 18U));
    /* OTYPER: open-drain */
    GPIOB->OTYPER |= (1U << 8U) | (1U << 9U);
    /* OSPEEDR: high speed */
    GPIOB->OSPEEDR |= (3U << 16U) | (3U << 18U);
    /* PUPDR: pull-up */
    GPIOB->PUPDR &= ~((3U << 16U) | (3U << 18U));
    GPIOB->PUPDR |=  ((1U << 16U) | (1U << 18U));
    /* AFR[1]: AF4 for PB8(bits[3:0]), PB9(bits[7:4]) */
    GPIOB->AFR[1] &= ~((0xFU << 0U) | (0xFU << 4U));
    GPIOB->AFR[1] |=  ((4U   << 0U) | (4U   << 4U));

    /* I2C1 리셋 후 설정 */
    I2C1->CR1  = I2C_CR1_SWRST;
    I2C1->CR1  = 0U;
    I2C1->CR2  = I2C1_FREQ;
    I2C1->CCR  = I2C1_CCR;
    I2C1->TRISE = I2C1_TRISE;
    I2C1->CR1 |= I2C_CR1_PE;
}

/* SR1 플래그 대기 헬퍼 (timeout_ms 초과 시 0 반환) */
static uint8_t i2c1_wait_sr1(uint32_t flag, uint32_t timeout_ms)
{
    uint32_t end = HAL_GetTick() + timeout_ms;
    while (!(I2C1->SR1 & flag)) {
        if (HAL_GetTick() >= end) return 0U;
    }
    return 1U;
}

/* 장치 존재 확인 (ACK 수신 시 1 반환) */
static uint8_t i2c1_scan(uint8_t dev_addr)
{
    /* START */
    I2C1->CR1 |= I2C_CR1_START;
    if (!i2c1_wait_sr1(I2C_SR1_SB, 10U)) { I2C1->CR1 |= I2C_CR1_STOP; return 0U; }

    /* 주소 전송 (Write 방향) */
    I2C1->DR = dev_addr & 0xFEU;
    uint32_t end = HAL_GetTick() + 10U;
    while (HAL_GetTick() < end) {
        uint16_t sr1 = (uint16_t)I2C1->SR1;
        if (sr1 & I2C_SR1_ADDR) { (void)I2C1->SR2; I2C1->CR1 |= I2C_CR1_STOP; return 1U; }
        if (sr1 & I2C_SR1_AF)   { I2C1->SR1 &= ~I2C_SR1_AF;  I2C1->CR1 |= I2C_CR1_STOP; return 0U; }
    }
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0U;
}

/* 레지스터 2바이트 쓰기 */
static uint8_t i2c1_write_reg16(uint8_t dev_addr, uint8_t reg, uint16_t val)
{
    /* START */
    I2C1->CR1 |= I2C_CR1_START;
    if (!i2c1_wait_sr1(I2C_SR1_SB, 10U)) goto fail;

    /* 주소 (Write) */
    I2C1->DR = dev_addr & 0xFEU;
    if (!i2c1_wait_sr1(I2C_SR1_ADDR, 10U)) goto fail;
    (void)I2C1->SR2;

    /* 레지스터 주소 */
    I2C1->DR = reg;
    if (!i2c1_wait_sr1(I2C_SR1_TXE, 10U)) goto fail;

    /* MSB */
    I2C1->DR = (uint8_t)(val >> 8U);
    if (!i2c1_wait_sr1(I2C_SR1_TXE, 10U)) goto fail;

    /* LSB */
    I2C1->DR = (uint8_t)(val & 0xFFU);
    if (!i2c1_wait_sr1(I2C_SR1_BTF, 10U)) goto fail;

    I2C1->CR1 |= I2C_CR1_STOP;
    return 1U;
fail:
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0U;
}

/* 레지스터 2바이트 읽기 */
static uint8_t i2c1_read_reg16(uint8_t dev_addr, uint8_t reg, uint16_t *out)
{
    /* 레지스터 주소 쓰기 */
    I2C1->CR1 |= I2C_CR1_START;
    if (!i2c1_wait_sr1(I2C_SR1_SB, 10U)) goto fail;

    I2C1->DR = dev_addr & 0xFEU;
    if (!i2c1_wait_sr1(I2C_SR1_ADDR, 10U)) goto fail;
    (void)I2C1->SR2;

    I2C1->DR = reg;
    if (!i2c1_wait_sr1(I2C_SR1_TXE, 10U)) goto fail;

    /* Repeated START → 읽기 방향 */
    I2C1->CR1 |= I2C_CR1_START;
    if (!i2c1_wait_sr1(I2C_SR1_SB, 10U)) goto fail;

    I2C1->DR = dev_addr | 0x01U;

    /* 2바이트 읽기: 마지막 바이트 전 NACK+STOP 준비 */
    I2C1->CR1 |= I2C_CR1_ACK;
    if (!i2c1_wait_sr1(I2C_SR1_ADDR, 10U)) goto fail;
    (void)I2C1->SR2;

    /* MSB */
    if (!i2c1_wait_sr1(I2C_SR1_RXNE, 10U)) goto fail;
    uint8_t msb = (uint8_t)I2C1->DR;

    /* LSB: NACK + STOP 설정 후 수신 */
    I2C1->CR1 &= ~I2C_CR1_ACK;
    I2C1->CR1 |=  I2C_CR1_STOP;
    if (!i2c1_wait_sr1(I2C_SR1_RXNE, 10U)) goto fail;
    uint8_t lsb = (uint8_t)I2C1->DR;

    *out = (uint16_t)((msb << 8U) | lsb);
    return 1U;
fail:
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0U;
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
/*  Test 2: 택트 스위치 (PA10 / D2)                               */
/* ─────────────────────────────────────────────────────────────── */

static void test_switch(void)
{
    tprint("\r\n=== [2/5] SWITCH TEST  (PB5 / D4) ===\r\n");
    tprint("스위치 한 핀 → PB5,  다른 핀 → GND\r\n");
    tprint("10초간 버튼을 여러 번 눌러보세요...\r\n");

    GPIO_PinState last = HAL_GPIO_ReadPin(SWITCH_PORT, SWITCH_PIN);
    tprint("  초기 상태: %s\r\n",
           last == GPIO_PIN_SET ? "HIGH (해제)" : "LOW  (눌림)");

    uint32_t end = HAL_GetTick() + 10000U;
    while (HAL_GetTick() < end) {
        HAL_IWDG_Refresh(&hiwdg);
        GPIO_PinState cur = HAL_GPIO_ReadPin(SWITCH_PORT, SWITCH_PIN);
        if (cur != last) {
            tprint("  >> %s\r\n",
                   cur == GPIO_PIN_RESET ? "LOW  (눌림)" : "HIGH (해제)");
            last = cur;
        }
        HAL_Delay(10U);
    }
    tprint(">>> 눌림/해제가 출력됐으면 OK\r\n");
}

/* ─────────────────────────────────────────────────────────────── */
/*  Test 3: 포텐셔미터 ADC (PA0 / A0)                             */
/* ─────────────────────────────────────────────────────────────── */

static void test_potentiometer(void)
{
    tprint("\r\n=== [3/5] POTENTIOMETER ADC TEST  (PA0 / A0) ===\r\n");
    tprint("포텐 SIG → PA0,  VCC → 3.3V,  GND → GND\r\n");
    tprint("포텐을 천천히 돌려보세요 — 8회 측정\r\n");

    for (int i = 1; i <= 8; i++) {
        uint32_t raw = adc1_read_raw();
        float    vol = (float)raw * 3.3f / 4095.0f;
        tprint("  [%d] raw=%4lu  %.2f V\r\n", i, (unsigned long)raw, vol);
        test_delay_ms(700U);
    }
    tprint(">>> 포텐 돌릴 때 0~4095 사이로 변하면 OK\r\n");
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
    if (!i2c1_scan(INA219_ADDR)) {
        tprint(">>> FAIL: I2C 응답 없음 (addr=0x40)\r\n");
        tprint("    배선/풀업/VCC 재확인\r\n");
        return;
    }
    tprint("  I2C 스캔  OK  (addr=0x40)\r\n");

    /* Config 레지스터 기본값 확인 (0x399F) */
    uint16_t cfg = 0U;
    i2c1_read_reg16(INA219_ADDR, INA219_REG_CONFIG, &cfg);
    tprint("  Config = 0x%04X  %s\r\n", cfg,
           cfg == 0x399FU ? "(기본값 OK)" : "(기본값 아님 — 주의)");

    /*
     * 캘리브레이션 설정
     *   shunt = 0.1 Ω (모듈 내장)
     *   Current_LSB = 100 µA
     *   Cal = 0.04096 / (100e-6 × 0.1) = 4096 = 0x1000
     */
    i2c1_write_reg16(INA219_ADDR, INA219_REG_CALIB, 0x1000U);

    /* 3회 측정 출력 */
    tprint("  측정 3회 (1초 간격)\r\n");
    for (int i = 1; i <= 3; i++) {
        uint16_t bus_raw  = 0U;
        uint16_t shnt_raw = 0U;
        uint16_t curr_raw = 0U;

        i2c1_read_reg16(INA219_ADDR, INA219_REG_BUS,     &bus_raw);
        i2c1_read_reg16(INA219_ADDR, INA219_REG_SHUNT,   &shnt_raw);
        i2c1_read_reg16(INA219_ADDR, INA219_REG_CURRENT, &curr_raw);

        /* bus voltage: bits[15:3] × 4 mV */
        float vbus   = (float)(bus_raw >> 3U) * 0.004f;
        /* shunt voltage: signed × 10 µV → mV */
        float vshunt = (float)(int16_t)shnt_raw * 0.01f;
        /* current: signed × 100 µA → mA */
        float curr   = (float)(int16_t)curr_raw * 0.1f;

        tprint("  [%d] Vbus=%.2fV  Vshunt=%.3fmV  I=%.1fmA\r\n",
               i, vbus, vshunt, curr);
        test_delay_ms(1000U);
    }
    tprint(">>> 전압값이 출력되면 OK\r\n");
}

/* ─────────────────────────────────────────────────────────────── */
/*  Test 5: IRF520 + 쿨링 팬 (PC7 / D9)                           */
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
    i2c1_init_raw();

    test_buzzer();
    test_switch();
    test_potentiometer();
    test_ina219();
    test_fan();

    tprint("\r\n########################################\r\n");
    tprint("  ALL TESTS DONE\r\n");
    tprint("########################################\r\n");
}

#endif /* TEST_MODE */
