#pragma once
#include "StateMachine.h"
#include "AlarmManager.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  undef ERROR
using SocketFd = SOCKET;
static constexpr SocketFd INVALID_FD = INVALID_SOCKET;
#else
using SocketFd = int;
static constexpr SocketFd INVALID_FD = -1;
#endif

// HSMS 세션 상태 (SEMI E37)
enum class HsmsSessionState { NOT_CONNECTED, CONNECTED, SELECTED };

class HsmsServer {
public:
    HsmsServer(uint16_t port, StateMachine& sm, AlarmManager& alarmMgr);
    ~HsmsServer();

    bool start();
    void stop();

    void sendAlarmReport(uint8_t alid, bool isSet, const std::string& text);
    void sendEventReport(uint32_t ceid, const uint8_t* rptData, uint16_t rptLen);

    HsmsSessionState sessionState() const;

private:
    void acceptLoop();
    void sessionLoop(SocketFd clientFd);

    static std::vector<uint8_t> buildControlMsg(uint8_t stype, uint32_t sysBytes);
    static std::vector<uint8_t> buildDataMsg(uint16_t sessionId,
                                             uint8_t stream, uint8_t function,
                                             uint32_t sysBytes,
                                             const uint8_t* body, uint16_t bodyLen);

    void handleSelectReq(uint32_t sysBytes);
    void handleLinktestReq(uint32_t sysBytes);
    void handleS1F13(uint32_t sysBytes, const uint8_t* body, uint16_t len);
    void handleS2F41(uint32_t sysBytes, const uint8_t* body, uint16_t len);
    void handleS1F1(uint32_t sysBytes);

    bool sendRaw(const uint8_t* data, int len);

    uint16_t              m_port;
    StateMachine&         m_sm;
    AlarmManager&         m_alarmMgr;

    SocketFd              m_listenFd;
    SocketFd              m_clientFd;
    uint16_t              m_sessionId;
    std::atomic<HsmsSessionState> m_state;

    std::thread           m_acceptThread;
    std::thread           m_sessionThread;
    std::atomic<bool>     m_running;
};
