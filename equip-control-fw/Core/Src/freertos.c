/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : FreeRTOS 태스크 및 동기화 객체 구현
  ******************************************************************************
  */
/* USER CODE END Header */

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* USER CODE BEGIN Includes */
#include "config.h"
#include "protocol.h"
#include "uart_comm.h"
#include "watchdog_mgr.h"
#include <string.h>
/* USER CODE END Includes */

/* ── 태스크 핸들 ── */
osThreadId_t TaskWatchdogHandle;
osThreadId_t TaskCommHandle;
osThreadId_t TaskButtonHandle;
osThreadId_t TaskSensorHandle;
osThreadId_t TaskActuatorHandle;
osThreadId_t TaskHeartbeatHandle;

const osThreadAttr_t TaskWatchdog_attributes = {
  .name = "TaskWatchdog", .stack_size = 256 * 4,
  .priority = (osPriority_t)osPriorityRealtime,
};
const osThreadAttr_t TaskComm_attributes = {
  .name = "TaskComm", .stack_size = 512 * 4,
  .priority = (osPriority_t)osPriorityHigh,
};
const osThreadAttr_t TaskButton_attributes = {
  .name = "TaskButton", .stack_size = 128 * 4,
  .priority = (osPriority_t)osPriorityHigh,
};
const osThreadAttr_t TaskSensor_attributes = {
  .name = "TaskSensor", .stack_size = 256 * 4,
  .priority = (osPriority_t)osPriorityNormal,
};
const osThreadAttr_t TaskActuator_attributes = {
  .name = "TaskActuator", .stack_size = 256 * 4,
  .priority = (osPriority_t)osPriorityNormal,
};
const osThreadAttr_t TaskHeartbeat_attributes = {
  .name = "TaskHeartbeat", .stack_size = 128 * 4,
  .priority = (osPriority_t)osPriorityLow,
};

/* ── 태스크 간 동기화 객체 (SDD Section 3.2.2) ── */

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

/* ── 함수 선언 ── */
void StartTaskWatchdog(void *argument);
void StartTaskComm(void *argument);
void StartTaskButton(void *argument);
void StartTaskSensor(void *argument);
void StartTaskActuator(void *argument);
void StartTaskHeartbeat(void *argument);

void MX_FREERTOS_Init(void);

/* =========================================================================
 * MX_FREERTOS_Init — 큐/뮤텍스 생성 및 태스크 생성
 * ======================================================================= */
void MX_FREERTOS_Init(void)
{
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

  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  /* 태스크 생성 */
  TaskWatchdogHandle = osThreadNew(StartTaskWatchdog, NULL, &TaskWatchdog_attributes);
  TaskCommHandle     = osThreadNew(StartTaskComm,     NULL, &TaskComm_attributes);
  TaskButtonHandle   = osThreadNew(StartTaskButton,   NULL, &TaskButton_attributes);
  TaskSensorHandle   = osThreadNew(StartTaskSensor,   NULL, &TaskSensor_attributes);
  TaskActuatorHandle = osThreadNew(StartTaskActuator, NULL, &TaskActuator_attributes);
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
}

/* =========================================================================
 * TaskWatchdog — 우선순위 최고 (osPriorityRealtime)
 * 100ms마다 각 태스크 체크인 감시 + IWDG 피드 (FR-021~024)
 * ======================================================================= */
void StartTaskWatchdog(void *argument)
{
  /* IWDG 초기화 (FR-021) */
  watchdog_iwdg_init();

  for (;;) {
    watchdog_monitor();
    osDelay(CFG_WATCHDOG_POLL_MS);
  }
}

/* =========================================================================
 * TaskComm — 우선순위 높음 (osPriorityHigh)
 * RX 큐에서 바이트를 꺼내 프레임 파서에 투입.
 * 유효 프레임: ACK 전송 후 명령 큐에 삽입.
 * CRC 불일치: NACK 전송.
 * TX 큐에 있는 프레임을 꺼내 UART로 전송 + ACK 대기/재전송 (FR-012~017)
 * ======================================================================= */
void StartTaskComm(void *argument)
{
  /* UART RX 인터럽트 시작 */
  uart_rx_start();

  watchdog_register(TASK_ID_COMM, TaskCommHandle,
                    StartTaskComm, CFG_WD_TIMEOUT_COMM_MS);

  static uint8_t txSeq = 0U;

  osMessageQueueId_t rxByteQ = uart_get_rx_queue();

  for (;;) {
    watchdog_checkin(TASK_ID_COMM);

    /* ── RX 처리: 큐에서 바이트 꺼내 파서에 투입 ── */
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

        /* 명령 프레임이면 RxCmd 큐에 삽입 */
        if (frame.type == MSG_CMD_FAN    ||
            frame.type == MSG_CMD_BUZZER ||
            frame.type == MSG_CMD_LED    ||
            frame.type == MSG_CMD_STATE_SYNC) {
          osMessageQueuePut(xQueueRxCmd, &frame, 0U, 0U);
        }
        /* Heartbeat REQ에 ACK 응답 */
        if (frame.type == MSG_HEARTBEAT_REQ) {
          uint8_t hbBuf[PROTO_MAX_FRAME];
          uint16_t hbLen = frame_build(MSG_HEARTBEAT_ACK, txSeq++,
                                       NULL, 0U, hbBuf);
          uart_transmit_raw(hbBuf, hbLen);
        }
      }
      /* CRC 불일치: frame_parser_feed가 false 반환 — NACK는 생략
       * (파서가 이미 버렸으므로 EC 재전송 타임아웃으로 처리) */
    }

    /* ── TX 처리: TX 큐에서 요청 꺼내 전송 + ACK 대기 ── */
    TxRequest_t req;
    if (osMessageQueueGet(xQueueTxFrame, &req, NULL, 0U) == osOK) {
      uint8_t txBuf[PROTO_MAX_FRAME];
      uint16_t len = frame_build(req.type, txSeq,
                                 req.payload, req.payloadLen, txBuf);
      uint8_t retries = 0U;
      bool acked = false;

      while (retries < CFG_MAX_RETRANSMIT && !acked) {
        uart_transmit_raw(txBuf, len);

        /* ACK 대기 (1초) — RX 큐에서 ACK 프레임 확인 */
        uint32_t deadline = osKernelGetTickCount() + CFG_TX_TIMEOUT_MS;
        while (osKernelGetTickCount() < deadline) {
          uint8_t b;
          if (osMessageQueueGet(rxByteQ, &b, NULL, 10U) == osOK) {
            ParsedFrame_t ackFrame;
            if (frame_parser_feed(b, &ackFrame)) {
              if (ackFrame.type == MSG_ACK &&
                  ackFrame.payload[0] == txSeq) {
                acked = true;
                break;
              }
            }
          }
        }
        retries++;
      }
      /* 3회 모두 미수신 → 통신 오류 (EC에서 ALID-4로 처리) */
      txSeq++;
    }

    osDelay(1U);
  }
}

/* =========================================================================
 * TaskSensor — 우선순위 보통 (osPriorityNormal)
 * 1초 주기로 센서 수집 후 TX 큐에 삽입 (FR-001~004, FR-009)
 * 지금은 더미값 — 센서 연결 후 드라이버 호출로 교체
 * ======================================================================= */
void StartTaskSensor(void *argument)
{
  watchdog_register(TASK_ID_SENSOR, TaskSensorHandle,
                    StartTaskSensor, CFG_WD_TIMEOUT_SENSOR_MS);

  for (;;) {
    watchdog_checkin(TASK_ID_SENSOR);

    /* ── 더미 센서 데이터 생성 (실제 센서 연결 시 교체) ── */
    SensorDataPayload_t data;
    data.flags       = 0x03U;   /* bit0=sht31_valid, bit1=ina219_valid */
    data.sht31_error  = SENSOR_ERR_NONE;
    data.ina219_error = SENSOR_ERR_NONE;
    data.temperature  = 25.0f;  /* °C — 더미 */
    data.humidity     = 50.0f;  /* %  — 더미 */
    data.current_mA   = 100.0f; /* mA — 더미 */
    data.voltage_V    = 3.3f;   /* V  — 더미 */
    data.timestamp_ms = osKernelGetTickCount();

    /* 공유 버퍼 갱신 */
    if (osMutexAcquire(xMutexSensorBuf, 10U) == osOK) {
      g_sensorBuf = data;
      osMutexRelease(xMutexSensorBuf);
    }

    /* TX 큐에 삽입 */
    TxRequest_t req;
    req.type       = MSG_SENSOR_DATA;
    req.payloadLen = (uint16_t)sizeof(data);
    memcpy(req.payload, &data, sizeof(data));
    osMessageQueuePut(xQueueTxFrame, &req, 0U, 0U);

    osDelay(CFG_SENSOR_PERIOD_MS);
  }
}

/* =========================================================================
 * TaskActuator — 우선순위 보통 (osPriorityNormal)
 * RxCmd 큐에서 EC 명령을 꺼내 GPIO 제어 (FR-005~007, FR-011)
 * 지금은 내장 LED(PA5)로 동작 확인 — 실제 핀 연결 후 교체
 * ======================================================================= */
void StartTaskActuator(void *argument)
{
  watchdog_register(TASK_ID_ACTUATOR, TaskActuatorHandle,
                    StartTaskActuator, CFG_WD_TIMEOUT_ACTUATOR_MS);

  for (;;) {
    watchdog_checkin(TASK_ID_ACTUATOR);

    ParsedFrame_t frame;
    if (osMessageQueueGet(xQueueRxCmd, &frame, NULL, 100U) == osOK) {
      switch (frame.type) {
      case MSG_CMD_FAN: {
        CmdFanPayload_t *p = (CmdFanPayload_t *)frame.payload;
        /* TODO: 팬 GPIO 제어 — 지금은 LD2 LED로 대체 */
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
                          p->on ? GPIO_PIN_SET : GPIO_PIN_RESET);
        break;
      }
      case MSG_CMD_BUZZER:
        /* TODO: 부저 GPIO 제어 */
        break;
      case MSG_CMD_LED: {
        CmdLedPayload_t *p = (CmdLedPayload_t *)frame.payload;
        /* TODO: 다색 LED 제어 — 지금은 상태 코드만 저장 */
        (void)p;
        break;
      }
      default:
        break;
      }
    }
  }
}

/* =========================================================================
 * TaskButton — 우선순위 높음 (osPriorityHigh)
 * 버튼 이벤트 큐에서 꺼내 TX 큐에 삽입 (FR-008, FR-010)
 * 실제 버튼 인터럽트는 gpio_button_event_from_isr()로 큐에 삽입
 * ======================================================================= */
void StartTaskButton(void *argument)
{
  watchdog_register(TASK_ID_BUTTON, TaskButtonHandle,
                    StartTaskButton, CFG_WD_TIMEOUT_BUTTON_MS);

  for (;;) {
    watchdog_checkin(TASK_ID_BUTTON);

    ButtonEventPayload_t evt;
    if (osMessageQueueGet(xQueueButtonEvent, &evt, NULL, 100U) == osOK) {
      TxRequest_t req;
      req.type       = MSG_BUTTON_EVENT;
      req.payloadLen = (uint16_t)sizeof(evt);
      memcpy(req.payload, &evt, sizeof(evt));
      osMessageQueuePut(xQueueTxFrame, &req, 0U, 0U);
    }
  }
}

/* =========================================================================
 * TaskHeartbeat — 우선순위 낮음 (osPriorityLow)
 * 5초 주기로 HEARTBEAT_REQ 전송 (FR-017)
 * ======================================================================= */
void StartTaskHeartbeat(void *argument)
{
  watchdog_register(TASK_ID_HEARTBEAT, TaskHeartbeatHandle,
                    StartTaskHeartbeat, CFG_WD_TIMEOUT_HB_MS);

  for (;;) {
    watchdog_checkin(TASK_ID_HEARTBEAT);

    TxRequest_t req;
    req.type       = MSG_HEARTBEAT_REQ;
    req.payloadLen = 0U;
    osMessageQueuePut(xQueueTxFrame, &req, 0U, 0U);

    osDelay(CFG_HEARTBEAT_PERIOD_MS);
  }
}

/* ── 버튼 ISR에서 호출 — xQueueButtonEvent에 삽입 ── */
void gpio_button_event_from_isr(uint8_t buttonId)
{
  ButtonEventPayload_t evt = { .button_id = buttonId };
  osMessageQueuePut(xQueueButtonEvent, &evt, 0U, 0U);
}

/* USER CODE BEGIN Application */
/* USER CODE END Application */
