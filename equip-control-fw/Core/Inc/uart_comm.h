#ifndef __UART_COMM_H
#define __UART_COMM_H

/* -----------------------------------------------------------------------
 * uart_comm.h — UART 통신 레이어 인터페이스 (SDD Section 4.5)
 * CRC16 계산, 프레임 빌드, 프레임 파서, 바이트 송신
 * --------------------------------------------------------------------- */

#include "protocol.h"
#include "config.h"
#include "cmsis_os.h"
#include <stdint.h>
#include <stdbool.h>

/* CRC16-CCITT (poly=0x1021, init=0xFFFF) */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len);

/* 프레임 빌드: outBuf에 완성된 프레임을 쓰고 총 길이를 반환 */
uint16_t frame_build(uint8_t type, uint8_t seq,
                     const uint8_t *payload, uint16_t payloadLen,
                     uint8_t *outBuf);

/* 프레임 파서 — 1바이트씩 입력, 완성된 유효 프레임이 있으면 true 반환 */
bool frame_parser_feed(uint8_t byte, ParsedFrame_t *outFrame);

/* UART 원시 바이트 송신 (blocking, timeout=100ms) */
void uart_transmit_raw(const uint8_t *buf, uint16_t len);

/* ISR에서 호출: 수신 바이트를 RX 큐에 삽입 */
void uart_rx_byte_from_isr(uint8_t byte);

/* HAL_UART_Receive_IT 시작 — 초기화 시 1회 호출 */
void uart_rx_start(void);

/* TaskComm에서 호출: RX 바이트 큐 핸들 반환 */
osMessageQueueId_t uart_get_rx_queue(void);

/* 버튼 ISR에서 호출 — freertos.c의 xQueueButtonEvent에 삽입 */
void gpio_button_event_from_isr(uint8_t buttonId);

#endif /* __UART_COMM_H */
