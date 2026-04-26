/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dht22.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum {
    STATE_IDLE = 0,
    STATE_HEATING,
    STATE_WARNING,
    STATE_ALARM
} EquipState;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SP_DEFAULT        30.0f
#define WARN_THR          3.0f    /* SP+3°C → WARNING */
#define ALARM_THR         5.0f    /* SP+5°C → ALARM */
#define WARN_DUR_MS       5000U
#define ALARM_DUR_MS      10000U
#define RESET_THR         2.0f    /* 온도 ≤ SP-2°C 시 RESET 허용 */
#define SENSOR_FAIL_MAX   3
#define DHT_INTERVAL_MS   2000U
#define DATA_TX_MS        1000U
#define HB_TX_MS          5000U
#define RX_BUF_SIZE       64
#define BUZZER_TOGGLE_MS  500U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
static EquipState g_state     = STATE_IDLE;
static char       g_alm_id[8] = "NONE";

static float    g_sp       = SP_DEFAULT;
static float    g_kp       = 25.0f;
static float    g_ki       = 0.3f;
static float    g_kd       = 3.0f;
static float    g_integral = 0.0f;
static float    g_prev_err = 0.0f;
static uint32_t g_last_pid_tick = 0;

static float   g_temp        = 0.0f;
static uint8_t g_temp_valid  = 0;
static uint8_t g_sensor_fail = 0;
static uint32_t g_last_dht_tick = 0;

static uint32_t g_warn_start   = 0;
static uint32_t g_alarm_start  = 0;
static uint8_t  g_in_warn_tmr  = 0;
static uint8_t  g_in_alarm_tmr = 0;

static uint32_t g_last_data_tx = 0;
static uint32_t g_last_hb_tx   = 0;

static uint8_t          g_rx_byte = 0;
static char             g_rx_buf[RX_BUF_SIZE];
static uint8_t          g_rx_idx  = 0;
static volatile uint8_t g_line_ready = 0;
static char             g_line[RX_BUF_SIZE];

static uint32_t g_buzz_tick  = 0;
static uint8_t  g_buzz_state = 0;
static uint32_t g_led_tick   = 0;
static uint8_t  g_led_state  = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM3_Init(void);
/* USER CODE BEGIN PFP */
static void        uart_tx(const char *str);
static void        heater_off(void);
static void        heater_set(uint32_t cmp);
static uint32_t    pid_update(float temp, float sp, uint32_t dt_ms);
static const char *state_str(void);
static void        send_data(void);
static void        enter_alarm(const char *alm_id);
static void        handle_cmd(char *line);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) return;
    char c = (char)g_rx_byte;
    if (c == '\n') {
        if (g_rx_idx > 0 && g_rx_buf[g_rx_idx - 1] == '\r')
            g_rx_buf[g_rx_idx - 1] = '\0';
        else
            g_rx_buf[g_rx_idx] = '\0';
        if (!g_line_ready) {
            memcpy(g_line, g_rx_buf, sizeof(g_line));
            g_line_ready = 1;
        }
        g_rx_idx = 0;
    } else if (g_rx_idx < RX_BUF_SIZE - 1) {
        g_rx_buf[g_rx_idx++] = c;
    }
    HAL_UART_Receive_IT(&huart2, &g_rx_byte, 1);
}

static void uart_tx(const char *str)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)str, strlen(str), 200);
}

static void heater_off(void)
{
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, 0);
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_3);
    g_integral = 0.0f;
    g_prev_err = 0.0f;
}

static void heater_set(uint32_t cmp)
{
    if (cmp == 0) { heater_off(); return; }
    if (cmp > 1000) cmp = 1000;
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, cmp);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);
}

static uint32_t pid_update(float temp, float sp, uint32_t dt_ms)
{
    float dt  = (float)dt_ms / 1000.0f;
    float err = sp - temp;

    g_integral += err * dt;
    if (g_integral >  100.0f) g_integral =  100.0f;
    if (g_integral < -100.0f) g_integral = -100.0f;

    float deriv = (dt > 0.0f) ? (err - g_prev_err) / dt : 0.0f;
    g_prev_err  = err;

    float out = g_kp * err + g_ki * g_integral + g_kd * deriv;
    if (out <    0.0f) out =    0.0f;
    if (out > 1000.0f) out = 1000.0f;
    return (uint32_t)out;
}

static const char *state_str(void)
{
    switch (g_state) {
        case STATE_IDLE:    return "IDLE";
        case STATE_HEATING: return "HEATING";
        case STATE_WARNING: return "WARNING";
        case STATE_ALARM:   return "ALARM";
        default:            return "IDLE";
    }
}

static void send_data(void)
{
    char buf[96];
    const char *alm = (g_state == STATE_WARNING) ? "ALM-01"
                    : (g_state == STATE_ALARM)   ? g_alm_id
                    : "NONE";
    snprintf(buf, sizeof(buf), "DATA:%.1f,%.1f,%s,%s\r\n",
             g_temp, g_sp, state_str(), alm);
    uart_tx(buf);
}

static void enter_alarm(const char *alm_id)
{
    g_in_warn_tmr  = 0;
    g_in_alarm_tmr = 0;
    g_state = STATE_ALARM;
    strncpy(g_alm_id, alm_id, sizeof(g_alm_id) - 1);
    g_alm_id[sizeof(g_alm_id) - 1] = '\0';

    heater_off();
    HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);
    g_buzz_state = 1;

    char buf[32];
    snprintf(buf, sizeof(buf), "EVENT:ALARM,%s\r\n", alm_id);
    uart_tx(buf);
}

static void handle_cmd(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
        line[--len] = '\0';
    if (len == 0) return;

    if (strcmp(line, "START") == 0) {
        if (g_state != STATE_IDLE) {
            uart_tx("NACK:START,NOT_IDLE\r\n");
        } else {
            g_state        = STATE_HEATING;
            g_integral     = 0.0f;
            g_prev_err     = 0.0f;
            g_in_warn_tmr  = 0;
            g_in_alarm_tmr = 0;
            g_last_pid_tick = HAL_GetTick();
            uart_tx("ACK:START\r\n");
        }

    } else if (strcmp(line, "STOP") == 0) {
        if (g_state == STATE_WARNING)
            uart_tx("EVENT:CLEAR,ALM-01\r\n");
        g_state = STATE_IDLE;
        heater_off();
        HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
        g_buzz_state   = 0;
        g_in_warn_tmr  = 0;
        g_in_alarm_tmr = 0;
        uart_tx("ACK:STOP\r\n");

    } else if (strcmp(line, "RESET") == 0) {
        if (g_state != STATE_ALARM) {
            uart_tx("NACK:RESET,NOT_ALARM\r\n");
        } else if (!g_temp_valid) {
            uart_tx("NACK:RESET,NO_SENSOR\r\n");
        } else if (g_temp > (g_sp - RESET_THR)) {
            uart_tx("NACK:RESET,TEMP_HIGH\r\n");
        } else {
            strncpy(g_alm_id, "NONE", sizeof(g_alm_id));
            g_state = STATE_IDLE;
            heater_off();
            HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
            g_buzz_state   = 0;
            g_in_warn_tmr  = 0;
            g_in_alarm_tmr = 0;
            uart_tx("ACK:RESET\r\n");
        }

    } else if (strncmp(line, "SET:", 4) == 0) {
        float new_sp = strtof(line + 4, NULL);
        if (new_sp >= 20.0f && new_sp <= 80.0f) {
            g_sp = new_sp;
            char buf[32];
            snprintf(buf, sizeof(buf), "ACK:SET:%.1f\r\n", g_sp);
            uart_tx(buf);
        } else {
            uart_tx("NACK:SET,OUT_OF_RANGE\r\n");
        }

    } else if (strcmp(line, "STATUS") == 0) {
        send_data();
    }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_I2C1_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  DHT22_Init();
  HAL_Delay(500);

  heater_off();
  HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  uint32_t boot_now = HAL_GetTick();
  g_last_dht_tick = boot_now;
  g_last_data_tx  = boot_now;
  g_last_hb_tx    = boot_now;
  g_last_pid_tick = boot_now;

  HAL_UART_Receive_IT(&huart2, &g_rx_byte, 1);
  uart_tx("BOOT:OK\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t now = HAL_GetTick();

    /* 1. Process received UART command */
    if (g_line_ready) {
        char cmd[RX_BUF_SIZE];
        memcpy(cmd, g_line, sizeof(cmd));
        g_line_ready = 0;
        handle_cmd(cmd);
    }

    /* 2. DHT22 read every 2s */
    uint8_t new_reading = 0;
    if ((now - g_last_dht_tick) >= DHT_INTERVAL_MS) {
        g_last_dht_tick = now;
        DHT22_Data dht  = {0};
        if (DHT22_Read(&dht) == DHT22_OK) {
            g_temp        = dht.temperature;
            g_temp_valid  = 1;
            g_sensor_fail = 0;
            new_reading   = 1;
        } else {
            g_sensor_fail++;
            if (g_sensor_fail >= SENSOR_FAIL_MAX) {
                g_temp_valid = 0;
                if (g_state != STATE_ALARM)
                    enter_alarm("ALM-03");
            }
        }
    }

    /* 3. PID update on new valid reading */
    if (new_reading && (g_state == STATE_HEATING || g_state == STATE_WARNING)) {
        uint32_t dt = now - g_last_pid_tick;
        if (dt == 0) dt = DHT_INTERVAL_MS;
        g_last_pid_tick = now;
        heater_set(pid_update(g_temp, g_sp, dt));
    }

    /* 4. Alarm tier logic */
    if (g_temp_valid && (g_state == STATE_HEATING || g_state == STATE_WARNING)) {
        float over = g_temp - g_sp;

        if (over >= ALARM_THR) {
            if (!g_in_warn_tmr)  { g_in_warn_tmr  = 1; g_warn_start  = now; }
            if (!g_in_alarm_tmr) { g_in_alarm_tmr = 1; g_alarm_start = now; }

            if (g_state == STATE_HEATING && (now - g_warn_start) >= WARN_DUR_MS) {
                g_state = STATE_WARNING;
                uart_tx("EVENT:WARN,ALM-01\r\n");
            }
            if ((now - g_alarm_start) >= ALARM_DUR_MS) {
                if (g_state == STATE_WARNING)
                    uart_tx("EVENT:CLEAR,ALM-01\r\n");
                enter_alarm("ALM-02");
            }

        } else if (over >= WARN_THR) {
            g_in_alarm_tmr = 0;
            if (!g_in_warn_tmr) { g_in_warn_tmr = 1; g_warn_start = now; }

            if (g_state == STATE_HEATING && (now - g_warn_start) >= WARN_DUR_MS) {
                g_state = STATE_WARNING;
                uart_tx("EVENT:WARN,ALM-01\r\n");
            }

        } else {
            g_in_warn_tmr  = 0;
            g_in_alarm_tmr = 0;
            if (g_state == STATE_WARNING) {
                g_state = STATE_HEATING;
                uart_tx("EVENT:CLEAR,ALM-01\r\n");
            }
        }
    }

    /* 5. Buzzer: ALARM=연속, WARNING=500ms 토글, else=OFF */
    if (g_state == STATE_ALARM) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);
        g_buzz_state = 1;
    } else if (g_state == STATE_WARNING) {
        if ((now - g_buzz_tick) >= BUZZER_TOGGLE_MS) {
            g_buzz_tick  = now;
            g_buzz_state = !g_buzz_state;
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10,
                              g_buzz_state ? GPIO_PIN_SET : GPIO_PIN_RESET);
        }
    } else if (g_buzz_state) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
        g_buzz_state = 0;
    }

    /* 6. LED: IDLE=OFF, HEATING=ON, WARNING=500ms, ALARM=200ms */
    if (g_state == STATE_IDLE) {
        HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
        g_led_state = 0;
    } else if (g_state == STATE_HEATING) {
        HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
        g_led_state = 1;
    } else {
        uint32_t blink = (g_state == STATE_ALARM) ? 200U : 500U;
        if ((now - g_led_tick) >= blink) {
            g_led_tick  = now;
            g_led_state = !g_led_state;
            HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin,
                              g_led_state ? GPIO_PIN_SET : GPIO_PIN_RESET);
        }
    }

    /* 7. DATA TX 1s 주기 */
    if ((now - g_last_data_tx) >= DATA_TX_MS) {
        g_last_data_tx = now;
        send_data();
    }

    /* 8. HB TX 5s 주기 */
    if ((now - g_last_hb_tx) >= HB_TX_MS) {
        g_last_hb_tx = now;
        uart_tx("HB\r\n");
    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 83;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, FAN_RELAY_Pin|LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(DHT22_DATA_GPIO_Port, DHT22_DATA_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : FAN_RELAY_Pin LD2_Pin */
  GPIO_InitStruct.Pin = FAN_RELAY_Pin|LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : DHT22_DATA_Pin */
  GPIO_InitStruct.Pin = DHT22_DATA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(DHT22_DATA_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* BUZZER: PB10 / D6 */
  GPIO_InitStruct.Pin   = GPIO_PIN_10;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
