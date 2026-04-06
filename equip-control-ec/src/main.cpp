#include "DeviceComm.h"
#include "StateMachine.h"
#include "AlarmManager.h"
#include "HsmsServer.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

// ── 센서 유효 범위 (SRS FR-010~012) ────────────────────────────
constexpr float    TEMP_MAX_C     = 80.0f;
constexpr float    HUMIDITY_MAX   = 95.0f;
constexpr uint64_t HB_TIMEOUT_MS  = 10000U;
constexpr uint16_t HSMS_PORT      = 5000U;

static uint64_t nowMs() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
}

int main(int argc, char* argv[]) {
    std::string port = (argc > 1) ? argv[1] : "/dev/tty.usbmodem14103";

    StateMachine  sm;
    AlarmManager  alarmMgr(sm);
    HsmsServer    hsmsSrv(HSMS_PORT, sm, alarmMgr);
    DeviceComm    devComm(port);

    // ── StateMachine 상태 변경 → S6F11 이벤트 전송 ────────────
    sm.setStateChangeCallback([&](EquipState prev, EquipState next) {
        const char* names[] = {"IDLE", "RUNNING", "ALARM", "ERROR"};
        std::cout << "[SM] " << names[static_cast<int>(prev)]
                  << " → " << names[static_cast<int>(next)] << "\n";

        if (next == EquipState::RUNNING) {
            // STM32 팬(LD2) ON
            CmdFanPayload fanOn{ 1 };
            devComm.sendCommand(MSG_CMD_FAN,
                                reinterpret_cast<const uint8_t*>(&fanOn),
                                sizeof(fanOn));
            hsmsSrv.sendEventReport(3U, nullptr, 0);  // CEID-3 ProcessStart
        } else if (next == EquipState::IDLE) {
            // STM32 팬(LD2) OFF
            CmdFanPayload fanOff{ 0 };
            devComm.sendCommand(MSG_CMD_FAN,
                                reinterpret_cast<const uint8_t*>(&fanOff),
                                sizeof(fanOff));
            if (prev == EquipState::RUNNING)
                hsmsSrv.sendEventReport(4U, nullptr, 0);  // CEID-4 ProcessComplete
        }
    });

    // ── AlarmManager → S5F1 알람 보고 ─────────────────────────
    // AlarmManager가 알람 상태를 변경할 때마다 HSMS로 전달
    // (AlarmManager는 checkAndSet/tryClear 시 SM에만 알리므로
    //  여기서 DeviceComm 콜백 안에서 직접 호출)

    // ── STM32 수신 프레임 처리 ─────────────────────────────────
    devComm.setFrameCallback([&](const ParsedFrame& f) {
        switch (f.type) {

        case MSG_SENSOR_DATA: {
            SensorDataPayload sensor;
            std::memcpy(&sensor, f.payload, sizeof(sensor));
            std::cout << "[SENSOR] temp=" << sensor.temperature
                      << "C humi=" << sensor.humidity
                      << "% curr=" << sensor.current_mA
                      << "mA volt=" << sensor.voltage_V << "V\n";

            // 알람 조건 검사 + S5F1 전송
            auto checkAlarm = [&](AlarmId id, bool triggered,
                                  uint8_t alid, const char* text) {
                bool wasClear = !alarmMgr.isActive(id);
                if (triggered) {
                    alarmMgr.checkAndSet(id);
                    if (wasClear)
                        hsmsSrv.sendAlarmReport(alid, true, text);
                } else {
                    bool wasSet = alarmMgr.isActive(id);
                    alarmMgr.tryClear(id);
                    if (wasSet)
                        hsmsSrv.sendAlarmReport(alid, false, text);
                }
            };

            checkAlarm(AlarmId::TEMP_HIGH,
                       sensor.temperature > TEMP_MAX_C,
                       1, "Temperature High");
            checkAlarm(AlarmId::HUMIDITY_HIGH,
                       sensor.humidity > HUMIDITY_MAX,
                       2, "Humidity High");
            checkAlarm(AlarmId::SENSOR_ERROR,
                       sensor.sht31_error || sensor.ina219_error,
                       3, "Sensor Error");

            // 센서 데이터 S6F11 CEID-8 전송 (F4×4 페이로드)
            uint8_t rpt[24];
            uint16_t rptLen = 0;
            auto appendF4 = [&](float v) {
                rpt[rptLen++] = 0x91; rpt[rptLen++] = 0x04;
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                rpt[rptLen++] = (bits >> 24) & 0xFF;
                rpt[rptLen++] = (bits >> 16) & 0xFF;
                rpt[rptLen++] = (bits >>  8) & 0xFF;
                rpt[rptLen++] =  bits        & 0xFF;
            };
            appendF4(sensor.temperature);
            appendF4(sensor.humidity);
            appendF4(sensor.current_mA);
            appendF4(sensor.voltage_V);
            hsmsSrv.sendEventReport(8U, rpt, rptLen);
            break;
        }

        case MSG_BUTTON_EVENT: {
            ButtonEventPayload btn;
            std::memcpy(&btn, f.payload, sizeof(btn));
            std::cout << "[BTN] id=" << static_cast<int>(btn.button_id)
                      << " type=" << static_cast<int>(btn.event_type) << "\n";
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
            std::cout << "[RESTART] task=" << static_cast<int>(rr.task_id)
                      << " count=" << rr.restart_count << "\n";
            break;
        }

        case MSG_HEARTBEAT_REQ:
            std::cout << "[HB] seq=" << static_cast<int>(f.seq) << "\n";
            break;

        default:
            break;
        }
    });

    // ── HSMS 서버 시작 ─────────────────────────────────────────
    if (!hsmsSrv.start()) {
        std::cerr << "[EC] HSMS server start failed (port " << HSMS_PORT << ")\n";
        return 1;
    }

    // ── UART 열기 ──────────────────────────────────────────────
    if (!devComm.open()) {
        std::cerr << "[EC] UART open failed: " << port << "\n";
        return 1;
    }
    std::cout << "[EC] UART connected: " << port << "\n";

    // ── 메인 루프: Heartbeat 감시 ──────────────────────────────
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        uint64_t lastRx = devComm.lastRxTimeMs();
        if (lastRx > 0 && (nowMs() - lastRx) > HB_TIMEOUT_MS) {
            std::cerr << "[EC] Heartbeat timeout\n";
            bool wasClear = !alarmMgr.isActive(AlarmId::UART_COMM_ERROR);
            alarmMgr.checkAndSet(AlarmId::UART_COMM_ERROR);
            if (wasClear)
                hsmsSrv.sendAlarmReport(4, true, "UART Comm Error");
        }
    }

    return 0;
}
