/* -----------------------------------------------------------------------
 * uart_comm.c — UART 프레임 빌드/파싱, CRC16, 송수신 (SDD Section 5)
 * --------------------------------------------------------------------- */

#include "uart_comm.h"
#include "config.h"
#include "usart.h"
#include "cmsis_os.h"
#include <string.h>

/* ── CRC16-CCITT (poly=0x1021, init=0xFFFF) ── */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000U)
                crc = (crc << 1) ^ 0x1021U;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ── 프레임 빌드 ──
 * outBuf 레이아웃:
 *   SOF(1) TYPE(1) SEQ(1) LEN_L(1) LEN_H(1) PAYLOAD(N) CRC_L(1) CRC_H(1)
 * CRC 계산 범위: TYPE ~ PAYLOAD 끝
 * 반환값: 전체 프레임 길이 */
uint16_t frame_build(uint8_t type, uint8_t seq,
                     const uint8_t *payload, uint16_t payloadLen,
                     uint8_t *outBuf)
{
    outBuf[0] = PROTO_SOF;
    outBuf[1] = type;
    outBuf[2] = seq;
    outBuf[3] = (uint8_t)(payloadLen & 0xFFU);
    outBuf[4] = (uint8_t)(payloadLen >> 8);
    if (payloadLen > 0U && payload != NULL)
        memcpy(&outBuf[5], payload, payloadLen);

    /* CRC: TYPE(1)+SEQ(1)+LEN(2)+PAYLOAD(N) */
    uint16_t crc = crc16_ccitt(&outBuf[1], 4U + payloadLen);
    outBuf[5U + payloadLen] = (uint8_t)(crc & 0xFFU);
    outBuf[6U + payloadLen] = (uint8_t)(crc >> 8);

    return PROTO_HEADER_SIZE + payloadLen + PROTO_CRC_SIZE;
}

/* ── 프레임 파서 (FSM) ──
 * 1바이트씩 입력한다.
 * CRC가 맞는 완성 프레임이 들어오면 outFrame에 쓰고 true를 반환한다.
 * CRC 불일치 시 false 반환 — 호출자(TaskComm)가 NACK를 보낸다. */
typedef enum {
    PS_WAIT_SOF,
    PS_TYPE,
    PS_SEQ,
    PS_LEN_L,
    PS_LEN_H,
    PS_PAYLOAD,
    PS_CRC_L,
    PS_CRC_H
} ParserState_t;

static ParserState_t s_state     = PS_WAIT_SOF;
static ParsedFrame_t s_frame;
static uint16_t      s_payloadIdx;
/* CRC 누적 버퍼: TYPE(1)+SEQ(1)+LEN(2)+PAYLOAD(최대 64) */
static uint8_t       s_crcBuf[4U + PROTO_MAX_PAYLOAD];
static uint16_t      s_crcBufIdx;
static uint8_t       s_crcL;

bool frame_parser_feed(uint8_t byte, ParsedFrame_t *outFrame)
{
    switch (s_state) {

    case PS_WAIT_SOF:
        if (byte == PROTO_SOF) {
            s_crcBufIdx  = 0;
            s_payloadIdx = 0;
            s_state = PS_TYPE;
        }
        break;

    case PS_TYPE:
        s_frame.type         = byte;
        s_crcBuf[s_crcBufIdx++] = byte;
        s_state = PS_SEQ;
        break;

    case PS_SEQ:
        s_frame.seq          = byte;
        s_crcBuf[s_crcBufIdx++] = byte;
        s_state = PS_LEN_L;
        break;

    case PS_LEN_L:
        s_frame.len          = byte;
        s_crcBuf[s_crcBufIdx++] = byte;
        s_state = PS_LEN_H;
        break;

    case PS_LEN_H:
        s_frame.len         |= (uint16_t)byte << 8;
        s_crcBuf[s_crcBufIdx++] = byte;
        if (s_frame.len > PROTO_MAX_PAYLOAD) {
            s_state = PS_WAIT_SOF;  /* 비정상 길이 — 리셋 */
        } else if (s_frame.len == 0U) {
            s_state = PS_CRC_L;
        } else {
            s_state = PS_PAYLOAD;
        }
        break;

    case PS_PAYLOAD:
        s_frame.payload[s_payloadIdx] = byte;
        s_crcBuf[s_crcBufIdx++]       = byte;
        s_payloadIdx++;
        if (s_payloadIdx >= s_frame.len)
            s_state = PS_CRC_L;
        break;

    case PS_CRC_L:
        s_crcL  = byte;
        s_state = PS_CRC_H;
        break;

    case PS_CRC_H: {
        uint16_t rxCrc   = (uint16_t)s_crcL | ((uint16_t)byte << 8);
        uint16_t calcCrc = crc16_ccitt(s_crcBuf, s_crcBufIdx);
        s_state = PS_WAIT_SOF;
        if (rxCrc == calcCrc) {
            *outFrame = s_frame;
            return true;
        }
        /* CRC 불일치: 파서는 버리고 false 반환 */
        break;
    }

    default:
        s_state = PS_WAIT_SOF;
        break;
    }
    return false;
}

/* ── UART 송신 (blocking, timeout=100ms) ── */
void uart_transmit_raw(const uint8_t *buf, uint16_t len)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, 100U);
}

/* ── RX 인터럽트 처리 ──
 * HAL이 1바이트를 수신할 때마다 HAL_UART_RxCpltCallback을 호출한다.
 * 거기서 uart_rx_byte_from_isr()를 부르면 바이트를 RX 큐에 넣는다.
 * TaskComm이 큐에서 꺼내 frame_parser_feed()에 투입한다. */

static uint8_t           s_rxByte;
static osMessageQueueId_t s_rxByteQueue = NULL;

void uart_rx_start(void)
{
    if (s_rxByteQueue == NULL)
        s_rxByteQueue = osMessageQueueNew(CFG_QUEUE_UART_RX_SIZE,
                                          sizeof(uint8_t), NULL);
    HAL_UART_Receive_IT(&huart2, &s_rxByte, 1U);
}

void uart_rx_byte_from_isr(uint8_t byte)
{
    /* ISR 컨텍스트이므로 timeout=0 */
    osMessageQueuePut(s_rxByteQueue, &byte, 0U, 0U);
    /* 다음 1바이트 수신 재등록 */
    HAL_UART_Receive_IT(&huart2, &s_rxByte, 1U);
}

osMessageQueueId_t uart_get_rx_queue(void)
{
    return s_rxByteQueue;
}

/* PA3(RX) 플로팅 등으로 프레이밍 에러 발생 시 RX IT 자동 복구 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        HAL_UART_Receive_IT(huart, &s_rxByte, 1U);
    }
}
