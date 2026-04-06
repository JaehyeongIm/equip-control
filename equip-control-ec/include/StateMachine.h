#pragma once
#include <functional>
#include <cstdint>
#ifdef ERROR
#  undef ERROR  // windows.h의 ERROR 매크로 충돌 방지
#endif

enum class EquipState {
    IDLE    = 0,
    RUNNING = 1,
    ALARM   = 2,
    ERROR_STATE = 3,
};

enum class EquipEvent {
    CMD_START   = 0,
    CMD_STOP    = 1,
    CMD_RESET   = 2,
    EVENT_ALARM = 3,
    EVENT_ERROR = 4,
    ALARM_CLEAR = 5,
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
