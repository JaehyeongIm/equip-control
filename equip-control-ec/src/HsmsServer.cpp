#include "HsmsServer.h"
#include <cstring>
#include <iostream>
#include <vector>

#ifdef _WIN32
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
static inline void closesocket(int fd) { ::close(fd); }
#endif

// ── HSMS SType 상수 ─────────────────────────────────────────────
static constexpr uint8_t STYPE_DATA         = 0x00;
static constexpr uint8_t STYPE_SELECT_REQ   = 0x01;
static constexpr uint8_t STYPE_SELECT_RSP   = 0x02;
static constexpr uint8_t STYPE_LINKTEST_REQ = 0x05;
static constexpr uint8_t STYPE_LINKTEST_RSP = 0x06;
static constexpr uint8_t STYPE_SEPARATE_REQ = 0x09;

static constexpr int T7_SEC = 10;

static constexpr uint32_t CEID_EQUIPMENT_ONLINE = 1U;
static constexpr uint32_t CEID_COMMAND_RECEIVED = 5U;
static constexpr uint32_t CEID_ALARM_SET        = 6U;
static constexpr uint32_t CEID_ALARM_CLEARED    = 7U;

// ── 생성자 / 소멸자 ─────────────────────────────────────────────
HsmsServer::HsmsServer(uint16_t port, StateMachine& sm, AlarmManager& alarmMgr)
    : m_port(port), m_sm(sm), m_alarmMgr(alarmMgr),
      m_listenFd(INVALID_FD), m_clientFd(INVALID_FD), m_sessionId(0xFFFF),
      m_state(HsmsSessionState::NOT_CONNECTED), m_running(false)
{}

HsmsServer::~HsmsServer() { stop(); }

// ── 서버 시작 ────────────────────────────────────────────────────
bool HsmsServer::start() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd == INVALID_FD) return false;

#ifdef _WIN32
    char opt = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#else
    int opt = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(m_port);

    if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        return false;
    if (::listen(m_listenFd, 1) < 0)
        return false;

    m_running = true;
    m_acceptThread = std::thread(&HsmsServer::acceptLoop, this);
    std::cout << "[HSMS] Listening on port " << m_port << "\n";
    return true;
}

void HsmsServer::stop() {
    m_running = false;
    if (m_listenFd != INVALID_FD) { closesocket(m_listenFd); m_listenFd = INVALID_FD; }
    if (m_clientFd != INVALID_FD) { closesocket(m_clientFd); m_clientFd = INVALID_FD; }
    if (m_acceptThread.joinable())  m_acceptThread.join();
    if (m_sessionThread.joinable()) m_sessionThread.join();
#ifdef _WIN32
    WSACleanup();
#endif
}

HsmsSessionState HsmsServer::sessionState() const { return m_state.load(); }

// ── Accept 루프 ──────────────────────────────────────────────────
void HsmsServer::acceptLoop() {
    while (m_running) {
        sockaddr_in clientAddr{};
        socklen_t   addrLen = sizeof(clientAddr);
        SocketFd fd = ::accept(m_listenFd,
                               reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (fd == INVALID_FD) break;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
        std::cout << "[HSMS] Host connected from " << ip << "\n";

        m_clientFd = fd;
        m_state    = HsmsSessionState::CONNECTED;

        if (m_sessionThread.joinable()) m_sessionThread.join();
        m_sessionThread = std::thread(&HsmsServer::sessionLoop, this, fd);
    }
}

// ── 세션 루프 ────────────────────────────────────────────────────
void HsmsServer::sessionLoop(SocketFd clientFd) {
#ifdef _WIN32
    DWORD tv = T7_SEC * 1000;
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv{ T7_SEC, 0 };
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    uint8_t buf[4096];
    std::vector<uint8_t> rxBuf;

    while (m_running) {
        int n = static_cast<int>(::recv(clientFd,
                                        reinterpret_cast<char*>(buf), sizeof(buf), 0));
        if (n <= 0) {
            std::cout << "[HSMS] Host disconnected\n";
            break;
        }
        rxBuf.insert(rxBuf.end(), buf, buf + n);

        while (rxBuf.size() >= 14) {
            uint32_t length = (static_cast<uint32_t>(rxBuf[0]) << 24)
                            | (static_cast<uint32_t>(rxBuf[1]) << 16)
                            | (static_cast<uint32_t>(rxBuf[2]) << 8)
                            |  rxBuf[3];
            if (rxBuf.size() < 4U + length) break;

            const uint8_t* h   = rxBuf.data() + 4;
            uint16_t sessionId = (static_cast<uint16_t>(h[0]) << 8) | h[1];
            uint8_t  stream    = h[2] & 0x7F;
            uint8_t  function  = h[3];
            uint8_t  stype     = h[5];
            uint32_t sysBytes  = (static_cast<uint32_t>(h[8]) << 8) | h[9];
            const uint8_t* body = rxBuf.data() + 14;
            uint16_t bodyLen   = static_cast<uint16_t>(length - 10);

            if (m_state == HsmsSessionState::SELECTED) {
#ifdef _WIN32
                DWORD noTimeout = 0;
                setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
#else
                struct timeval noTimeout{ 0, 0 };
                setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &noTimeout, sizeof(noTimeout));
#endif
            }

            if (stype == STYPE_SELECT_REQ) {
                m_sessionId = sessionId;
                handleSelectReq(sysBytes);
            } else if (stype == STYPE_LINKTEST_REQ) {
                handleLinktestReq(sysBytes);
            } else if (stype == STYPE_SEPARATE_REQ) {
                std::cout << "[HSMS] SeparateReq — session closed\n";
                rxBuf.clear();
                goto session_end;
            } else if (stype == STYPE_DATA) {
                if (stream == 1 && function == 1)
                    handleS1F1(sysBytes);
                else if (stream == 1 && function == 13)
                    handleS1F13(sysBytes, body, bodyLen);
                else if (stream == 2 && function == 41)
                    handleS2F41(sysBytes, body, bodyLen);
            }

            rxBuf.erase(rxBuf.begin(), rxBuf.begin() + 4 + length);
        }
    }

session_end:
    closesocket(clientFd);
    if (m_clientFd == clientFd) m_clientFd = INVALID_FD;
    m_state = HsmsSessionState::NOT_CONNECTED;
}

// ── 메시지 빌더 ──────────────────────────────────────────────────
std::vector<uint8_t> HsmsServer::buildControlMsg(uint8_t stype, uint32_t sysBytes) {
    std::vector<uint8_t> msg(14, 0);
    msg[0] = 0; msg[1] = 0; msg[2] = 0; msg[3] = 10;
    msg[4] = 0xFF; msg[5] = 0xFF;
    msg[6] = 0; msg[7] = 0; msg[8] = 0; msg[9] = stype;
    msg[10] = 0; msg[11] = 0;
    msg[12] = static_cast<uint8_t>((sysBytes >> 8) & 0xFF);
    msg[13] = static_cast<uint8_t>(sysBytes & 0xFF);
    return msg;
}

std::vector<uint8_t> HsmsServer::buildDataMsg(uint16_t sessionId,
                                              uint8_t stream, uint8_t function,
                                              uint32_t sysBytes,
                                              const uint8_t* body, uint16_t bodyLen) {
    uint32_t length = 10U + bodyLen;
    std::vector<uint8_t> msg(4 + length);
    msg[0] = static_cast<uint8_t>(length >> 24);
    msg[1] = static_cast<uint8_t>(length >> 16);
    msg[2] = static_cast<uint8_t>(length >> 8);
    msg[3] = static_cast<uint8_t>(length);
    msg[4] = static_cast<uint8_t>(sessionId >> 8);
    msg[5] = static_cast<uint8_t>(sessionId);
    msg[6] = stream & 0x7F;
    msg[7] = function;
    msg[8] = 0; msg[9] = STYPE_DATA; msg[10] = 0; msg[11] = 0;
    msg[12] = static_cast<uint8_t>((sysBytes >> 8) & 0xFF);
    msg[13] = static_cast<uint8_t>(sysBytes & 0xFF);
    if (body && bodyLen > 0)
        std::memcpy(msg.data() + 14, body, bodyLen);
    return msg;
}

bool HsmsServer::sendRaw(const uint8_t* data, int len) {
    if (m_clientFd == INVALID_FD) return false;
    return ::send(m_clientFd, reinterpret_cast<const char*>(data), len, 0) == len;
}

// ── S/F 핸들러 ───────────────────────────────────────────────────
void HsmsServer::handleSelectReq(uint32_t sysBytes) {
    m_state = HsmsSessionState::SELECTED;
    auto rsp = buildControlMsg(STYPE_SELECT_RSP, sysBytes);
    sendRaw(rsp.data(), static_cast<int>(rsp.size()));
    std::cout << "[HSMS] SELECTED\n";
    sendEventReport(CEID_EQUIPMENT_ONLINE, nullptr, 0);
}

void HsmsServer::handleLinktestReq(uint32_t sysBytes) {
    auto rsp = buildControlMsg(STYPE_LINKTEST_RSP, sysBytes);
    sendRaw(rsp.data(), static_cast<int>(rsp.size()));
}

void HsmsServer::handleS1F1(uint32_t sysBytes) {
    uint8_t body[] = {
        0x01, 0x02,
        0x41, 0x0D, 'E','Q','U','I','P','-','C','T','R','L','-','E','C',
        0x41, 0x03, '1','.','0'
    };
    auto msg = buildDataMsg(m_sessionId, 1, 2, sysBytes, body, sizeof(body));
    sendRaw(msg.data(), static_cast<int>(msg.size()));
}

void HsmsServer::handleS1F13(uint32_t sysBytes, const uint8_t* /*body*/, uint16_t /*len*/) {
    uint8_t body[] = { 0x01, 0x02, 0x21, 0x01, 0x00, 0x01, 0x00 };
    auto msg = buildDataMsg(m_sessionId, 1, 14, sysBytes, body, sizeof(body));
    sendRaw(msg.data(), static_cast<int>(msg.size()));
    std::cout << "[HSMS] S1F14 sent — communication established\n";
}

void HsmsServer::handleS2F41(uint32_t sysBytes, const uint8_t* body, uint16_t len) {
    if (m_state != HsmsSessionState::SELECTED) return;

    std::string rcmd;
    if (len > 4 && body[2] == 0x41) {
        uint8_t rcmdLen = body[3];
        if (4 + rcmdLen <= len)
            rcmd = std::string(reinterpret_cast<const char*>(body + 4), rcmdLen);
    }
    std::cout << "[HSMS] S2F41 RCMD=" << rcmd << "\n";

    uint8_t hcack = 0x00;
    if (rcmd == "START") {
        if (m_sm.currentState() != EquipState::IDLE) hcack = 0x02;
        else m_sm.processEvent(EquipEvent::CMD_START);
    } else if (rcmd == "STOP") {
        m_sm.processEvent(EquipEvent::CMD_STOP);
    } else if (rcmd == "RESET") {
        if (m_sm.currentState() == EquipState::ERROR_STATE) hcack = 0x04;
        else m_sm.processEvent(EquipEvent::CMD_RESET);
    } else if (rcmd == "ACK_ALARM") {
        m_alarmMgr.tryClear(AlarmId::TEMP_HIGH);
        m_alarmMgr.tryClear(AlarmId::HUMIDITY_HIGH);
        m_alarmMgr.tryClear(AlarmId::SENSOR_ERROR);
    } else {
        hcack = 0x05;
    }

    uint8_t rspBody[] = { 0x01, 0x02, 0x21, 0x01, hcack, 0x01, 0x00 };
    auto msg = buildDataMsg(m_sessionId, 2, 42, sysBytes, rspBody, sizeof(rspBody));
    sendRaw(msg.data(), static_cast<int>(msg.size()));
    sendEventReport(CEID_COMMAND_RECEIVED, nullptr, 0);
}

void HsmsServer::sendAlarmReport(uint8_t alid, bool isSet, const std::string& text) {
    if (m_state != HsmsSessionState::SELECTED) return;

    uint8_t alcd = isSet ? 0x81U : 0x80U;
    std::vector<uint8_t> body;
    body.push_back(0x01); body.push_back(0x03);
    body.push_back(0x21); body.push_back(0x01); body.push_back(alcd);
    body.push_back(0x21); body.push_back(0x01); body.push_back(alid);
    body.push_back(0x41);
    body.push_back(static_cast<uint8_t>(text.size()));
    for (char c : text) body.push_back(static_cast<uint8_t>(c));

    auto msg = buildDataMsg(m_sessionId, 5, 1, 0x0001,
                            body.data(), static_cast<uint16_t>(body.size()));
    sendRaw(msg.data(), static_cast<int>(msg.size()));
    sendEventReport(isSet ? CEID_ALARM_SET : CEID_ALARM_CLEARED, nullptr, 0);
}

void HsmsServer::sendEventReport(uint32_t ceid,
                                 const uint8_t* rptData, uint16_t rptLen) {
    if (m_state != HsmsSessionState::SELECTED) return;

    std::vector<uint8_t> body;
    body.push_back(0x01); body.push_back(0x03);
    body.push_back(0xB1); body.push_back(0x04);
    body.push_back(0); body.push_back(0); body.push_back(0); body.push_back(0);
    body.push_back(0xB1); body.push_back(0x04);
    body.push_back(static_cast<uint8_t>(ceid >> 24));
    body.push_back(static_cast<uint8_t>(ceid >> 16));
    body.push_back(static_cast<uint8_t>(ceid >> 8));
    body.push_back(static_cast<uint8_t>(ceid));
    body.push_back(0x01); body.push_back(0x01);
    body.push_back(0x01); body.push_back(static_cast<uint8_t>(rptLen > 0 ? 1 : 0));
    if (rptData && rptLen > 0)
        body.insert(body.end(), rptData, rptData + rptLen);

    auto msg = buildDataMsg(m_sessionId, 6, 11, 0x0002,
                            body.data(), static_cast<uint16_t>(body.size()));
    sendRaw(msg.data(), static_cast<int>(msg.size()));
}
