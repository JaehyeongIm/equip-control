#pragma once
#include <functional>
#include <cstdint>
#ifdef ERROR
#  undef ERROR  // windows.h의 ERROR 매크로 충돌 방지
#endif

enum class EquipState {
    IDLE      = 0,
    HEATING   = 1,
    ALARM     = 2,
    INTERLOCK = 3,
};

enum class EquipEvent {
    CMD_START        = 0,
    CMD_STOP         = 1,
    CMD_RESET        = 2,
    EVENT_ALARM      = 3,  // SW-AL: ALARM 상태로 전이 (ALID=1,4,6)
    EVENT_INTERLOCK  = 4,  // SW-IL: INTERLOCK 상태로 전이 (ALID=2,5)
    ALARM_CLEAR      = 5,  // 모든 알람 해소 시 자동 복구
};

using StateChangeCallback = std::function<void(EquipState prev, EquipState next)>;

class StateMachine {
public:
    StateMachine();

    void processEvent(EquipEvent e);
    EquipState currentState() const;

    void setStateChangeCallback(StateChangeCallback cb);

private:
    void transition(EquipState next);

    EquipState          m_state;
    StateChangeCallback m_callback;
};
