#ifndef __PROTOCOL_H
#define __PROTOCOL_H

/* -----------------------------------------------------------------------
 * protocol.h — STM32↔EC 내부 프로토콜 정의 (SDD Section 5)
 *
 * 프레임 구조:
 *   [SOF(1)] [TYPE(1)] [SEQ(1)] [LEN_L(1)] [LEN_H(1)] [PAYLOAD(N)] [CRC_L(1)] [CRC_H(1)]
 *   CRC16-CCITT 계산 범위: TYPE ~ PAYLOAD 끝
 * --------------------------------------------------------------------- */

#include <stdint.h>
#include <stdbool.h>

/* 프레임 상수 */
#define PROTO_SOF               0xAAU
#define PROTO_HEADER_SIZE       5U      /* SOF + TYPE + SEQ + LEN(2) */
#define PROTO_CRC_SIZE          2U
#define PROTO_MAX_PAYLOAD       64U
#define PROTO_MAX_FRAME         (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_CRC_SIZE)

/* ── 메시지 타입 (STM32→EC) ── */
#define MSG_SENSOR_DATA         0x01U
#define MSG_BUTTON_EVENT        0x02U
#define MSG_RESTART_REASON      0x03U
#define MSG_ACK                 0x04U
#define MSG_NACK                0x05U

/* ── 메시지 타입 (EC→STM32) ── */
#define MSG_CMD_FAN             0x10U
#define MSG_CMD_BUZZER          0x11U
#define MSG_CMD_LED             0x12U
#define MSG_CMD_STATE_SYNC      0x13U
#define MSG_CMD_HEATER          0x14U
#define MSG_HEARTBEAT_REQ       0x20U
#define MSG_HEARTBEAT_ACK       0x21U

/* LED 상태 코드 (FR-007) */
#define LED_STATE_IDLE          0x01U
#define LED_STATE_RUN           0x02U
#define LED_STATE_ALARM         0x03U
#define LED_STATE_ERROR         0x04U

/* 재시작 원인 코드 (FR-024) */
#define RESTART_POWER_ON        0x01U
#define RESTART_IWDG            0x02U
#define RESTART_TASK_WD         0x03U

/* 오류 코드 */
#define SENSOR_ERR_NONE         0x00U
#define SENSOR_ERR_I2C_NACK     0x01U
#define SENSOR_ERR_CRC          0x02U
#define SENSOR_ERR_RANGE        0x03U

/* ── 페이로드 구조체 ── */

/* SENSOR_DATA 페이로드 — 22 bytes (FR-001~004) */
typedef struct __attribute__((packed)) {
    uint8_t  flags;         /* bit0=sht31_valid, bit1=ina219_valid, bit2=door_open */
    uint8_t  sht31_error;   /* SENSOR_ERR_* (flags.bit0==0일 때 유효) */
    uint8_t  ina219_error;  /* SENSOR_ERR_* (flags.bit1==0일 때 유효) */
    float    temperature;   /* °C */
    float    humidity;      /* % */
    float    current_mA;    /* mA */
    float    voltage_V;     /* V */
    uint32_t timestamp_ms;  /* HAL_GetTick() */
} SensorDataPayload_t;

/* ACK/NACK 페이로드 — 1 byte */
typedef struct __attribute__((packed)) {
    uint8_t ack_seq;
} AckPayload_t;

/* CMD_FAN 페이로드 — 1 byte */
typedef struct __attribute__((packed)) {
    uint8_t on;             /* 1=ON, 0=OFF */
} CmdFanPayload_t;

/* CMD_BUZZER 페이로드 — 1 byte */
typedef struct __attribute__((packed)) {
    uint8_t on;
} CmdBuzzerPayload_t;

/* CMD_LED 페이로드 — 1 byte */
typedef struct __attribute__((packed)) {
    uint8_t state;          /* LED_STATE_* */
} CmdLedPayload_t;

/* CMD_HEATER 페이로드 — 1 byte */
typedef struct __attribute__((packed)) {
    uint8_t duty_percent;   /* 0=OFF, 100=최대출력(26W) */
} CmdHeaterPayload_t;

/* RESTART_REASON 페이로드 — 3 bytes (FR-024) */
typedef struct __attribute__((packed)) {
    uint8_t reason_code;    /* RESTART_* */
    uint8_t failed_task_id; /* Task Watchdog 재시작 시 태스크 ID */
    uint8_t restart_count;  /* 누적 재시작 횟수 */
} RestartReasonPayload_t;

/* BUTTON_EVENT 페이로드 — 1 byte */
typedef struct __attribute__((packed)) {
    uint8_t button_id;      /* 0=B1 */
} ButtonEventPayload_t;

/* ── 내부 파싱 결과 구조체 ── */
typedef struct {
    uint8_t  type;
    uint8_t  seq;
    uint16_t len;
    uint8_t  payload[PROTO_MAX_PAYLOAD];
} ParsedFrame_t;

/* ── TaskComm TX 요청 구조체 ── */
typedef struct {
    uint8_t  type;
    uint16_t payloadLen;
    uint8_t  payload[PROTO_MAX_PAYLOAD];
} TxRequest_t;

#endif /* __PROTOCOL_H */
