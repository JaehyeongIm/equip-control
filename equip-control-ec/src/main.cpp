#include "DeviceComm.h"
#include "StateMachine.h"
#include "AlarmManager.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

// ── 센서 유효 범위 (SRS FR-010~012) ────────────────────────────
constexpr float TEMP_MAX_C    = 80.0f;
constexpr float HUMIDITY_MAX  = 95.0f;
constexpr uint64_t HB_TIMEOUT_MS = 10000U;

// ── 현재 시각 (ms) ──────────────────────────────────────────────
static uint64_t nowMs() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
}

int main(int argc, char* argv[]) {
    // UART 포트: 인자로 받거나 기본값 사용
    // macOS: /dev/tty.usbmodem*, Windows: COM3 등
    std::string port = (argc > 1) ? argv[1] : "/dev/tty.usbmodem14103";

    StateMachine  sm;
    AlarmManager  alarmMgr(sm);
    DeviceComm    devComm(port);

    // ── 상태 변경 로그 ─────────────────────────────────────────
    sm.setStateChangeCallback([](EquipState prev, EquipState next) {
        const char* names[] = {"IDLE", "RUNNING", "ALARM", "ERROR"};
        std::cout << "[SM] " << names[(int)prev]
                  << " → " << names[(int)next] << "\n";
    });

    // ── 수신 프레임 처리 콜백 ──────────────────────────────────
    devComm.setFrameCallback([&](const ParsedFrame& f) {
        switch (f.type) {

        case MSG_SENSOR_DATA: {
            SensorDataPayload sensor;
            std::memcpy(&sensor, f.payload, sizeof(sensor));
            std::cout << "[SENSOR] temp=" << sensor.temperature
                      << "C humi=" << sensor.humidity
                      << "% curr=" << sensor.current_mA
                      << "mA volt=" << sensor.voltage_V << "V\n";

            // 알람 조건 검사
            if (sensor.temperature > TEMP_MAX_C)
                alarmMgr.checkAndSet(AlarmId::TEMP_HIGH);
            else
                alarmMgr.tryClear(AlarmId::TEMP_HIGH);

            if (sensor.humidity > HUMIDITY_MAX)
                alarmMgr.checkAndSet(AlarmId::HUMIDITY_HIGH);
            else
                alarmMgr.tryClear(AlarmId::HUMIDITY_HIGH);

            if (sensor.sht31_error || sensor.ina219_error)
                alarmMgr.checkAndSet(AlarmId::SENSOR_ERROR);
            else
                alarmMgr.tryClear(AlarmId::SENSOR_ERROR);
            break;
        }

        case MSG_BUTTON_EVENT: {
            ButtonEventPayload btn;
            std::memcpy(&btn, f.payload, sizeof(btn));
            std::cout << "[BTN] id=" << (int)btn.button_id
                      << " type=" << (int)btn.event_type << "\n";
            // 버튼 press → START/STOP 토글
            if (btn.event_type == 0) {
                if (sm.currentState() == EquipState::IDLE)
                    sm.processEvent(EquipEvent::CMD_START);
                else if (sm.currentState() == EquipState::RUNNING)
                    sm.processEvent(EquipEvent::CMD_STOP);
            }
            break;
        }

        case MSG_RESTART_REASON: {
            RestartReasonPayload rr;
            std::memcpy(&rr, f.payload, sizeof(rr));
            std::cout << "[RESTART] task=" << (int)rr.task_id
                      << " count=" << rr.restart_count << "\n";
            break;
        }

        case MSG_HEARTBEAT_REQ: {
            // STM32 Heartbeat REQ → EC가 ACK 응답 (sendCommand가 자동 ACK 처리)
            std::cout << "[HB] req seq=" << (int)f.seq << "\n";
            break;
        }

        default:
            break;
        }
    });

    // ── UART 열기 ──────────────────────────────────────────────
    if (!devComm.open()) {
        std::cerr << "[EC] UART open failed: " << port << "\n";
        return 1;
    }
    std::cout << "[EC] Connected: " << port << "\n";

    // ── 메인 루프: Heartbeat 감시 ──────────────────────────────
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        uint64_t lastRx = devComm.lastRxTimeMs();
        if (lastRx > 0 && (nowMs() - lastRx) > HB_TIMEOUT_MS) {
            std::cerr << "[EC] Heartbeat timeout — UART_COMM_ERROR\n";
            alarmMgr.checkAndSet(AlarmId::UART_COMM_ERROR);
        }
    }

    return 0;
}
