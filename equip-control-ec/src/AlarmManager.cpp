#include "AlarmManager.h"

AlarmManager::AlarmManager(StateMachine& sm) : m_sm(sm) {}

void AlarmManager::checkAndSet(AlarmId id) {
    if (m_alarms[id]) return;  // 이미 활성
    m_alarms[id] = true;
    m_sm.processEvent(EquipEvent::EVENT_ALARM);
}

void AlarmManager::tryClear(AlarmId id) {
    m_alarms[id] = false;
    if (!hasActiveAlarm())
        m_sm.processEvent(EquipEvent::ALARM_CLEAR);
}

bool AlarmManager::hasActiveAlarm() const {
    for (const auto& kv : m_alarms)
        if (kv.second) return true;
    return false;
}

bool AlarmManager::isActive(AlarmId id) const {
    auto it = m_alarms.find(id);
    return it != m_alarms.end() && it->second;
}
