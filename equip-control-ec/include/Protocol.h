#pragma once
#include <cstdint>

// ── 메시지 타입 ──────────────────────────────────────────────
constexpr uint8_t MSG_SENSOR_DATA    = 0x01U;
constexpr uint8_t MSG_BUTTON_EVENT   = 0x02U;
constexpr uint8_t MSG_RESTART_REASON = 0x03U;
constexpr uint8_t MSG_ACK            = 0x04U;
constexpr uint8_t MSG_NACK           = 0x05U;
constexpr uint8_t MSG_CMD_FAN        = 0x10U;
constexpr uint8_t MSG_CMD_BUZZER     = 0x11U;
constexpr uint8_t MSG_CMD_LED        = 0x12U;
constexpr uint8_t MSG_CMD_STATE_SYNC = 0x13U;
constexpr uint8_t MSG_HEARTBEAT_REQ  = 0x20U;
constexpr uint8_t MSG_HEARTBEAT_ACK  = 0x21U;

constexpr uint8_t  PROTO_SOF       = 0xAAU;
constexpr uint16_t PROTO_MAX_FRAME = 128U;

// ── 페이로드 구조체 ───────────────────────────────────────────
#pragma pack(push, 1)

struct SensorDataPayload {
    uint8_t  flags;         // bit0=sht31_valid, bit1=ina219_valid, bit2=door_open
    uint8_t  sht31_error;
    uint8_t  ina219_error;
    float    temperature;
    float    humidity;
    float    current_mA;
    float    voltage_V;
    uint32_t timestamp_ms;
};  // 22 bytes

struct ButtonEventPayload {
    uint8_t  button_id;
    uint8_t  event_type;  // 0=press, 1=release
    uint32_t timestamp_ms;
};

struct RestartReasonPayload {
    uint8_t  task_id;
    uint32_t restart_count;
};

struct AckPayload {
    uint8_t ack_seq;
};

struct CmdFanPayload    { uint8_t on; };
struct CmdBuzzerPayload { uint8_t on; };
struct CmdLedPayload    { uint8_t state; };  // 1=ON, 0=OFF
struct CmdStateSyncPayload { uint8_t state; };

#pragma pack(pop)

// ── 파싱된 프레임 ─────────────────────────────────────────────
struct ParsedFrame {
    uint8_t  type;
    uint8_t  seq;
    uint16_t len;
    uint8_t  payload[64];
};
