/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : FreeRTOS 태스크 및 동기화 객체 구현
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "config.h"
#include "protocol.h"
#include "uart_comm.h"
#include "watchdog_mgr.h"
#include "gpio.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* TX 프레임 큐: TaskSensor/Button/Heartbeat → TaskComm */
static osMessageQueueId_t xQueueTxFrame;

/* RX 명령 큐: TaskComm → TaskActuator */
static osMessageQueueId_t xQueueRxCmd;

/* 버튼 이벤트 큐: TaskButton → TaskComm */
static osMessageQueueId_t xQueueButtonEvent;

/* 공유 센서 버퍼 보호 Mutex */
static osMutexId_t xMutexSensorBuf;

/* 공유 센서 버퍼 (더미값 — 실제 센서 연결 후 교체) */
static SensorDataPayload_t g_sensorBuf;

/* USER CODE END Variables */
/* Definitions for TaskWatchdog */
osThreadId_t TaskWatchdogHandle;
const osThreadAttr_t TaskWatchdog_attributes = {
  .name = "TaskWatchdog",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityRealtime,
};
/* Definitions for TaskComm */
osThreadId_t TaskCommHandle;
const osThreadAttr_t TaskComm_attributes = {
  .name = "TaskComm",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for TaskButton */
osThreadId_t TaskButtonHandle;
const osThreadAttr_t TaskButton_attributes = {
  .name = "TaskButton",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for TaskSensor */
osThreadId_t TaskSensorHandle;
const osThreadAttr_t TaskSensor_attributes = {
  .name = "TaskSensor",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for TaskActuator */
osThreadId_t TaskActuatorHandle;
const osThreadAttr_t TaskActuator_attributes = {
  .name = "TaskActuator",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for TaskHeartbeat */
osThreadId_t TaskHeartbeatHandle;
const osThreadAttr_t TaskHeartbeat_attributes = {
  .name = "TaskHeartbeat",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartTaskWatchdog(void *argument);
void StartTaskComm(void *argument);
void StartTaskButton(void *argument);
void StartTaskSensor(void *argument);
void StartTaskActuator(void *argument);
void StartTaskHeartbeat(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* 큐 생성 */
  xQueueTxFrame    = osMessageQueueNew(CFG_QUEUE_TX_SIZE,
                                       sizeof(TxRequest_t), NULL);
  xQueueRxCmd      = osMessageQueueNew(CFG_QUEUE_RX_CMD_SIZE,
                                       sizeof(ParsedFrame_t), NULL);
  xQueueButtonEvent = osMessageQueueNew(CFG_QUEUE_BUTTON_SIZE,
                                        sizeof(ButtonEventPayload_t), NULL);
  xMutexSensorBuf  = osMutexNew(NULL);

  /* Task Watchdog에 TX 큐 전달 (RESTART_REASON 보고용) */
  watchdog_set_tx_queue(xQueueTxFrame);

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of TaskWatchdog */
  TaskWatchdogHandle = osThreadNew(StartTaskWatchdog, NULL, &TaskWatchdog_attributes);

  /* creation of TaskComm */
  TaskCommHandle = osThreadNew(StartTaskComm, NULL, &TaskComm_attributes);

  /* creation of TaskButton */
  TaskButtonHandle = osThreadNew(StartTaskButton, NULL, &TaskButton_attributes);

  /* creation of TaskSensor */
  TaskSensorHandle = osThreadNew(StartTaskSensor, NULL, &TaskSensor_attributes);

  /* creation of TaskActuator */
  TaskActuatorHandle = osThreadNew(StartTaskActuator, NULL, &TaskActuator_attributes);

  /* creation of TaskHeartbeat */
  TaskHeartbeatHandle = osThreadNew(StartTaskHeartbeat, NULL, &TaskHeartbeat_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* Task Watchdog 등록 (FR-022) */
  watchdog_register(TASK_ID_SENSOR,    TaskSensorHandle,
                    StartTaskSensor,    CFG_WD_TIMEOUT_SENSOR_MS);
  watchdog_register(TASK_ID_COMM,      TaskCommHandle,
                    StartTaskComm,      CFG_WD_TIMEOUT_COMM_MS);
  watchdog_register(TASK_ID_BUTTON,    TaskButtonHandle,
                    StartTaskButton,    CFG_WD_TIMEOUT_BUTTON_MS);
  watchdog_register(TASK_ID_ACTUATOR,  TaskActuatorHandle,
                    StartTaskActuator,  CFG_WD_TIMEOUT_ACTUATOR_MS);
  watchdog_register(TASK_ID_HEARTBEAT, TaskHeartbeatHandle,
                    StartTaskHeartbeat, CFG_WD_TIMEOUT_HB_MS);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartTaskWatchdog */
/**
  * @brief  Function implementing the TaskWatchdog thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartTaskWatchdog */
void StartTaskWatchdog(void *argument)
{
  /* USER CODE BEGIN StartTaskWatchdog */
  watchdog_iwdg_init();
  for(;;)
  {
    watchdog_monitor();
    osDelay(CFG_WATCHDOG_POLL_MS);
  }
  /* USER CODE END StartTaskWatchdog */
}

/* USER CODE BEGIN Header_StartTaskComm */
/**
* @brief Function implementing the TaskComm thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskComm */
void StartTaskComm(void *argument)
{
  /* USER CODE BEGIN StartTaskComm */
  uart_rx_start();
  watchdog_checkin(TASK_ID_COMM);

  static uint8_t txSeq = 0U;
  osMessageQueueId_t rxByteQ = uart_get_rx_queue();

  for(;;)
  {
    watchdog_checkin(TASK_ID_COMM);

    /* ── RX: 바이트 꺼내 파서 투입 ── */
    uint8_t byte;
    while (osMessageQueueGet(rxByteQ, &byte, NULL, 0U) == osOK) {
      ParsedFrame_t frame;
      bool crcOk = frame_parser_feed(byte, &frame);
      if (crcOk) {
        /* ACK 전송 */
        AckPayload_t ack = { .ack_seq = frame.seq };
        uint8_t txBuf[PROTO_MAX_FRAME];
        uint16_t len = frame_build(MSG_ACK, txSeq++,
                                   (uint8_t *)&ack, sizeof(ack), txBuf);
        uart_transmit_raw(txBuf, len);

        /* 명령 프레임 → RxCmd 큐 */
        if (frame.type == MSG_CMD_FAN    ||
            frame.type == MSG_CMD_BUZZER ||
            frame.type == MSG_CMD_LED    ||
            frame.type == MSG_CMD_STATE_SYNC) {
          osMessageQueuePut(xQueueRxCmd, &frame, 0U, 0U);
        }
        /* Heartbeat REQ → ACK 응답 */
        if (frame.type == MSG_HEARTBEAT_REQ) {
          uint8_t hbBuf[PROTO_MAX_FRAME];
          uint16_t hbLen = frame_build(MSG_HEARTBEAT_ACK, txSeq++,
                                       NULL, 0U, hbBuf);
          uart_transmit_raw(hbBuf, hbLen);
        }
      }
    }

    /* ── TX: 큐에서 요청 꺼내 전송 + ACK 대기/재전송 ── */
    TxRequest_t req;
    if (osMessageQueueGet(xQueueTxFrame, &req, NULL, 0U) == osOK) {
      uint8_t  txBuf[PROTO_MAX_FRAME];
      uint16_t len;
      uint8_t  retries = 0U;
      uint8_t  acked   = 0U;
      uint32_t deadline;

      len = frame_build(req.type, txSeq,
                        req.payload, req.payloadLen, txBuf);

      while (retries < CFG_MAX_RETRANSMIT && acked == 0U) {
        uart_transmit_raw(txBuf, len);
        deadline = osKernelGetTickCount() + CFG_TX_TIMEOUT_MS;
        while (osKernelGetTickCount() < deadline) {
          uint8_t b;
          if (osMessageQueueGet(rxByteQ, &b, NULL, 10U) == osOK) {
            ParsedFrame_t ackFrame;
            if (frame_parser_feed(b, &ackFrame)) {
              if (ackFrame.type == MSG_ACK &&
                  ackFrame.payload[0] == txSeq) {
                acked = 1U;
                break;
              }
            }
          }
        }
        retries++;
      }
      txSeq++;
    }

    osDelay(1U);
  }
  /* USER CODE END StartTaskComm */
}

/* USER CODE BEGIN Header_StartTaskButton */
/**
* @brief Function implementing the TaskButton thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskButton */
void StartTaskButton(void *argument)
{
  /* USER CODE BEGIN StartTaskButton */
  watchdog_checkin(TASK_ID_BUTTON);
  for(;;)
  {
    ButtonEventPayload_t evt;
    watchdog_checkin(TASK_ID_BUTTON);
    if (osMessageQueueGet(xQueueButtonEvent, &evt, NULL, 100U) == osOK) {
      TxRequest_t req;
      req.type       = MSG_BUTTON_EVENT;
      req.payloadLen = (uint16_t)sizeof(evt);
      memcpy(req.payload, &evt, sizeof(evt));
      osMessageQueuePut(xQueueTxFrame, &req, 0U, 0U);
    }
  }
  /* USER CODE END StartTaskButton */
}

/* USER CODE BEGIN Header_StartTaskSensor */
/**
* @brief Function implementing the TaskSensor thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskSensor */
void StartTaskSensor(void *argument)
{
  /* USER CODE BEGIN StartTaskSensor */
  watchdog_checkin(TASK_ID_SENSOR);
  for(;;)
  {
    TxRequest_t req;
    SensorDataPayload_t data;

    watchdog_checkin(TASK_ID_SENSOR);

    /* 더미 센서 데이터 — 실제 센서 연결 후 드라이버 호출로 교체 */
    data.flags        = 0x03U;
    data.sht31_error  = SENSOR_ERR_NONE;
    data.ina219_error = SENSOR_ERR_NONE;
    data.temperature  = 25.0f;
    data.humidity     = 50.0f;
    data.current_mA   = 100.0f;
    data.voltage_V    = 3.3f;
    data.timestamp_ms = osKernelGetTickCount();

    /* 공유 버퍼 갱신 */
    if (osMutexAcquire(xMutexSensorBuf, 10U) == osOK) {
      g_sensorBuf = data;
      osMutexRelease(xMutexSensorBuf);
    }

    /* TX 큐에 삽입 */
    req.type       = MSG_SENSOR_DATA;
    req.payloadLen = (uint16_t)sizeof(data);
    memcpy(req.payload, &data, sizeof(data));
    osMessageQueuePut(xQueueTxFrame, &req, 0U, 0U);

    osDelay(CFG_SENSOR_PERIOD_MS);
  }
  /* USER CODE END StartTaskSensor */
}

/* USER CODE BEGIN Header_StartTaskActuator */
/**
* @brief Function implementing the TaskActuator thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskActuator */
void StartTaskActuator(void *argument)
{
  /* USER CODE BEGIN StartTaskActuator */
  watchdog_checkin(TASK_ID_ACTUATOR);
  for(;;)
  {
    ParsedFrame_t frame;
    watchdog_checkin(TASK_ID_ACTUATOR);
    if (osMessageQueueGet(xQueueRxCmd, &frame, NULL, 100U) == osOK) {
      switch (frame.type) {
      case MSG_CMD_FAN: {
        CmdFanPayload_t *p = (CmdFanPayload_t *)frame.payload;
        /* TODO: 팬 GPIO — 현재 내장 LD2(PA5)로 대체 */
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
                          p->on ? GPIO_PIN_SET : GPIO_PIN_RESET);
        break;
      }
      case MSG_CMD_BUZZER:
        /* TODO: 부저 GPIO 제어 */
        break;
      case MSG_CMD_LED:
        /* TODO: 다색 LED 제어 */
        break;
      default:
        break;
      }
    }
  }
  /* USER CODE END StartTaskActuator */
}

/* USER CODE BEGIN Header_StartTaskHeartbeat */
/**
* @brief Function implementing the TaskHeartbeat thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskHeartbeat */
void StartTaskHeartbeat(void *argument)
{
  /* USER CODE BEGIN StartTaskHeartbeat */
  watchdog_checkin(TASK_ID_HEARTBEAT);
  for(;;)
  {
    TxRequest_t req;
    watchdog_checkin(TASK_ID_HEARTBEAT);
    req.type       = MSG_HEARTBEAT_REQ;
    req.payloadLen = 0U;
    osMessageQueuePut(xQueueTxFrame, &req, 0U, 0U);
    osDelay(CFG_HEARTBEAT_PERIOD_MS);
  }
  /* USER CODE END StartTaskHeartbeat */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* 버튼 ISR(HAL_GPIO_EXTI_Callback)에서 호출 — xQueueButtonEvent에 삽입 */
void gpio_button_event_from_isr(uint8_t buttonId)
{
  ButtonEventPayload_t evt;
  evt.button_id = buttonId;
  osMessageQueuePut(xQueueButtonEvent, &evt, 0U, 0U);
}

/* USER CODE END Application */

