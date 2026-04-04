/* -----------------------------------------------------------------------
 * watchdog_mgr.c — Task Watchdog + IWDG 관리 (FR-021~024, SDD 3.2.3)
 *
 * 동작 원리:
 *   각 태스크는 루프 상단에서 watchdog_checkin(id)를 호출한다.
 *   TaskWatchdog이 100ms마다 watchdog_monitor()를 호출하여
 *   체크인 타임스탬프를 검사한다.
 *   타임아웃된 태스크는 재시작하고 EC에 RESTART_REASON을 보고한다.
 *   모든 태스크가 정상이면 IWDG를 피드(리셋)한다.
 *   TaskWatchdog 자신이 hang되면 IWDG 피드가 끊겨 MCU 전체가 재시작된다.
 * --------------------------------------------------------------------- */

#include "watchdog_mgr.h"
#include "config.h"
#include "protocol.h"
#include "cmsis_os.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* ── IWDG 핸들 ── */
static IWDG_HandleTypeDef s_hiwdg;

/* ── 태스크 등록 테이블 ── */
typedef struct {
    osThreadId_t   handle;
    osThreadFunc_t entry;
    uint32_t       timeoutMs;
    uint32_t       lastCheckinMs;
    bool           registered;
} TaskWdEntry_t;

static TaskWdEntry_t s_tasks[TASK_ID_COUNT];
static uint8_t       s_restartCount = 0U;

/* TX 큐 핸들 — freertos.c에서 초기화 후 설정 (RESTART_REASON 전송용) */
static osMessageQueueId_t s_txQueue = NULL;

void watchdog_set_tx_queue(osMessageQueueId_t q)
{
    s_txQueue = q;
}

/* ── IWDG 초기화 (FR-021) ──
 * LSI(32kHz), prescaler=256, reload=999
 * timeout = (reload+1) / (32000 / 256) ≈ 8초 */
void watchdog_iwdg_init(void)
{
    s_hiwdg.Instance       = IWDG;
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
    s_hiwdg.Init.Reload    = 999U;
    HAL_IWDG_Init(&s_hiwdg);
}

/* ── 태스크 등록 ── */
void watchdog_register(TaskWdId_t id, osThreadId_t handle,
                       osThreadFunc_t entry, uint32_t timeoutMs)
{
    if (id >= TASK_ID_COUNT) return;
    s_tasks[id].handle        = handle;
    s_tasks[id].entry         = entry;
    s_tasks[id].timeoutMs     = timeoutMs;
    s_tasks[id].lastCheckinMs = osKernelGetTickCount();
    s_tasks[id].registered    = true;
}

/* ── 체크인 — 각 태스크 루프 상단에서 호출 ── */
void watchdog_checkin(TaskWdId_t id)
{
    if (id < TASK_ID_COUNT)
        s_tasks[id].lastCheckinMs = osKernelGetTickCount();
}

/* ── 모니터링 — TaskWatchdog 루프에서 100ms마다 호출 ── */
void watchdog_monitor(void)
{
    uint32_t now   = osKernelGetTickCount();
    bool     allOk = true;

    for (uint8_t i = 0U; i < TASK_ID_COUNT; i++) {
        if (!s_tasks[i].registered) continue;

        uint32_t elapsed = now - s_tasks[i].lastCheckinMs;
        if (elapsed > s_tasks[i].timeoutMs) {
            /* 해당 태스크만 재시작 (FR-022, FR-027) */
            osThreadTerminate(s_tasks[i].handle);
            s_tasks[i].handle        = osThreadNew(s_tasks[i].entry, NULL, NULL);
            s_tasks[i].lastCheckinMs = now;
            s_restartCount++;
            allOk = false;

            /* EC에 재시작 원인 보고 (FR-024) */
            if (s_txQueue != NULL) {
                RestartReasonPayload_t reason;
                reason.reason_code    = RESTART_TASK_WD;
                reason.failed_task_id = i;
                reason.restart_count  = s_restartCount;

                TxRequest_t req;
                req.type       = MSG_RESTART_REASON;
                req.payloadLen = (uint16_t)sizeof(reason);
                memcpy(req.payload, &reason, sizeof(reason));
                osMessageQueuePut(s_txQueue, &req, 0U, 0U);
            }
        }
    }

    /* 모든 태스크 정상 → IWDG 피드 (FR-021)
     * 하나라도 타임아웃이면 피드 생략 → IWDG 만료 → MCU 전체 재시작 */
    if (allOk)
        HAL_IWDG_Refresh(&s_hiwdg);
}

uint8_t watchdog_get_restart_count(void)
{
    return s_restartCount;
}
