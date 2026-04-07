# Troubleshooting: HOST START → STM32 LD2 제어

## 목표

HOST에서 START 명령을 보내면 EC가 STM32에 `MSG_CMD_LED`를 전송하고,
STM32가 PA5(LD2)를 켜는 흐름을 구현 및 검증한다.

```
HOST ──S2F41 START──► EC ──MSG_CMD_LED──► STM32 ──HAL_GPIO_WritePin──► LD2(PA5)
```

---

## 1단계: 코드 수정

### 문제
- EC가 START 시 `MSG_CMD_FAN`을 전송하고 있었음 (주석에는 "LD2 ON" 이라고 돼있으나 타입이 잘못됨)
- `MSG_CMD_LED` 핸들러가 FW에서 TODO만 있고 구현되지 않음
- EC의 `CmdLedPayload` 구조체 (`r, g, b` 3바이트)와 FW의 `CmdLedPayload_t` 구조체 (`state` 1바이트)가 불일치

### 수정 내용

| 파일 | 변경 내용 |
|---|---|
| `equip-control-ec/src/main.cpp` | `MSG_CMD_FAN` → `MSG_CMD_LED`, `CmdFanPayload` → `CmdLedPayload` |
| `equip-control-ec/include/Protocol.h` | `CmdLedPayload { r, g, b }` → `{ state }` (FW와 동일하게) |
| `equip-control-fw/Core/Src/freertos.c` | `MSG_CMD_LED` 케이스에 `HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, ...)` 구현 |

---

## 2단계: 하드웨어 디버거(브레이크포인트) 시도

### 시도
- STM32CubeIDE에서 `freertos.c`의 `case MSG_CMD_LED` 라인에 브레이크포인트 설정
- Debug(F11) → Resume(F8) 후 HOST에서 START 전송

### 결과
- 브레이크포인트에 걸리지 않음
- `TaskComm`의 `osMessageQueuePut(xQueueRxCmd, ...)` 라인에도 브레이크포인트를 걸었으나 마찬가지로 걸리지 않음

### 판단
- STM32가 `MSG_CMD_LED` 프레임을 수신하지 못하고 있음
- EC → STM32 UART 경로에 문제 있음

---

## 3단계: SWV ITM 디버깅 설정

하드웨어 디버거(브레이크포인트) 방식 대신 `printf` 스타일 로그를 SWV ITM으로 출력하는 방법으로 전환.

### 설정 과정

**1. ITM 출력 리디렉션 (`syscalls.c`)**
```c
// _write 함수를 ITM_SendChar로 리디렉션
int _write(int file, char *ptr, int len) {
    for (int i = 0; i < len; i++)
        ITM_SendChar((uint32_t)*ptr++);
    return len;
}
```
→ `#include "stm32f4xx.h"` 누락으로 컴파일 오류 발생 후 추가

**2. ITM 수동 초기화 (`main.c`)**
```c
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
ITM->LAR  = 0xC5ACCE55UL;
ITM->TCR  = 0x0001000DUL;
ITM->TPR  = 0xFFFFFFFFUL;
ITM->TER  = 0x00000001UL;
```
→ CubeIDE가 자동으로 ITM 활성화를 안 해줘서 펌웨어에서 직접 초기화 필요

**3. `freertos.c`에 `dbg()` 헬퍼 함수 추가**
```c
static void dbg(const char *s) {
    while (*s) ITM_SendChar((uint32_t)*s++);
}
```
→ `printf` → `_write` → ITM 체인을 거치면 `--specs=nano.specs` 버퍼링 문제로 출력 안 됨. ITM 직접 쓰기로 우회

### 겪은 문제들

| 문제 | 원인 | 해결 |
|---|---|---|
| SWV Start Trace 버튼 비활성화 | Resume 후에 Start Trace를 눌러서 | Resume 전에 Start Trace 먼저 클릭 |
| Core Clock 불일치 | F411RE 최대 100MHz이나 실제 클럭은 84MHz | PLL 계산으로 84MHz 확인 후 수정 |
| SWV 콘솔에 아무것도 안 뜸 | ITM 미초기화, printf 버퍼링 | ITM 수동 초기화 + `dbg()` 직접 구현 |
| `BOOT` 중 앞 글자 `BO` 누락 | Start Trace 시작 타이밍이 늦음 | 정상 동작 확인 (`OT` 출력되면 OK) |

### SWV 동작 방식
```
USB 한 케이블에 두 채널 분리:
├── SWD (ST-Link 디버그 채널) → CubeIDE 디버거 + SWV ITM 출력
└── VCP USART2 (PA2/PA3)     → EC UART 통신
```
→ 두 채널이 독립적이므로 디버그 연결 중에도 EC UART 통신 가능

### 디버깅 로그 추가 위치

```
TaskComm  RX 수신 시    → [COMM] frame rx
TaskComm  CMD 큐 삽입 시 → [COMM] cmd queued
TaskComm  TX 전송 시    → [COMM] tx start
TaskActuator 수신 시    → [ACTUATOR] MSG_CMD_LED
TaskActuator GPIO 후    → [ACTUATOR] LD2 ON/OFF
TaskHeartbeat 주기마다  → [HB] tick
```

---

## 4단계: UART 통신 경로 분석

### 확인 순서 (소프트웨어 → 인터페이스 → 하드웨어)

**Step 1 — SWV로 STM32 TX 동작 확인**
- `[COMM] tx start` 로그 확인 → STM32가 실제로 `uart_transmit_raw` 호출 중 ✓

**Step 2 — PuTTY로 COM3 원시 데이터 확인**
- EC 종료 후 PuTTY(115200, 8N1)로 COM3 접속
- 깨진 문자(바이너리) 출력 확인 → STM32 → PC 경로 정상 ✓

**Step 3 — EC rxLoop에 바이트 카운터 추가**
```cpp
// DeviceComm.cpp rxLoop에 추가
rxCount++;
if (rxCount % 10 == 0)
    std::cout << "[RX] " << rxCount << " bytes received\n";
if (parserFeed(byte, frame))
    std::cout << "[RX] frame OK type=0x" << std::hex << frame.type << "\n";
```
- `[RX] frame OK type=0x1` (MSG_SENSOR_DATA) 수신 확인 → EC 수신 정상 ✓

**Step 4 — sendCommand 결과 로깅**
```cpp
bool ok = devComm.sendCommand(MSG_CMD_LED, ...);
std::cout << "[EC] sendCommand LED ON: " << (ok ? "ACK" : "TIMEOUT") << "\n";
```
- 결과: `TIMEOUT` → STM32가 ACK를 보내지 않음

---

## 5단계: 근본 원인 발견

### 버그 위치
`freertos.c` — `StartTaskComm()` 내 TX ACK 대기 내부 루프

### 버그 설명

```
TaskComm 동작 흐름:
1. xQueueTxFrame에서 센서 데이터 꺼냄
2. uart_transmit_raw()로 전송
3. EC의 ACK를 기다리는 내부 루프 진입 (최대 1000ms × 3회 = 3초)
   └─ rxByteQ에서 바이트를 꺼내 frame_parser_feed()에 투입
   └─ MSG_ACK가 오면 break
   └─ MSG_ACK가 아닌 프레임은 그냥 버림  ← 여기가 문제
4. EC는 센서 데이터에 ACK를 보내지 않음
   → 내부 루프가 항상 3초 timeout
   → TaskComm은 거의 항상 내부 루프에 갇혀 있음
```

EC가 이 3초 window 안에 `MSG_CMD_LED`를 보내면:
- 내부 루프가 바이트를 소비 및 파싱
- `MSG_ACK`가 아니므로 버림
- `xQueueRxCmd`에 넣지 않음
- STM32가 EC에 ACK도 안 보냄
- EC의 `sendCommand` → TIMEOUT

### 수정 코드

```c
// 내부 루프에서 CMD 프레임 도착 시 처리 추가
if (frame_parser_feed(b, &ackFrame)) {
    if (ackFrame.type == MSG_ACK && ackFrame.payload[0] == txSeq) {
        acked = 1U;
        break;
    }
    /* CMD 프레임이 도착하면 ACK 전송 + 큐에 전달 */
    if (ackFrame.type == MSG_CMD_FAN    ||
        ackFrame.type == MSG_CMD_BUZZER ||
        ackFrame.type == MSG_CMD_LED    ||
        ackFrame.type == MSG_CMD_STATE_SYNC) {
        AckPayload_t ack2 = { .ack_seq = ackFrame.seq };
        uint8_t ackBuf[PROTO_MAX_FRAME];
        uint16_t ackLen = frame_build(MSG_ACK, txSeq,
                                      (uint8_t *)&ack2, sizeof(ack2), ackBuf);
        uart_transmit_raw(ackBuf, ackLen);
        osMessageQueuePut(xQueueRxCmd, &ackFrame, 0U, 0U);
    }
}
```

---

## 최종 결과

- HOST START → EC `MSG_CMD_LED` 전송 → STM32 수신 + ACK → LD2 ON ✓
- HOST STOP  → EC `MSG_CMD_LED` 전송 → STM32 수신 + ACK → LD2 OFF ✓

---

## 교훈

1. **프로토콜 구조체는 양쪽(EC/FW)이 반드시 일치해야 한다**
   - EC의 `CmdLedPayload { r, g, b }`와 FW의 `CmdLedPayload_t { state }`가 달라 혼란 발생

2. **TX와 RX를 동시에 처리하는 루프는 프레임을 버리지 않아야 한다**
   - ACK 대기 중 수신된 CMD 프레임을 처리하지 않으면 유실됨

3. **디버깅 순서: 소프트웨어 → 인터페이스 → 하드웨어**
   - 브레이크포인트로 코드 도달 확인
   - SWV ITM으로 런타임 흐름 확인
   - PuTTY로 UART 원시 데이터 확인
   - 바이트 카운터로 EC 수신 확인

4. **SWV ITM 설정 시 주의사항**
   - Core Clock을 실제 클럭(PLL 계산값)과 정확히 일치시켜야 함
   - Resume 전에 Start Trace를 먼저 클릭해야 함
   - `printf` 버퍼링 문제 → ITM 직접 쓰기로 우회
