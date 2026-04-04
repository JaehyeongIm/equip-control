#ifndef __CONFIG_H
#define __CONFIG_H

/* -----------------------------------------------------------------------
 * config.h — 펌웨어 설정값 중앙 관리 (NFR-017)
 * 모든 주기, 임계값, 크기는 여기서만 수정한다.
 * --------------------------------------------------------------------- */

/* 타이밍 (ms) */
#define CFG_SENSOR_PERIOD_MS        1000U
#define CFG_HEARTBEAT_PERIOD_MS     5000U
#define CFG_TX_TIMEOUT_MS           1000U
#define CFG_WATCHDOG_POLL_MS        100U

/* 재전송 (FR-015) */
#define CFG_MAX_RETRANSMIT          3U

/* Task Watchdog 타임아웃: 태스크 주기 × 3 (FR-022) */
#define CFG_WD_TIMEOUT_SENSOR_MS    (CFG_SENSOR_PERIOD_MS    * 3U)
#define CFG_WD_TIMEOUT_COMM_MS      3000U
#define CFG_WD_TIMEOUT_BUTTON_MS    3000U
#define CFG_WD_TIMEOUT_ACTUATOR_MS  3000U
#define CFG_WD_TIMEOUT_HB_MS        (CFG_HEARTBEAT_PERIOD_MS * 3U)

/* 하드웨어 IWDG 타임아웃 (ms) — Task Watchdog 최대값보다 커야 함 (FR-021) */
#define CFG_IWDG_TIMEOUT_MS         8000U

/* 센서 유효 범위 (FR-004) */
#define CFG_TEMP_MIN_C              (-40.0f)
#define CFG_TEMP_MAX_C              (125.0f)
#define CFG_HUMI_MIN_PCT            (0.0f)
#define CFG_HUMI_MAX_PCT            (100.0f)
#define CFG_CURR_MIN_MA             (0.0f)
#define CFG_CURR_MAX_MA             (3200.0f)
#define CFG_VOLT_MIN_V              (0.0f)
#define CFG_VOLT_MAX_V              (26.0f)

/* 큐 크기 */
#define CFG_QUEUE_UART_RX_SIZE      64U
#define CFG_QUEUE_TX_SIZE           8U
#define CFG_QUEUE_RX_CMD_SIZE       4U
#define CFG_QUEUE_BUTTON_SIZE       4U

#endif /* __CONFIG_H */
