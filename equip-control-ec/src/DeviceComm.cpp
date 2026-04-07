#include "DeviceComm.h"
#include <chrono>
#include <cstring>
#include <iostream>

#ifndef _WIN32
#  include <fcntl.h>
#  include <termios.h>
#  include <unistd.h>
#endif

// ── 생성자 / 소멸자 ─────────────────────────────────────────────
DeviceComm::DeviceComm(const std::string& portName, uint32_t /*baudRate*/)
    : m_port(portName),
#ifdef _WIN32
      m_fd(INVALID_HANDLE_VALUE),
#else
      m_fd(-1),
#endif
      m_txSeq(0),
      m_running(false), m_lastRxMs(0),
      m_parseState(ParseState::WAIT_SOF), m_parseIdx(0)
{}

DeviceComm::~DeviceComm() { close(); }

// ── UART 열기 ───────────────────────────────────────────────────
bool DeviceComm::open() {
#ifdef _WIN32
    // Windows: COM 포트는 "\\\\.\\COMx" 형식
    std::string path = "\\\\.\\" + m_port;
    m_fd = CreateFileA(path.c_str(),
                       GENERIC_READ | GENERIC_WRITE,
                       0, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_fd == INVALID_HANDLE_VALUE) return false;

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(m_fd, &dcb);
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(m_fd, &dcb);

    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout         = 10;
    to.ReadTotalTimeoutConstant    = 100;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.WriteTotalTimeoutConstant   = 100;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(m_fd, &to);
#else
    m_fd = ::open(m_port.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (m_fd < 0) return false;

    struct termios tty{};
    tcgetattr(m_fd, &tty);
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_lflag = 0;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 0;
    tcsetattr(m_fd, TCSANOW, &tty);
#endif

    m_running = true;
    m_rxThread = std::thread(&DeviceComm::rxLoop, this);
    return true;
}

void DeviceComm::close() {
    m_running = false;
    if (m_rxThread.joinable()) m_rxThread.join();
#ifdef _WIN32
    if (m_fd != INVALID_HANDLE_VALUE) { CloseHandle(m_fd); m_fd = INVALID_HANDLE_VALUE; }
#else
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
#endif
}

bool DeviceComm::isOpen() const {
#ifdef _WIN32
    return m_fd != INVALID_HANDLE_VALUE;
#else
    return m_fd >= 0;
#endif
}

void DeviceComm::setFrameCallback(FrameCallback cb) {
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    m_callback = std::move(cb);
}

uint64_t DeviceComm::lastRxTimeMs() const { return m_lastRxMs.load(); }

// ── CRC16-CCITT (poly=0x1021, init=0xFFFF) ──────────────────────
uint16_t DeviceComm::crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFFU;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000U) ? (crc << 1) ^ 0x1021U : crc << 1;
    }
    return crc;
}

// ── 프레임 빌드: SOF+TYPE+SEQ+LEN_L+LEN_H+PAYLOAD+CRC_L+CRC_H ──
uint16_t DeviceComm::buildFrame(uint8_t type, uint8_t seq,
                                const uint8_t* payload, uint16_t payloadLen,
                                uint8_t* outBuf) {
    outBuf[0] = PROTO_SOF;
    outBuf[1] = type;
    outBuf[2] = seq;
    outBuf[3] = static_cast<uint8_t>(payloadLen & 0xFFU);
    outBuf[4] = static_cast<uint8_t>(payloadLen >> 8);
    if (payload && payloadLen > 0)
        std::memcpy(&outBuf[5], payload, payloadLen);
    uint16_t crc = crc16(&outBuf[1], 4U + payloadLen);
    outBuf[5 + payloadLen] = static_cast<uint8_t>(crc & 0xFFU);
    outBuf[6 + payloadLen] = static_cast<uint8_t>(crc >> 8);
    return static_cast<uint16_t>(7U + payloadLen);
}

// ── 1바이트 파서 ─────────────────────────────────────────────────
bool DeviceComm::parserFeed(uint8_t byte, ParsedFrame& out) {
    switch (m_parseState) {
    case ParseState::WAIT_SOF:
        if (byte == PROTO_SOF) m_parseState = ParseState::TYPE;
        break;
    case ParseState::TYPE:
        m_parseFrame.type = byte;
        m_parseState = ParseState::SEQ;
        break;
    case ParseState::SEQ:
        m_parseFrame.seq = byte;
        m_parseState = ParseState::LEN_L;
        break;
    case ParseState::LEN_L:
        m_parseFrame.len = byte;
        m_parseState = ParseState::LEN_H;
        break;
    case ParseState::LEN_H:
        m_parseFrame.len |= static_cast<uint16_t>(byte) << 8;
        m_parseIdx = 0;
        m_parseState = (m_parseFrame.len == 0) ? ParseState::CRC_L
                                                : ParseState::PAYLOAD;
        break;
    case ParseState::PAYLOAD:
        if (m_parseIdx < sizeof(m_parseFrame.payload))
            m_parseFrame.payload[m_parseIdx] = byte;
        if (++m_parseIdx >= m_parseFrame.len)
            m_parseState = ParseState::CRC_L;
        break;
    case ParseState::CRC_L:
        m_parseFrame.payload[m_parseFrame.len] = byte;
        m_parseState = ParseState::CRC_H;
        break;
    case ParseState::CRC_H: {
        uint16_t rxCrc = m_parseFrame.payload[m_parseFrame.len]
                       | (static_cast<uint16_t>(byte) << 8);
        uint8_t hdr[4] = { m_parseFrame.type, m_parseFrame.seq,
                           static_cast<uint8_t>(m_parseFrame.len & 0xFF),
                           static_cast<uint8_t>(m_parseFrame.len >> 8) };
        uint16_t calc = crc16(hdr, 4);
        for (uint16_t i = 0; i < m_parseFrame.len; i++) {
            uint8_t b = m_parseFrame.payload[i];
            calc ^= static_cast<uint16_t>(b) << 8;
            for (int j = 0; j < 8; j++)
                calc = (calc & 0x8000U) ? (calc << 1) ^ 0x1021U : calc << 1;
        }
        m_parseState = ParseState::WAIT_SOF;
        if (calc == rxCrc) { out = m_parseFrame; return true; }
        break;
    }
    }
    return false;
}

// ── RX 스레드 ───────────────────────────────────────────────────
void DeviceComm::rxLoop() {
    uint8_t byte;
    uint32_t rxCount = 0;
    while (m_running) {
        if (readBytes(&byte, 1) == 1) {
            rxCount++;
            if (rxCount % 10 == 0)
                std::cout << "[RX] " << rxCount << " bytes received\n";
            ParsedFrame frame;
            if (parserFeed(byte, frame)) {
                std::cout << "[RX] frame OK type=0x" << std::hex
                          << static_cast<int>(frame.type) << std::dec << "\n";
                auto now = std::chrono::steady_clock::now().time_since_epoch();
                m_lastRxMs = std::chrono::duration_cast<
                    std::chrono::milliseconds>(now).count();
                FrameCallback cb;
                {
                    std::lock_guard<std::mutex> lk(m_callbackMutex);
                    cb = m_callback;
                }
                if (cb) cb(frame);
            }
        }
    }
}

// ── sendCommand ─────────────────────────────────────────────────
bool DeviceComm::sendCommand(uint8_t type, const uint8_t* payload, uint16_t len) {
    std::lock_guard<std::mutex> sendLk(m_sendMutex);  // 재진입 방지

    uint8_t  buf[PROTO_MAX_FRAME];
    uint16_t frameLen = buildFrame(type, m_txSeq, payload, len, buf);

    std::atomic<bool> acked{false};
    uint8_t expectedSeq = m_txSeq;

    FrameCallback prev;
    {
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        prev = m_callback;
        m_callback = [&](const ParsedFrame& f) {
            if (f.type == MSG_ACK && f.payload[0] == expectedSeq)
                acked = true;
            if (prev) prev(f);
        };
    }

    constexpr int MAX_RETRY  = 3;
    constexpr int TIMEOUT_MS = 1000;
    bool ok = false;

    for (int i = 0; i < MAX_RETRY && !ok; i++) {
        writeBytes(buf, frameLen);
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(TIMEOUT_MS);
        while (std::chrono::steady_clock::now() < deadline) {
            if (acked) { ok = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        m_callback = prev;
    }
    m_txSeq++;
    return ok;
}

// ── 플랫폼 UART I/O ─────────────────────────────────────────────
int DeviceComm::readBytes(uint8_t* buf, int len) {
#ifdef _WIN32
    DWORD read = 0;
    ReadFile(m_fd, buf, len, &read, nullptr);
    return static_cast<int>(read);
#else
    return static_cast<int>(::read(m_fd, buf, len));
#endif
}

bool DeviceComm::writeBytes(const uint8_t* buf, int len) {
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(m_fd, buf, len, &written, nullptr);
    return static_cast<int>(written) == len;
#else
    return ::write(m_fd, buf, len) == len;
#endif
}
