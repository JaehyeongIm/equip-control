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
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dht22.h"
#include "task.h"       /* vTaskNotifyGiveFromISR, vTaskSuspendAll */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum {
    STATE_IDLE = 0,
    STATE_HEATING,
    STATE_WARNING,
    STATE_ALARM
} ChamberState;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SP_DEFAULT        30.0f
#define WARN_THR          1.0f
#define ALARM_THR         2.0f
#define WARN_DUR_MS       5000U
#define ALARM_DUR_MS      10000U
#define RESET_THR         2.0f
#define SENSOR_FAIL_MAX   3
#define DHT_INTERVAL_MS   2000U
#define DATA_TX_MS        1000U
#define RX_BUF_SIZE       64
#define BUZZER_TOGGLE_MS  1000U
#define SETTLE_BAND_C     1.0f
#define SETTLE_COUNT      5U

/* Fixed-duty heat-up test mode.
 * TEST_FIXED_DUTY_ENABLE=1: PID 대신 히터를 50% duty로 고정.
 * TEST_FIXED_DUTY_ENABLE=0: 기본 PID 제어.
 */
#define TEST_FIXED_DUTY_ENABLE  0
#define TEST_FIXED_DUTY_CMP     500U
#define STOP_HEATER_AT_TARGET   1

/* Relay polarity. Active-Low 릴레이 모듈이면 두 값을 바꿀 것. */
#define FAN_ON            GPIO_PIN_SET
#define FAN_OFF           GPIO_PIN_RESET

/* UART TX 큐 설정 */
#define TX_MSG_SIZE       128U
#define TX_QUEUE_LEN      8U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart2;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */

/* ── 제어 상태 (g_state_mutex로 보호) ─────────────────────────── */
static ChamberState g_state     = STATE_IDLE;
static char       g_alm_id[8] = "NONE";

static float    g_sp       = SP_DEFAULT;
static float    g_kp       = 200.0f;
static float    g_ki       = 2.0f;
static float    g_kd       = 0.0f;
static float    g_integral = 0.0f;
static float    g_prev_err = 0.0f;
static uint32_t g_last_pid_tick = 0;

static float   g_temp        = 0.0f;
static uint8_t g_temp_valid  = 0;
static uint8_t g_sensor_fail = 0;

static uint32_t g_warn_start   = 0;
static uint32_t g_alarm_start  = 0;
static uint8_t  g_in_warn_tmr  = 0;
static uint8_t  g_in_alarm_tmr = 0;

static uint32_t g_heater_cmp = 0;

/* KPI */
static uint8_t  g_run_active     = 0;
static uint32_t g_run_start_tick = 0;
static uint8_t  g_target_reached = 0;
static uint32_t g_reach_ms       = 0;
static float    g_peak_temp_after_reach = 0.0f;
static float    g_overshoot_c           = 0.0f;
static uint8_t  g_settled       = 0;
static uint32_t g_settle_ms     = 0;
static uint8_t  g_in_band_count = 0;

/* ── UART RX 버퍼 (ISR + UartRxTask) ──────────────────────────── */
static uint8_t          g_rx_byte = 0;
static char             g_rx_buf[RX_BUF_SIZE];
static uint8_t          g_rx_idx  = 0;
static volatile uint8_t g_line_ready = 0;
static char             g_line[RX_BUF_SIZE];

/* ── RTOS 오브젝트 ─────────────────────────────────────────────── */
static osMutexId_t        g_state_mutex;   /* 공유 상태 보호 */
static osSemaphoreId_t    g_sensor_sem;    /* SensorTask → ControlTask 동기화 */
static osMessageQueueId_t g_tx_queue;      /* 모든 태스크 → UartTxTask */
static osThreadId_t       g_uart_rx_task_h; /* ISR 태스크 알림 대상 */

/* ── 태스크 속성 ───────────────────────────────────────────────── */
static const osThreadAttr_t k_sensor_attr   = { .name="SensorTask",   .stack_size=512,  .priority=osPriorityNormal };
static const osThreadAttr_t k_control_attr  = { .name="ControlTask",  .stack_size=1536, .priority=osPriorityAboveNormal };
static const osThreadAttr_t k_uart_rx_attr  = { .name="UartRxTask",   .stack_size=768,  .priority=osPriorityAboveNormal };
static const osThreadAttr_t k_uart_tx_attr  = { .name="UartTxTask",   .stack_size=1024, .priority=osPriorityNormal };
static const osThreadAttr_t k_actuator_attr = { .name="ActuatorTask", .stack_size=384,  .priority=osPriorityBelowNormal };
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM3_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */
/* 헬퍼 함수 */
static void        uart_tx(const char *str);
static void        uart_enqueue(const char *str);
static void        heater_off(void);
static void        heater_set(uint32_t cmp);
static uint32_t    pid_update(float temp, float sp, uint32_t dt_ms);
static const char *state_str(void);
static int32_t     to_x10(float v);
static void        build_data_str(char *buf, size_t len);
static void        send_data(void);
static void        handle_cmd(char *line);

/* FreeRTOS 태스크 함수 */
static void SensorTask(void *arg);
static void ControlTask(void *arg);
static void UartRxTask(void *arg);
static void UartTxTask(void *arg);
static void ActuatorTask(void *arg);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ── UART RX ISR 콜백 ──────────────────────────────────────────── */
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
            /* vTaskNotifyGiveFromISR: ISR 우선순위에 관계없이 안전 */
            if (g_uart_rx_task_h != NULL) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                vTaskNotifyGiveFromISR((TaskHandle_t)g_uart_rx_task_h,
                                       &xHigherPriorityTaskWoken);
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            }
        }
        g_rx_idx = 0;
    } else if (g_rx_idx < RX_BUF_SIZE - 1) {
        g_rx_buf[g_rx_idx++] = c;
    }
    HAL_UART_Receive_IT(&huart2, &g_rx_byte, 1);
}

/* ── 직접 UART 송신 (UartTxTask 전용, ISR/task에서 비차단 전송) ── */
static void uart_tx(const char *str)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)str, strlen(str), 200);
}

/* ── TX 큐에 메시지 추가 (어느 태스크에서나 호출 가능) ──────────── */
static void uart_enqueue(const char *str)
{
    if (g_tx_queue == NULL) return;
    char msg[TX_MSG_SIZE];
    strncpy(msg, str, TX_MSG_SIZE - 1);
    msg[TX_MSG_SIZE - 1] = '\0';
    osMessageQueuePut(g_tx_queue, msg, 0, 0);  /* 큐 가득 찬 경우 드롭 */
}

/* ── 히터 제어 (g_state_mutex 보유 상태에서 호출) ──────────────── */
static void heater_off(void)
{
    g_heater_cmp = 0;
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, 0);
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_3);
    g_integral = 0.0f;
    g_prev_err = 0.0f;
}

static void heater_set(uint32_t cmp)
{
    if (cmp > 1000) cmp = 1000;
    g_heater_cmp = cmp;
    if (cmp == 0) { heater_off(); return; }
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, cmp);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);
}

/* ── PI 제어기 (g_state_mutex 보유 상태에서 호출) ──────────────── */
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

static int32_t to_x10(float v)
{
    if (v >= 0.0f) return (int32_t)(v * 10.0f + 0.5f);
    else           return (int32_t)(v * 10.0f - 0.5f);
}

/* ── DATA 프레임 문자열 생성 (g_state_mutex 보유 상태에서 호출) ── */
static void build_data_str(char *buf, size_t len)
{
    const char *alm = (g_state == STATE_WARNING) ? "ALM-01"
                    : (g_state == STATE_ALARM)   ? g_alm_id
                    : "NONE";

    int32_t  temp10    = to_x10(g_temp);
    int32_t  sp10      = to_x10(g_sp);
    uint32_t duty10    = g_heater_cmp;
    uint32_t elapsed10 = g_run_active ? (HAL_GetTick() - g_run_start_tick) / 100U : 0U;
    uint32_t reach10   = g_reach_ms / 100U;
    int32_t  peak10    = to_x10(g_peak_temp_after_reach);
    int32_t  os10      = to_x10(g_overshoot_c);
    uint32_t settle10  = g_settle_ms / 100U;

    snprintf(buf, len,
        "DATA:%ld.%01ld,%ld.%01ld,%s,%s,%lu.%01lu,%lu.%01lu,%u,%lu.%01lu,%ld.%01ld,%ld.%01ld,%u,%lu.%01lu\r\n",
        temp10/10, labs(temp10%10),
        sp10/10,   labs(sp10%10),
        state_str(), alm,
        (unsigned long)(duty10/10U),    (unsigned long)(duty10%10U),
        (unsigned long)(elapsed10/10U), (unsigned long)(elapsed10%10U),
        (unsigned int)g_target_reached,
        (unsigned long)(reach10/10U),   (unsigned long)(reach10%10U),
        peak10/10, labs(peak10%10),
        os10/10,   labs(os10%10),
        (unsigned int)g_settled,
        (unsigned long)(settle10/10U),  (unsigned long)(settle10%10U));
}

/* ── STATUS 응답용 DATA 전송 (g_state_mutex 보유 상태에서 호출) ── */
static void send_data(void)
{
    char buf[TX_MSG_SIZE];
    build_data_str(buf, sizeof(buf));
    uart_enqueue(buf);
}

/* ── EC → FW 명령 처리 (g_state_mutex 보유 상태에서 호출) ──────── */
static void handle_cmd(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
        line[--len] = '\0';
    if (len == 0) return;

    if (strcmp(line, "START") == 0) {
        if (g_state != STATE_IDLE) {
            uart_enqueue("NACK:START,NOT_IDLE\r\n");
        } else {
            g_state         = STATE_HEATING;
            g_integral      = 0.0f;
            g_prev_err      = 0.0f;
            g_in_warn_tmr   = 0;
            g_in_alarm_tmr  = 0;
            g_last_pid_tick = HAL_GetTick();
            strncpy(g_alm_id, "NONE", sizeof(g_alm_id));
            g_run_active     = 1;
            g_run_start_tick = g_last_pid_tick;
            g_target_reached = 0;
            g_reach_ms       = 0;
            g_peak_temp_after_reach = 0.0f;
            g_overshoot_c           = 0.0f;
            g_settled       = 0;
            g_settle_ms     = 0;
            g_in_band_count = 0;
#if TEST_FIXED_DUTY_ENABLE
            if (!g_temp_valid || g_temp < g_sp)
                heater_set(TEST_FIXED_DUTY_CMP);
            else
                heater_off();
#endif
            uart_enqueue("ACK:START\r\n");
        }

    } else if (strcmp(line, "STOP") == 0) {
        if (g_state == STATE_WARNING)
            uart_enqueue("EVENT:CLEAR,ALM-01\r\n");
        g_state = STATE_IDLE;
        heater_off();
        HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, FAN_OFF);
        g_in_warn_tmr   = 0;
        g_in_alarm_tmr  = 0;
        g_run_active     = 0;
        g_target_reached = 0;
        g_reach_ms       = 0;
        g_peak_temp_after_reach = 0.0f;
        g_overshoot_c           = 0.0f;
        g_settled       = 0;
        g_settle_ms     = 0;
        g_in_band_count = 0;
        uart_enqueue("ACK:STOP\r\n");

    } else if (strcmp(line, "RESET") == 0) {
        if (g_state != STATE_ALARM) {
            uart_enqueue("NACK:RESET,NOT_ALARM\r\n");
        } else if (!g_temp_valid) {
            uart_enqueue("NACK:RESET,NO_SENSOR\r\n");
        } else if (g_temp > (g_sp - RESET_THR)) {
            uart_enqueue("NACK:RESET,TEMP_HIGH\r\n");
        } else {
            strncpy(g_alm_id, "NONE", sizeof(g_alm_id));
            g_state = STATE_IDLE;
            heater_off();
            HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, FAN_OFF);
            g_in_warn_tmr   = 0;
            g_in_alarm_tmr  = 0;
            g_run_active     = 0;
            g_target_reached = 0;
            g_reach_ms       = 0;
            g_peak_temp_after_reach = 0.0f;
            g_overshoot_c           = 0.0f;
            g_settled       = 0;
            g_settle_ms     = 0;
            g_in_band_count = 0;
            uart_enqueue("ACK:RESET\r\n");
        }

    } else if (strncmp(line, "SET:", 4) == 0) {
        float new_sp = strtof(line + 4, NULL);
        if (new_sp >= 20.0f && new_sp <= 80.0f) {
            g_sp = new_sp;
            char buf[32];
            int32_t sp10 = to_x10(g_sp);
            snprintf(buf, sizeof(buf), "ACK:SET:%ld.%01ld\r\n", sp10/10, labs(sp10%10));
            uart_enqueue(buf);
        } else {
            uart_enqueue("NACK:SET,OUT_OF_RANGE\r\n");
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
  HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, FAN_OFF);
  HAL_GPIO_WritePin(Buzzer_GPIO_Port, Buzzer_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  g_last_pid_tick = HAL_GetTick();

  /* UART RX는 UartRxTask 시작 시점에서 활성화 (g_uart_rx_task_h 유효 보장) */
  uart_tx("BOOT:OK\r\n");
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  g_state_mutex = osMutexNew(NULL);
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  g_sensor_sem = osSemaphoreNew(1, 0, NULL);  /* 이진 세마포어, 초기값 0 */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  g_tx_queue = osMessageQueueNew(TX_QUEUE_LEN, TX_MSG_SIZE, NULL);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  osThreadNew(SensorTask,   NULL, &k_sensor_attr);
  osThreadNew(ControlTask,  NULL, &k_control_attr);
  g_uart_rx_task_h = osThreadNew(UartRxTask, NULL, &k_uart_rx_attr);
  osThreadNew(UartTxTask,   NULL, &k_uart_tx_attr);
  osThreadNew(ActuatorTask, NULL, &k_actuator_attr);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
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
  HAL_GPIO_WritePin(GPIOB, Buzzer_Pin|DHT22_DATA_Pin, GPIO_PIN_RESET);

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

  /*Configure GPIO pins : Buzzer_Pin DHT22_DATA_Pin */
  GPIO_InitStruct.Pin = Buzzer_Pin|DHT22_DATA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* ════════════════════════════════════════════════════════════════
 * SensorTask  (Normal priority, 2s 주기)
 * DHT22 읽기 → 공유 상태 갱신 → ControlTask 신호
 * ════════════════════════════════════════════════════════════════ */
static void SensorTask(void *arg)
{
    (void)arg;
    for (;;) {
        osDelay(DHT_INTERVAL_MS);

        /* vTaskSuspendAll: 다른 태스크의 선점을 막아 DHT22 타이밍 보호.
         * 인터럽트는 계속 동작하므로 HAL_Delay(내부 1ms)도 정상 작동. */
        DHT22_Data dht = {0};
        vTaskSuspendAll();
        DHT22_Status status = DHT22_Read(&dht);
        xTaskResumeAll();

        if (status == DHT22_OK) {
            osMutexAcquire(g_state_mutex, osWaitForever);
            g_temp        = dht.temperature;
            g_temp_valid  = 1;
            g_sensor_fail = 0;
            osMutexRelease(g_state_mutex);
        } else {
            osMutexAcquire(g_state_mutex, osWaitForever);
            g_sensor_fail++;
            uint8_t    fail = g_sensor_fail;
            ChamberState st   = g_state;
            osMutexRelease(g_state_mutex);

            if (fail >= SENSOR_FAIL_MAX && st != STATE_ALARM) {
                osMutexAcquire(g_state_mutex, osWaitForever);
                if (g_state != STATE_ALARM) {   /* 재진입 방지 */
                    g_temp_valid   = 0;
                    g_in_warn_tmr  = 0;
                    g_in_alarm_tmr = 0;
                    g_state        = STATE_ALARM;
                    strncpy(g_alm_id, "ALM-03", sizeof(g_alm_id) - 1);
                    g_alm_id[sizeof(g_alm_id) - 1] = '\0';
                    heater_off();
                    HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, FAN_ON);
                }
                osMutexRelease(g_state_mutex);
                uart_enqueue("EVENT:ALARM,ALM-03\r\n");
            }
        }

        osSemaphoreRelease(g_sensor_sem);  /* ControlTask 깨우기 */
    }
}

/* ════════════════════════════════════════════════════════════════
 * ControlTask  (AboveNormal priority, SensorTask 신호 대기)
 * PI 제어 + KPI 추적 + 알람 판정
 * ════════════════════════════════════════════════════════════════ */
static void ControlTask(void *arg)
{
    (void)arg;
    for (;;) {
        osSemaphoreAcquire(g_sensor_sem, osWaitForever);
        uint32_t now = HAL_GetTick();

        osMutexAcquire(g_state_mutex, osWaitForever);

        if (g_state != STATE_HEATING && g_state != STATE_WARNING) {
            osMutexRelease(g_state_mutex);
            continue;
        }

        /* ── PI 히터 출력 ──────────────────────────────────────── */
        uint32_t dt = now - g_last_pid_tick;
        if (dt == 0) dt = DHT_INTERVAL_MS;
        g_last_pid_tick = now;

#if TEST_FIXED_DUTY_ENABLE
#if STOP_HEATER_AT_TARGET
        if (g_target_reached || g_temp >= g_sp) heater_off();
        else heater_set(TEST_FIXED_DUTY_CMP);
#else
        if (g_temp < g_sp) heater_set(TEST_FIXED_DUTY_CMP);
        else heater_off();
#endif
#else
        heater_set(pid_update(g_temp, g_sp, dt));
#endif

        /* ── KPI: 목표 온도 최초 도달 ─────────────────────────── */
        if (g_run_active && !g_target_reached && g_temp_valid && g_temp >= g_sp) {
            g_target_reached        = 1;
            g_reach_ms              = now - g_run_start_tick;
            g_peak_temp_after_reach = g_temp;
            g_overshoot_c           = (g_temp > g_sp) ? (g_temp - g_sp) : 0.0f;

            char rbuf[96];
            int32_t t10 = to_x10(g_temp), s10 = to_x10(g_sp), os10 = to_x10(g_overshoot_c);
            snprintf(rbuf, sizeof(rbuf),
                "EVENT:TARGET_REACHED,%lu.%01lu,%ld.%01ld,%ld.%01ld,%ld.%01ld\r\n",
                (unsigned long)(g_reach_ms/1000U),
                (unsigned long)((g_reach_ms%1000U)/100U),
                t10/10, labs(t10%10), s10/10, labs(s10%10), os10/10, labs(os10%10));
            uart_enqueue(rbuf);
        }

        /* ── KPI: 오버슈트 갱신 ───────────────────────────────── */
        if (g_run_active && g_target_reached && g_temp_valid) {
            if (g_temp > g_peak_temp_after_reach) g_peak_temp_after_reach = g_temp;
            g_overshoot_c = g_peak_temp_after_reach - g_sp;
            if (g_overshoot_c < 0.0f) g_overshoot_c = 0.0f;
        }

        /* ── KPI: 안정화 시간 ─────────────────────────────────── */
        if (g_run_active && !g_settled && g_temp_valid) {
            float err_abs = g_temp - g_sp;
            if (err_abs < 0.0f) err_abs = -err_abs;
            if (err_abs <= SETTLE_BAND_C) {
                if (++g_in_band_count >= SETTLE_COUNT) {
                    g_settled   = 1;
                    g_settle_ms = now - g_run_start_tick;
                    char sbuf[48];
                    snprintf(sbuf, sizeof(sbuf), "EVENT:SETTLED,%lu.%01lu\r\n",
                        (unsigned long)(g_settle_ms/1000U),
                        (unsigned long)((g_settle_ms%1000U)/100U));
                    uart_enqueue(sbuf);
                }
            } else {
                g_in_band_count = 0;
            }
        }

        /* ── 알람 판정 ────────────────────────────────────────── */
        if (g_temp_valid) {
            float over = g_temp - g_sp;

            if (over >= ALARM_THR) {
                if (!g_in_warn_tmr)  { g_in_warn_tmr  = 1; g_warn_start  = now; }
                if (!g_in_alarm_tmr) { g_in_alarm_tmr = 1; g_alarm_start = now; }

                if (g_state == STATE_HEATING && (now - g_warn_start) >= WARN_DUR_MS) {
                    g_state    = STATE_WARNING;
                    g_integral = 0.0f;
                    HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, FAN_ON);
                    uart_enqueue("EVENT:WARN,ALM-01\r\n");
                }
                if ((now - g_alarm_start) >= ALARM_DUR_MS) {
                    if (g_state == STATE_WARNING)
                        uart_enqueue("EVENT:CLEAR,ALM-01\r\n");
                    /* ALARM 진입 */
                    g_in_warn_tmr  = 0;
                    g_in_alarm_tmr = 0;
                    g_state        = STATE_ALARM;
                    strncpy(g_alm_id, "ALM-02", sizeof(g_alm_id) - 1);
                    g_alm_id[sizeof(g_alm_id) - 1] = '\0';
                    heater_off();
                    HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, FAN_ON);
                    osMutexRelease(g_state_mutex);
                    uart_enqueue("EVENT:ALARM,ALM-02\r\n");
                    continue;  /* mutex 이미 해제 */
                }

            } else if (over >= WARN_THR) {
                g_in_alarm_tmr = 0;
                if (!g_in_warn_tmr) { g_in_warn_tmr = 1; g_warn_start = now; }
                if (g_state == STATE_HEATING && (now - g_warn_start) >= WARN_DUR_MS) {
                    g_state    = STATE_WARNING;
                    g_integral = 0.0f;
                    HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, FAN_ON);
                    uart_enqueue("EVENT:WARN,ALM-01\r\n");
                }

            } else {
                g_in_warn_tmr  = 0;
                g_in_alarm_tmr = 0;
                if (g_state == STATE_WARNING) {
                    g_state    = STATE_HEATING;
                    g_integral = 0.0f;
                    HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin, FAN_OFF);
                    uart_enqueue("EVENT:CLEAR,ALM-01\r\n");
                }
            }
        }

        osMutexRelease(g_state_mutex);
    }
}

/* ════════════════════════════════════════════════════════════════
 * UartRxTask  (AboveNormal priority)
 * ISR 태스크 알림 대기 → 명령 파싱 → 상태 반영
 * ════════════════════════════════════════════════════════════════ */
static void UartRxTask(void *arg)
{
    (void)arg;
    /* g_uart_rx_task_h 설정 완료 후 수신 시작 */
    HAL_UART_Receive_IT(&huart2, &g_rx_byte, 1);

    for (;;) {
        /* 100ms 타임아웃: 스케줄러 시작 전 수신된 라인도 처리 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (!g_line_ready) continue;

        char cmd[RX_BUF_SIZE];
        taskENTER_CRITICAL();
        memcpy(cmd, g_line, sizeof(cmd));
        g_line_ready = 0;
        taskEXIT_CRITICAL();

        osMutexAcquire(g_state_mutex, osWaitForever);
        handle_cmd(cmd);
        osMutexRelease(g_state_mutex);
    }
}

/* ════════════════════════════════════════════════════════════════
 * UartTxTask  (Normal priority)
 * TX 큐 수신 → 즉시 전송 / 1s 타임아웃 → 주기 DATA 전송
 * ════════════════════════════════════════════════════════════════ */
static void UartTxTask(void *arg)
{
    (void)arg;
    char msg[TX_MSG_SIZE];

    for (;;) {
        if (osMessageQueueGet(g_tx_queue, msg, NULL, DATA_TX_MS) == osOK) {
            /* 이벤트/응답 메시지 즉시 전송 */
            uart_tx(msg);
        } else {
            /* 1s 타임아웃: 주기 DATA 프레임 생성 후 직접 전송 */
            osMutexAcquire(g_state_mutex, osWaitForever);
            build_data_str(msg, sizeof(msg));
            osMutexRelease(g_state_mutex);
            uart_tx(msg);
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 * ActuatorTask  (BelowNormal priority, 50ms 주기)
 * 상태에 따라 부저 패턴 · LED 패턴 구동
 * ════════════════════════════════════════════════════════════════ */
static void ActuatorTask(void *arg)
{
    (void)arg;
    uint32_t buzz_tick = 0, led_tick = 0;
    uint8_t  buzz_on   = 0, led_on  = 0;

    for (;;) {
        osDelay(50);
        uint32_t now = HAL_GetTick();

        osMutexAcquire(g_state_mutex, osWaitForever);
        ChamberState st = g_state;
        osMutexRelease(g_state_mutex);

        /* ── 부저: ALARM=연속, WARNING=1s 토글, 그 외=OFF ──────── */
        if (st == STATE_ALARM) {
            HAL_GPIO_WritePin(Buzzer_GPIO_Port, Buzzer_Pin, GPIO_PIN_SET);
            buzz_on   = 1;
            buzz_tick = now;
        } else if (st == STATE_WARNING) {
            if ((now - buzz_tick) >= BUZZER_TOGGLE_MS) {
                buzz_tick = now;
                buzz_on   = !buzz_on;
                HAL_GPIO_WritePin(Buzzer_GPIO_Port, Buzzer_Pin,
                    buzz_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
            }
        } else {
            HAL_GPIO_WritePin(Buzzer_GPIO_Port, Buzzer_Pin, GPIO_PIN_RESET);
            buzz_on = 0;
        }

        /* ── LED: IDLE=OFF, HEATING=ON, WARNING=500ms, ALARM=200ms */
        if (st == STATE_IDLE) {
            HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
            led_on = 0;
        } else if (st == STATE_HEATING) {
            HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
            led_on = 1;
        } else {
            uint32_t blink = (st == STATE_ALARM) ? 200U : 500U;
            if ((now - led_tick) >= blink) {
                led_tick = now;
                led_on   = !led_on;
                HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin,
                    led_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
            }
        }
    }
}

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  osThreadExit();  /* 스택 반환 */
  /* USER CODE END 5 */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM14 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM14)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
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
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
