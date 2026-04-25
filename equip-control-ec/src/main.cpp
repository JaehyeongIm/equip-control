#include "DeviceComm.h"
#include "StateMachine.h"
#include "AlarmManager.h"
#include "HsmsServer.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

// ── 알람 임계값 (EFS-001 SP-01~SP-05) ──────────────────────────
constexpr float    TEMP_HIGH_C    = 65.0f;   // SP-01: SW ALARM 전이
constexpr float    TEMP_CRIT_C    = 68.0f;   // SP-02: SW INTERLOCK 전이
constexpr float    CURR_OPEN_MA   = 50.0f;   // SP-05: HW 인터락 판정 (ST-22 개방)
constexpr uint64_t HB_TIMEOUT_MS  = 10000U;  // Heartbeat 타임아웃 (10초)
constexpr uint16_t HSMS_PORT      = 5001U;

static uint64_t nowMs() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
}

int main(int argc, char* argv[]) {
    std::string port = (argc > 1) ? argv[1] : "/dev/tty.usbmodem21303";

    StateMachine  sm;
    AlarmManager  alarmMgr(sm);
    HsmsServer    hsmsSrv(HSMS_PORT, sm, alarmMgr);
    DeviceComm    devComm(port);

    // ── StateMachine 상태 변경 → S6F11 이벤트 전송 ────────────
    auto sendHeater = [&](uint8_t duty) {
        CmdHeaterPayload p{ duty };
        bool ok = devComm.sendCommand(MSG_CMD_HEATER,
                      reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        std::cout << "[EC] HEATER duty=" << static_cast<int>(duty)
                  << "%: " << (ok ? "ACK" : "TIMEOUT") << "\n";
    };
    auto sendFan = [&](uint8_t on) {
        CmdFanPayload p{ on };
        bool ok = devComm.sendCommand(MSG_CMD_FAN,
                      reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        std::cout << "[EC] FAN " << (on ? "ON" : "OFF")
                  << ": " << (ok ? "ACK" : "TIMEOUT") << "\n";
    };
    auto sendBuzzer = [&](uint8_t on) {
        CmdBuzzerPayload p{ on };
        bool ok = devComm.sendCommand(MSG_CMD_BUZZER,
                      reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        std::cout << "[EC] BUZZER " << (on ? "ON" : "OFF")
                  << ": " << (ok ? "ACK" : "TIMEOUT") << "\n";
    };
    auto sendLed = [&](uint8_t state) {
        CmdLedPayload p{ state };
        devComm.sendCommand(MSG_CMD_LED,
            reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    };

    sm.setStateChangeCallback([&](EquipState prev, EquipState next) {
        const char* names[] = {"IDLE", "HEATING", "ALARM", "INTERLOCK"};
        std::cout << "[SM] " << names[static_cast<int>(prev)]
                  << " -> " << names[static_cast<int>(next)] << "\n";

        if (next == EquipState::HEATING) {
            sendHeater(100);
            sendFan(0);
            sendBuzzer(0);
            sendLed(LED_STATE_RUN);
            if (prev == EquipState::IDLE)
                hsmsSrv.sendEventReport(3U, nullptr, 0);  // CEID-3 ProcessStart

        } else if (next == EquipState::ALARM) {
            // AIM-001: 65°C 초과 → PWM 감소, 팬 ON, 부저 ON
            sendHeater(50);
            sendFan(1);
            sendBuzzer(1);
            sendLed(LED_STATE_ALARM);

        } else if (next == EquipState::INTERLOCK) {
            // AIM-001: 68°C 초과 → PWM=0
            sendHeater(0);
            sendFan(1);
            sendBuzzer(1);
            sendLed(LED_STATE_ERROR);

        } else if (next == EquipState::IDLE) {
            sendHeater(0);
            sendFan(0);
            sendBuzzer(0);
            sendLed(LED_STATE_IDLE);
            if (prev == EquipState::HEATING)
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

            // ALID=1 TEMP_HIGH: SW-AL (65°C 초과) — AIM-001
            checkAlarm(AlarmId::TEMP_HIGH,
                       sensor.temperature > TEMP_HIGH_C,
                       1, "TEMP_HIGH: Chamber temperature exceeded 65C");

            // ALID=2 TEMP_CRITICAL: SW-IL (68°C 초과) — INTERLOCK 전이
            {
                bool triggered = sensor.temperature > TEMP_CRIT_C;
                bool wasClear  = !alarmMgr.isActive(AlarmId::TEMP_CRITICAL);
                if (triggered) {
                    alarmMgr.checkAndSetInterlock(AlarmId::TEMP_CRITICAL);
                    if (wasClear)
                        hsmsSrv.sendAlarmReport(2, true,
                            "TEMP_CRITICAL: Interlock activated at 68C");
                } else {
                    bool wasSet = alarmMgr.isActive(AlarmId::TEMP_CRITICAL);
                    alarmMgr.tryClear(AlarmId::TEMP_CRITICAL);
                    if (wasSet)
                        hsmsSrv.sendAlarmReport(2, false,
                            "TEMP_CRITICAL: Interlock activated at 68C");
                }
            }

            // ALID=4 HW_INTERLOCK: ST-22 개방 감지 (HEATING 중 전류 < 50mA)
            checkAlarm(AlarmId::HW_INTERLOCK,
                       sm.currentState() == EquipState::HEATING
                           && sensor.current_mA < CURR_OPEN_MA,
                       4, "HW_INTERLOCK: ST-22 operated or circuit open");

            // ALID=5 SENSOR_ERROR: SW-IL — SHT31/INA219 I2C 오류
            {
                bool triggered = sensor.sht31_error || sensor.ina219_error;
                bool wasClear  = !alarmMgr.isActive(AlarmId::SENSOR_ERROR);
                if (triggered) {
                    alarmMgr.checkAndSetInterlock(AlarmId::SENSOR_ERROR);
                    if (wasClear)
                        hsmsSrv.sendAlarmReport(5, true,
                            "SENSOR_ERROR: SHT31 communication failure");
                } else {
                    bool wasSet = alarmMgr.isActive(AlarmId::SENSOR_ERROR);
                    alarmMgr.tryClear(AlarmId::SENSOR_ERROR);
                    if (wasSet)
                        hsmsSrv.sendAlarmReport(5, false,
                            "SENSOR_ERROR: SHT31 communication failure");
                }
            }

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
                else if (sm.currentState() == EquipState::HEATING)
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
            bool wasClear = !alarmMgr.isActive(AlarmId::COMM_ERROR);
            alarmMgr.checkAndSet(AlarmId::COMM_ERROR);
            if (wasClear)
                hsmsSrv.sendAlarmReport(6, true,
                    "COMM_ERROR: STM32 heartbeat timeout");
        } else if (alarmMgr.isActive(AlarmId::COMM_ERROR)) {
            // Heartbeat 재수신 → COMM_ERROR 자동 해제 (AIM-001 §7)
            alarmMgr.tryClear(AlarmId::COMM_ERROR);
            hsmsSrv.sendAlarmReport(6, false,
                "COMM_ERROR: STM32 heartbeat timeout");
        }
    }

    return 0;
}
