#ifndef __WATCHDOG_MGR_H
#define __WATCHDOG_MGR_H

/* -----------------------------------------------------------------------
 * watchdog_mgr.h — Task Watchdog 체크인 인터페이스 (FR-022)
 * 각 태스크는 루프 시작 시 watchdog_checkin()을 호출한다.
 * TaskWatchdog이 주기적으로 체크인 타임스탬프를 검사한다.
 * --------------------------------------------------------------------- */

#include <stdint.h>
#include "cmsis_os.h"

typedef enum {
    TASK_ID_SENSOR    = 0,
    TASK_ID_COMM      = 1,
    TASK_ID_BUTTON    = 2,
    TASK_ID_ACTUATOR  = 3,
    TASK_ID_HEARTBEAT = 4,
    TASK_ID_COUNT     = 5
} TaskWdId_t;

/* 태스크 등록 — 스케줄러 시작 후 각 태스크에서 1회 호출 */
void watchdog_register(TaskWdId_t id, osThreadId_t handle,
                       osThreadFunc_t entry, uint32_t timeoutMs);

/* 체크인 — 각 태스크 루프 상단에서 호출 (FR-022) */
void watchdog_checkin(TaskWdId_t id);

/* TaskWatchdog 루프에서 호출: 타임아웃 태스크 재시작, IWDG 피드 */
void watchdog_monitor(void);

/* 재시작 횟수 조회 (FR-033 ERROR 판정용) */
uint8_t watchdog_get_restart_count(void);

/* IWDG 초기화 — main.c에서 스케줄러 시작 전 1회 호출 (FR-021) */
void watchdog_iwdg_init(void);

/* TX 큐 설정 — freertos.c에서 큐 생성 후 1회 호출 */
void watchdog_set_tx_queue(osMessageQueueId_t q);

#endif /* __WATCHDOG_MGR_H */
