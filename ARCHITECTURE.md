# 시스템 아키텍처 개요

SECS/GEM 기반 장비 상태 모니터링 시스템 — 3계층 구조

---

## 1. 전체 구조

```
┌─────────────────────────────────────────────────────────────────────┐
│                        3계층: HOST (macOS)                          │
│                                                                     │
│   ┌─────────────┐   ┌─────────────┐   ┌──────────┐   ┌─────────┐  │
│   │ 상태 화면    │   │ 센서 화면   │   │ 알람 화면 │   │ 명령 UI │  │
│   │ IDLE/RUN..  │   │ 온도/습도.. │   │ ALID 1~4 │   │ S2F41   │  │
│   └─────────────┘   └─────────────┘   └──────────┘   └─────────┘  │
└──────────────────────────────┬──────────────────────────────────────┘
                               │  HSMS (TCP/IP : 5000)
                               │  SECS-II 메시지
                               │
┌──────────────────────────────▼──────────────────────────────────────┐
│                     2계층: EC (Windows)                             │
│                                                                     │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────┐  ┌──────────┐ │
│  │StateMachine │  │ AlarmManager │  │ HsmsServer  │  │DeviceComm│ │
│  │IDLE/RUNNING │  │ ALID 1~4    │  │S1F/S2F/S5F/ │  │UART 파싱 │ │
│  │ALARM/ERROR  │  │ 활성 추적   │  │S6F/S9F      │  │ACK 재전송│ │
│  └──────┬──────┘  └──────┬───────┘  └──────┬──────┘  └─────┬────┘ │
│         └────────────────┴──────────────────┴───────────────┘      │
│                          콜백 / 이벤트 연결                          │
└──────────────────────────────┬──────────────────────────────────────┘
                               │  UART (115200 bps)
                               │  바이너리 프레임 프로토콜
                               │
┌──────────────────────────────▼──────────────────────────────────────┐
│                   1계층: STM32 펌웨어 (NUCLEO-F411RE)               │
│                                                                     │
│  ┌──────────────┐  ┌──────────┐  ┌───────────┐  ┌───────────────┐  │
│  │ TaskSensor   │  │ TaskComm │  │TaskButton │  │ TaskWatchdog  │  │
│  │ SHT31/INA219 │  │ UART 송수│  │ 버튼 이벤 │  │ 태스크 감시  │  │
│  │ 1초 주기     │  │ 신/ACK  │  │ 트 전송   │  │ IWDG 피드    │  │
│  └──────────────┘  └──────────┘  └───────────┘  └───────────────┘  │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐                                │
│  │TaskActuator  │  │TaskHeartbeat │                                │
│  │팬/부저/LED   │  │5초 주기 HB   │                                │
│  │GPIO 제어     │  │전송          │                                │
│  └──────────────┘  └──────────────┘                                │
│                                                                     │
│    SHT31──I2C──┐   INA219──I2C──┐   팬   부저   LED   버튼        │
│                └───────────────▶│   GPIO 제어 (PA5, PC13)          │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. 계층별 역할

| 계층 | 플랫폼 | 핵심 역할 |
|------|--------|----------|
| **STM32 펌웨어** | NUCLEO-F411RE | 센서 수집, 액추에이터 제어, EC와 UART 통신 |
| **EC 소프트웨어** | Windows PC | GEM 상태 머신, 알람/이벤트 관리, HSMS 서버 |
| **Host 프로그램** | macOS | 장비 상태 표시, 원격 명령 전송 |

---

## 3. STM32 펌웨어 내부 구조

### FreeRTOS 태스크 구조

```
┌─────────────────────────────────────────────────────┐
│               FreeRTOS 스케줄러 (1kHz SysTick)       │
│                                                     │
│  ┌─────────────────────────────────────────────┐   │
│  │  TaskWatchdog  [osPriorityRealtime]          │   │
│  │  ┌──────────────────────────────────────┐   │   │
│  │  │ 100ms마다 watchdog_monitor() 호출     │   │   │
│  │  │ 각 태스크 lastCheckinMs 검사          │   │   │
│  │  │ elapsed > timeout → 태스크 재시작     │   │   │
│  │  │ 모두 정상 → HAL_IWDG_Refresh()       │   │   │
│  │  └──────────────────────────────────────┘   │   │
│  └─────────────────────────────────────────────┘   │
│                                                     │
│  ┌──────────────────┐  ┌──────────────────┐        │
│  │ TaskComm [High]  │  │ TaskButton [High] │        │
│  │ UART 수신 파싱   │  │ 버튼 GPIO 감지   │        │
│  │ 프레임 빌드/송신 │  │ xQueueButtonEvent │        │
│  │ ACK/NACK 처리   │  │ → xQueueTxFrame  │        │
│  └──────────────────┘  └──────────────────┘        │
│                                                     │
│  ┌──────────────────┐  ┌──────────────────┐        │
│  │ TaskSensor       │  │ TaskActuator     │        │
│  │ [Normal]         │  │ [Normal]         │        │
│  │ 1초 주기         │  │ xQueueRxCmd 대기 │        │
│  │ SHT31/INA219     │  │ 팬/부저/LED GPIO │        │
│  │ → xQueueTxFrame  │  │ 제어             │        │
│  └──────────────────┘  └──────────────────┘        │
│                                                     │
│  ┌──────────────────┐                              │
│  │ TaskHeartbeat    │                              │
│  │ [Low]            │                              │
│  │ 5초 주기         │                              │
│  │ HB_REQ 전송      │                              │
│  └──────────────────┘                              │
└─────────────────────────────────────────────────────┘

  큐 구조:
  xQueueTxFrame  ──→ TaskComm이 소비 → UART 송신
  xQueueRxCmd    ──→ TaskActuator가 소비 → GPIO 제어
  xQueueButton   ──→ TaskButton이 생산 → TaskComm이 전달
```

### 태스크 우선순위와 Watchdog 타임아웃

```
우선순위 (높음 → 낮음)          Watchdog 타임아웃
──────────────────────────────────────────────────
Realtime  TaskWatchdog          (자기 자신은 IWDG가 감시)
High      TaskComm              3,000ms (주기 없음)
High      TaskButton            3,000ms
Normal    TaskSensor            3,000ms (1초 × 3)
Normal    TaskActuator          3,000ms
Low       TaskHeartbeat        15,000ms (5초 × 3)
```

### 이중 Watchdog 구조

```
[소프트웨어 Task Watchdog]          [하드웨어 IWDG]
각 태스크 → checkin()               TaskWatchdog 정상 시
     ↓                                   ↓
TaskWatchdog 100ms마다 감시         HAL_IWDG_Refresh()
     ↓                                   ↓
타임아웃 → 해당 태스크만 재시작     TaskWatchdog hang →
     ↓                              피드 끊김 → 8초 후
RESTART_REASON → EC 전송           MCU 전체 리셋
```

---

## 4. STM32 ↔ EC 통신 프로토콜

### 프레임 구조

```
 1B     1B     1B    1B    1B    0~64B      1B     1B
┌────┬──────┬─────┬──────┬──────┬─────────┬──────┬──────┐
│SOF │ TYPE │ SEQ │LEN_L │LEN_H │ PAYLOAD │CRC_L │CRC_H │
│0xAA│      │     │      │      │         │      │      │
└────┴──────┴─────┴──────┴──────┴─────────┴──────┴──────┘
                                            └── CRC16-CCITT
```

### 메시지 타입

```
STM32 → EC                          EC → STM32
─────────────────────────────────   ────────────────────────
0x01  SENSOR_DATA   (22B)           0x10  CMD_FAN     (1B)
0x02  BUTTON_EVENT  (6B)            0x11  CMD_BUZZER  (1B)
0x03  RESTART_REASON(3B)            0x12  CMD_LED     (1B)
0x04  ACK           (1B)            0x13  CMD_STATE_SYNC
0x05  NACK          (1B)            0x04  ACK         (1B)
0x21  HEARTBEAT_ACK                 0x20  HEARTBEAT_REQ
```

### 신뢰성 메커니즘

```
EC가 명령 전송
    ↓
STM32 ACK 대기 (1초 타임아웃)
    ├── ACK 수신 → 성공
    └── 타임아웃 → 재전송 (최대 3회)
                      └── 3회 실패 → 통신 오류 처리
```

---

## 5. EC 소프트웨어 내부 구조

### 스레드 구조

```
┌─────────────────────────────────────────────────────────┐
│  Main Thread (1초 루프)                                  │
│  └── UART 수신 타임아웃 감시 (10초)                      │
│       └── 초과 시 ALID-4 알람 발생                       │
└──────┬──────────────────────────────────────────────────┘
       │ 초기화 시 콜백 등록
       │
       ├── DeviceComm
       │   └── RX Thread ─── UART 수신 → 파싱 → FrameCallback 호출
       │
       └── HsmsServer
           ├── Accept Thread ─── TCP 연결 수락 (port 5000)
           └── Session Thread ── HSMS 메시지 수신/송신
```

### 모듈 간 콜백 연결

```
                    [main.cpp 초기화]
                         │
        ┌────────────────┼────────────────┐
        │                │                │
        ▼                ▼                ▼
  StateChangeCallback  FrameCallback  (직접 참조)
        │                │
        │                │
┌───────┴──────┐  ┌──────┴──────────────────────────────────┐
│ StateMachine │  │ DeviceComm (RX Thread)                   │
│              │  │                                          │
│ 상태 전이 시  │  │ MSG_SENSOR_DATA → 임계값 검사            │
│ 콜백 호출    │  │                 → AlarmManager 호출       │
│              │  │                 → S6F11 CEID-8 전송       │
│              │  │ MSG_BUTTON_EVENT → CMD_START/STOP         │
│              │  │ MSG_RESTART_REASON → 로그                 │
│              │  │ MSG_HEARTBEAT_REQ → 로그                  │
└──────────────┘  └──────────────────────────────────────────┘
```

### 상태 머신 전이

```
             [STM32 연결 완료]
                    │
                    ▼
     ┌─────────── IDLE ────────────┐
     │              │              │
  [START]        [ALARM]        [ERROR]
  인터록 통과    조건 발생       조건 발생
     │              │              │
     ▼              ▼              ▼
  RUNNING ──[ALARM]──→ ALARM    ERROR
     │                  │          │
  [STOP]           [RESET /    [RESET]
     │             조건 해소]      │
     ▼                  │          │
   IDLE ←───────────────┘          │
     ▲                              │
     └──────────────────────────────┘

  상태 전이 시 부수 동작:
  IDLE → RUNNING : CMD_FAN(ON)  전송 + S6F11 CEID-3
  RUNNING → IDLE : CMD_FAN(OFF) 전송 + S6F11 CEID-4
  * → ALARM      : S5F1 Alarm Report 전송
  ALARM → IDLE   : S5F1 Alarm Clear 전송
```

### 알람 관리

```
알람 발생 조건                    ALID   Severity
─────────────────────────────────────────────────
온도 > 80°C                        1     WARN
전류 > 임계값                       2     WARN
센서 오류 3회 연속                  3     WARN
UART 수신 없음 10초 이상            4     WARN

AlarmManager.checkAndSet(id)
    → m_alarms[id] = true
    → sm.processEvent(EVENT_ALARM)   ← 상태 머신 ALARM 전이
    → hsmsSrv.sendAlarmReport()      ← S5F1 Host 전송

AlarmManager.tryClear(id)
    → m_alarms[id] = false
    → hasActiveAlarm() == false?
        → sm.processEvent(ALARM_CLEAR) ← IDLE 복귀
        → hsmsSrv.sendAlarmReport()    ← S5F1 CLEAR 전송
```

---

## 6. EC ↔ Host 통신 프로토콜

### HSMS 프레임 구조

```
 4B         2B          1B       1B       4B        가변
┌──────────┬───────────┬────────┬────────┬─────────┬──────────┐
│  LENGTH  │SESSION_ID │ STREAM │  FUNC  │ HEADER  │  BODY    │
│ Big-endian│           │ 0~127  │ 1~255  │         │ SECS-II  │
└──────────┴───────────┴────────┴────────┴─────────┴──────────┘
```

### 구현된 SECS-II 메시지

```
Host → EC                           EC → Host
──────────────────────────────────  ──────────────────────────────────
S1F1  Are You There                 S1F2  On Line Data
S1F13 Establish Communication       S1F14 Establish Communication Ack
S1F3  Status Variable Request       S1F4  Status Variable Data
S2F41 Remote Command                S2F42 Remote Command Ack
S5F5  Alarm List Request            S5F6  Alarm List Data
                                    S5F1  Alarm Report (비동기)
                                    S6F11 Event Report (비동기)
                                    S9F*  Error Response
```

### Remote Command 처리 흐름

```
Host → S2F41 RCMD="START"
    │
    ▼
HsmsServer.handleS2F41()
    │
    ├── 인터록 검사
    │   ├── 활성 알람 있음? → 거부 (S2F42 CMDA≠0)
    │   ├── STM32 연결 안 됨? → 거부
    │   └── 조건 충족 → 수락
    │
    ▼
sm.processEvent(CMD_START)
    │
    ▼
StateChangeCallback 호출 (main.cpp)
    │
    ├── devComm.sendCommand(CMD_FAN, ON) → STM32 UART
    └── hsmsSrv.sendEventReport(CEID-3)  → Host S6F11
    │
    ▼
Host ← S2F42 ACK
Host ← S6F11 CEID-5 (CommandReceived)
```

### Collection Event (CEID) 목록

```
CEID   이벤트명                  발생 조건
─────────────────────────────────────────────────
 1     EquipmentOnline           HSMS 세션 수립
 3     ProcessStart              IDLE → RUNNING
 4     ProcessComplete           RUNNING → IDLE
 5     CommandReceived           S2F41 수신 (성공/거부)
 6     AlarmSet                  알람 발생
 7     AlarmCleared              알람 해제
 8     SensorDataReport          센서 데이터 수신 (1초 주기)
```

---

## 7. 전체 데이터 흐름 시나리오

### 시나리오 A — 정상 운영 (센서 → Host 보고)

```
STM32                    EC                       Host
  │                       │                         │
  │── SENSOR_DATA(0x01) ──▶│                         │
  │                       │ 임계값 검사              │
  │◀── ACK(0x04) ─────────│ 정상이면 알람 없음        │
  │                       │                         │
  │                       │── S6F11 CEID-8 ────────▶│
  │                       │   (온도/습도/전류/전압)   │
```

### 시나리오 B — 원격 명령 (START)

```
STM32                    EC                       Host
  │                       │                         │
  │                       │◀── S2F41 START ─────────│
  │                       │ 인터록 검사 OK            │
  │◀── CMD_FAN(ON) ───────│                         │
  │── ACK ───────────────▶│                         │
  │                       │── S2F42 ACK ───────────▶│
  │                       │── S6F11 CEID-5 ────────▶│
```

### 시나리오 C — 알람 발생

```
STM32                    EC                       Host
  │                       │                         │
  │── SENSOR_DATA ────────▶│                         │
  │   (온도 > 80°C)        │ checkAlarm → SET        │
  │◀── ACK ───────────────│ → ALARM 상태 전이        │
  │                       │── S5F1(ALID=1, SET) ───▶│
  │◀── CMD_FAN(OFF) ───────│ (안전 정지)              │
  │◀── CMD_BUZZER(ON) ─────│                         │
  │◀── CMD_LED(ALARM) ─────│                         │
```

### 시나리오 D — Watchdog 복구

```
STM32                    EC                       Host
  │ [태스크 hang]          │                         │
  │                       │ heartbeat 끊김           │
  │ TaskWatchdog 감지      │                         │
  │ 해당 태스크 재시작      │                         │
  │── RESTART_REASON ─────▶│                         │
  │◀── ACK ───────────────│── S6F11 CEID-13 ───────▶│
  │                       │   (SystemRecovery)       │
```

---

## 8. 시스템 신뢰성 구조 요약

```
장애 유형                  복구 메커니즘              복구 주체
──────────────────────────────────────────────────────────────
태스크 hang (Normal/High)  Task Watchdog 재시작       STM32 SW
TaskWatchdog hang          IWDG 8초 → MCU 리셋        STM32 HW
센서 1개 장애              Graceful Degradation       STM32 SW
UART 통신 중단             안전 상태 유지 + ALID-4    STM32 / EC
HSMS 세션 단절             재연결 대기 (30초)          EC / Host
EC 내부 오류               ERROR 상태 → EC 재시작     운영자
```
