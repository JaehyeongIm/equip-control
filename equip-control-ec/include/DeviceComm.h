#pragma once
#include "Protocol.h"
#include <functional>
#include <string>
#include <atomic>
#include <thread>
#include <cstdint>

// STM32로부터 수신한 프레임 콜백 타입
using FrameCallback = std::function<void(const ParsedFrame&)>;

class DeviceComm {
public:
    explicit DeviceComm(const std::string& portName, uint32_t baudRate = 115200);
    ~DeviceComm();

    bool open();
    void close();
    bool isOpen() const;

    // 수신 콜백 등록 (파싱 완료된 프레임마다 호출)
    void setFrameCallback(FrameCallback cb);

    // STM32로 명령 전송 (ACK 대기, 최대 3회 재전송)
    bool sendCommand(uint8_t type, const uint8_t* payload, uint16_t len);

    // 마지막 수신 시각 (ms) — Heartbeat 감시용
    uint64_t lastRxTimeMs() const;

private:
    void rxLoop();

    static uint16_t crc16(const uint8_t* data, uint16_t len);
    uint16_t buildFrame(uint8_t type, uint8_t seq,
                        const uint8_t* payload, uint16_t payloadLen,
                        uint8_t* outBuf);
    bool parserFeed(uint8_t byte, ParsedFrame& out);

    int  readBytes(uint8_t* buf, int len);
    bool writeBytes(const uint8_t* buf, int len);

    std::string            m_port;
    int                    m_fd;
    uint8_t                m_txSeq;

    FrameCallback          m_callback;
    std::thread            m_rxThread;
    std::atomic<bool>      m_running;
    std::atomic<uint64_t>  m_lastRxMs;

    enum class ParseState {
        WAIT_SOF, TYPE, SEQ, LEN_L, LEN_H, PAYLOAD, CRC_L, CRC_H
    };
    ParseState  m_parseState;
    ParsedFrame m_parseFrame;
    uint16_t    m_parseIdx;
};
