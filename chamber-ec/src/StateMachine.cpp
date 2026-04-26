#include "StateMachine.h"

StateMachine::StateMachine() : m_state(EquipState::IDLE) {}

EquipState StateMachine::currentState() const { return m_state; }

void StateMachine::setStateChangeCallback(StateChangeCallback cb) {
    m_callback = std::move(cb);
}

void StateMachine::transition(EquipState next) {
    if (m_state == next) return;
    EquipState prev = m_state;
    m_state = next;
    if (m_callback) m_callback(prev, next);
}

// AIM-001 §4 상태 전이 테이블 구현
void StateMachine::processEvent(EquipEvent e) {
    switch (e) {
    case EquipEvent::CMD_START:
        if (m_state == EquipState::IDLE)
            transition(EquipState::HEATING);
        break;

    case EquipEvent::CMD_STOP:
        if (m_state == EquipState::HEATING)
            transition(EquipState::IDLE);
        break;

    case EquipEvent::CMD_RESET:
        // 복구 조건은 외부(HsmsServer)에서 확인 후 이벤트 전달
        if (m_state == EquipState::ALARM || m_state == EquipState::INTERLOCK)
            transition(EquipState::IDLE);
        break;

    case EquipEvent::EVENT_ALARM:
        // SW-AL: HEATING → ALARM (이미 INTERLOCK이면 무시)
        if (m_state == EquipState::HEATING)
            transition(EquipState::ALARM);
        break;

    case EquipEvent::EVENT_INTERLOCK:
        // SW-IL: HEATING 또는 ALARM → INTERLOCK
        if (m_state == EquipState::HEATING || m_state == EquipState::ALARM)
            transition(EquipState::INTERLOCK);
        break;

    case EquipEvent::ALARM_CLEAR:
        // SW-AL 알람이 모두 해소된 경우 HEATING으로 복귀 (COMM_ERROR 자동 복구)
        if (m_state == EquipState::ALARM)
            transition(EquipState::HEATING);
        break;
    }
}
