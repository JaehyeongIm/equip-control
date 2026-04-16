#pragma once
#include "StateMachine.h"
#include <cstdint>
#include <map>

// AIM-001 §4.1 알람 ID 정의
enum class AlarmId : uint8_t {
    TEMP_HIGH     = 1,  // SW-AL: 온도 > 65°C
    TEMP_CRITICAL = 2,  // SW-IL: 온도 > 68°C → INTERLOCK
    HW_INTERLOCK  = 4,  // HW-IL+SW-AL: INA219 전류 < 50mA (HEATING 중)
    SENSOR_ERROR  = 5,  // SW-IL: SHT31 I2C 연속 3회 오류 → INTERLOCK
    COMM_ERROR    = 6,  // SW-AL: Heartbeat 10초 미수신
};

class AlarmManager {
public:
    explicit AlarmManager(StateMachine& sm);

    void checkAndSet(AlarmId id);          // SW-AL: EVENT_ALARM 발생
    void checkAndSetInterlock(AlarmId id); // SW-IL: EVENT_INTERLOCK 발생
    void tryClear(AlarmId id);
    bool hasActiveAlarm() const;
    bool isActive(AlarmId id) const;

private:
    StateMachine&        m_sm;
    std::map<AlarmId, bool> m_alarms;
};
