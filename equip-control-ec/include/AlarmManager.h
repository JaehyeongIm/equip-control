#pragma once
#include "StateMachine.h"
#include <cstdint>
#include <map>

enum class AlarmId : uint8_t {
    TEMP_HIGH       = 1,
    HUMIDITY_HIGH   = 2,
    SENSOR_ERROR    = 3,
    UART_COMM_ERROR = 4,
};

class AlarmManager {
public:
    explicit AlarmManager(StateMachine& sm);

    void checkAndSet(AlarmId id);
    void tryClear(AlarmId id);
    bool hasActiveAlarm() const;
    bool isActive(AlarmId id) const;

private:
    StateMachine&        m_sm;
    std::map<AlarmId, bool> m_alarms;
};
