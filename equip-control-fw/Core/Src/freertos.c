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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
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

