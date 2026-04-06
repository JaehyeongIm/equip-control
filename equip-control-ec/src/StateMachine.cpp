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

// SDD 4.2 상태 전이 테이블 구현
void StateMachine::processEvent(EquipEvent e) {
    switch (e) {
    case EquipEvent::CMD_START:
        if (m_state == EquipState::IDLE)
            transition(EquipState::RUNNING);
        break;

    case EquipEvent::CMD_STOP:
        if (m_state == EquipState::RUNNING)
            transition(EquipState::IDLE);
        break;

    case EquipEvent::CMD_RESET:
        if (m_state == EquipState::ALARM || m_state == EquipState::ERROR)
            transition(EquipState::IDLE);
        break;

    case EquipEvent::EVENT_ALARM:
        // 어느 상태에서든 ALARM으로 전이
        transition(EquipState::ALARM);
        break;

    case EquipEvent::EVENT_ERROR:
        // 어느 상태에서든 ERROR로 전이
        transition(EquipState::ERROR);
        break;

    case EquipEvent::ALARM_CLEAR:
        if (m_state == EquipState::ALARM)
            transition(EquipState::IDLE);
        break;
    }
}
