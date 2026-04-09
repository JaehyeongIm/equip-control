# 장비 제어 시스템 — 다이어그램 문서

> SECS/GEM 기반 3계층 장비 제어 시스템 (STM32 FW / EC / Host)

---

## 1. 전체 구조 블록 다이어그램

```mermaid
block-beta
  columns 3

  block:host:3
    label["**Layer 3 · Host**\n(Python, macOS)"]
    H_UI["Console UI\n(curses)"]
    H_MH["MessageHandler\n(S1F14 / S5F1 / S6F11 / S2F42)"]
    H_HSMS["HsmsClient\n(SEMI E37)"]
  end

  space:1
  LINK1[" HSMS · TCP 5000\n SECS-II 메시지 "]:1
  space:1

  block:ec:3
    label2["**Layer 2 · EC**\n(C++, Windows)"]
    EC_SM["StateMachine\n(IDLE/RUNNING/ALARM/ERROR)"]
    EC_AM["AlarmManager\n(ALID 1~4)"]
    EC_HS["HsmsServer\n(SEMI E37 Passive)"]
    EC_DC["DeviceComm\n(UART 래퍼)"]
  end

  space:1
  LINK2[" UART 115200 bps\n 바이너리 프레임 (SOF 0xAA) "]:1
  space:1

  block:fw:3
    label3["**Layer 1 · STM32 Firmware**\n(C, FreeRTOS, NUCLEO-F411RE)"]
    FW_TS["TaskSensor\n(SHT31 / INA219 · 1s)"]
    FW_TC["TaskComm\n(UART RX/TX)"]
    FW_TB["TaskButton\n(GPIO 이벤트)"]
    FW_TA["TaskActuator\n(LED / Fan / Buzzer)"]
    FW_TH["TaskHeartbeat\n(5s 주기)"]
    FW_TW["TaskWatchdog\n(100ms 감시 + IWDG 피드)"]
  end
```

```mermaid
graph TB
  subgraph HOST["Layer 3 · Host (Python / macOS)"]
    direction LR
    H_CLI["CLI 입력\nSTART / STOP / RESET"]
    H_MH["MessageHandler"]
    H_HSMS["HsmsClient"]
    H_UI["Console UI\n(curses)"]
    H_CLI --> H_HSMS
    H_HSMS -->|"S2F41 원격 명령"| H_MH
    H_MH -->|상태·알람·센서 반영| H_UI
  end

  subgraph EC["Layer 2 · Equipment Controller (C++ / Windows)"]
    direction LR
    EC_HS["HsmsServer\n(TCP :5000)"]
    EC_SM["StateMachine\nIDLE / RUNNING\nALARM / ERROR"]
    EC_AM["AlarmManager\nALID 1~4"]
    EC_DC["DeviceComm\n(UART)"]
    EC_HS -->|"S2F41 START/STOP/RESET"| EC_SM
    EC_SM -->|"상태 전이 콜백"| EC_HS
    EC_SM -->|"CMD_LED / CMD_FAN"| EC_DC
    EC_AM -->|"processEvent(ALARM/CLEAR)"| EC_SM
    EC_AM -->|"S5F1 알람 보고"| EC_HS
    EC_DC -->|"SENSOR_DATA 임계값"| EC_AM
  end

  subgraph FW["Layer 1 · STM32 Firmware (C / FreeRTOS)"]
    direction LR
    FW_TS["TaskSensor\n1초 주기\nSHT31 / INA219"]
    FW_TC["TaskComm\nUART RX/TX\nACK/NACK"]
    FW_TB["TaskButton\nGPIO 이벤트"]
    FW_TA["TaskActuator\nLED PA5\nFan / Buzzer"]
    FW_TH["TaskHeartbeat\n5초 주기"]
    FW_TW["TaskWatchdog\n100ms 감시\nIWDG 피드"]
    QTX[("xQueueTxFrame")]
    QRX[("xQueueRxCmd")]
    FW_TS -->|"SENSOR_DATA"| QTX
    FW_TB -->|"BUTTON_EVENT"| QTX
    FW_TH -->|"HEARTBEAT_REQ"| QTX
    QTX --> FW_TC
    FW_TC -->|"CMD 프레임"| QRX
    QRX --> FW_TA
    FW_TW -.->|"100ms 체크인 감시"| FW_TS & FW_TC & FW_TB & FW_TA & FW_TH
  end

  H_HSMS <-->|"HSMS / SECS-II\nTCP 5000"| EC_HS
  EC_DC <-->|"UART 115200 bps\nCRC16-CCITT"| FW_TC
```

**계층 요약**

| 계층 | 플랫폼 | 주요 역할 | 통신 |
|------|--------|----------|------|
| Layer 1 · FW | STM32 NUCLEO-F411RE | 센서 수집, 액추에이터 제어, 실시간 태스크 | UART (115200 bps) |
| Layer 2 · EC | Windows PC (C++) | GEM 상태 머신, 알람/이벤트 관리, HSMS 서버 | TCP 5000 |
| Layer 3 · Host | macOS (Python) | 모니터링 UI, 원격 명령 전송 | HSMS 클라이언트 |

---

## 2. 장비 상태머신 다이어그램

```mermaid
stateDiagram-v2
  [*] --> IDLE : 시스템 초기화

  IDLE --> RUNNING   : CMD_START\n(인터록 OK — 활성 알람 없음)
  IDLE --> ALARM     : EVENT_ALARM\n(센서 임계값 초과 / 통신 오류)
  IDLE --> ERROR     : EVENT_ERROR\n(복구 불가 심각 오류)

  RUNNING --> IDLE   : CMD_STOP\n(정상 정지)
  RUNNING --> ALARM  : EVENT_ALARM\n(운전 중 알람 발생)
  RUNNING --> ERROR  : EVENT_ERROR

  ALARM --> IDLE     : ALARM_CLEAR\n(알람 해소 + CMD_RESET)
  ALARM --> ERROR    : EVENT_ERROR

  ERROR --> IDLE     : CMD_RESET\n(수동 리셋)

  note right of IDLE
    LED: LED_STATE_IDLE (0x01)
    FW 동작: 대기
  end note

  note right of RUNNING
    LED: LED_STATE_RUN (0x02)
    S6F11 CEID-3 → Host
    (ProcessStart)
  end note

  note right of ALARM
    LED: LED_STATE_ALARM (0x03)
    Fan OFF (안전 정지)
    S5F1 ALID 보고 → Host
  end note

  note right of ERROR
    LED: LED_STATE_ERROR (0x04)
    모든 액추에이터 정지
    수동 복구 필요
  end note
```

**알람 ID (ALID) 및 발생 조건**

| ALID | 이름 | 발생 조건 | 임계값 |
|------|------|---------|--------|
| 1 | TEMP_HIGH | 온도 초과 | > 80 °C |
| 2 | HUMIDITY_HIGH | 습도 초과 | > 95 % |
| 3 | SENSOR_ERROR | SHT31 / INA219 오류 | flags = 0 |
| 4 | UART_COMM_ERROR | Heartbeat 무응답 | 10초 초과 |

**인터록 규칙**

- `ALARM` 또는 `ERROR` 상태에서는 `CMD_START` 거부 → S2F42 NACK 반환
- `ALARM_CLEAR`는 모든 활성 알람이 해소된 경우에만 전이
- `EVENT_ERROR`는 어느 상태에서든 즉시 `ERROR`로 전이

---

## 3. 메시지 중심 시퀀스 다이어그램

### 3-A. 세션 수립 및 초기화

```mermaid
sequenceDiagram
  participant H  as Host (Python)
  participant EC as EC (C++)
  participant FW as STM32 FW

  Note over H,EC: HSMS 세션 수립
  H  ->> EC : [TCP Connect] port 5000
  H  ->> EC : S1F13  Establish Communication Req
  EC ->> H  : S1F14  Establish Communication Ack (COMMACK=0)
  EC ->> H  : S6F11  CEID-1  Equipment Online

  Note over H,EC: 초기 상태 조회
  H  ->> EC : S1F1   Are You There?
  EC ->> H  : S1F2   On Line Data (상태=IDLE)

  H  ->> EC : S1F3   Status Variable Request
  EC ->> H  : S1F4   Status Variable Data (온도/습도/전류/전압)

  Note over EC,FW: 하트비트 (5초 주기)
  loop 5초마다
    FW ->> EC : MSG_HEARTBEAT_REQ (0x20)
    EC ->> FW : MSG_HEARTBEAT_ACK (0x21)
  end
```

### 3-B. 정상 운전 — START → 센서 보고 → STOP

```mermaid
sequenceDiagram
  participant H  as Host
  participant EC as EC
  participant FW as STM32 FW

  H  ->> EC : S2F41  Remote Command: START
  Note over EC: 인터록 검사\n(활성 알람 없음 확인)
  Note over EC: StateMachine\nIDLE → RUNNING

  EC ->> FW : MSG_CMD_LED  LED_STATE_RUN (0x02)
  FW ->> EC : MSG_ACK

  EC ->> H  : S6F11  CEID-3  ProcessStart
  EC ->> H  : S2F42  Command Ack (ACK=0)

  Note over FW: 1초마다 센서 수집
  loop 1초마다
    FW ->> EC : MSG_SENSOR_DATA (온도/습도/전류/전압)
    EC ->> FW : MSG_ACK
    Note over EC: 임계값 정상
    EC ->> H  : S6F11  CEID-8  SensorDataReport\n(temp, humi, curr, volt)
  end

  H  ->> EC : S2F41  Remote Command: STOP
  Note over EC: StateMachine\nRUNNING → IDLE

  EC ->> FW : MSG_CMD_LED  LED_STATE_IDLE (0x01)
  FW ->> EC : MSG_ACK

  EC ->> H  : S6F11  CEID-4  ProcessComplete
  EC ->> H  : S2F42  Command Ack (ACK=0)
```

### 3-C. 알람 발생 및 복구

```mermaid
sequenceDiagram
  participant H  as Host
  participant EC as EC
  participant FW as STM32 FW

  FW ->> EC : MSG_SENSOR_DATA (온도 = 85 °C)
  EC ->> FW : MSG_ACK

  Note over EC: 임계값 초과!\nAlarmManager.checkAndSet(ALID=1)\nStateMachine: RUNNING → ALARM

  EC ->> FW : MSG_CMD_LED  LED_STATE_ALARM (0x03)
  EC ->> FW : MSG_CMD_FAN  OFF
  FW ->> EC : MSG_ACK

  EC ->> H  : S5F1   Alarm Report  ALID=1  ALCD=SET
  EC ->> H  : S6F11  CEID-6  AlarmSet

  Note over H: 알람 표시\n운영자 확인

  H  ->> EC : S2F41  Remote Command: RESET
  Note over EC: AlarmManager.tryClear(ALID=1)\n활성 알람 없음 확인\nStateMachine: ALARM → IDLE

  EC ->> FW : MSG_CMD_LED  LED_STATE_IDLE (0x01)
  FW ->> EC : MSG_ACK

  EC ->> H  : S5F1   Alarm Report  ALID=1  ALCD=CLEAR
  EC ->> H  : S6F11  CEID-7  AlarmCleared
  EC ->> H  : S2F42  Command Ack (ACK=0)
```

### 3-D. UART 프레임 재전송 (ACK 타임아웃)

```mermaid
sequenceDiagram
  participant EC as EC (DeviceComm)
  participant FW as STM32 TaskComm

  EC ->> FW : 프레임 전송 (SEQ=N)
  Note over FW: 수신 실패 또는 처리 지연
  Note over EC: 1초 타임아웃

  EC ->> FW : 재전송 #1 (SEQ=N)
  Note over EC: 1초 타임아웃

  EC ->> FW : 재전송 #2 (SEQ=N)
  FW ->> EC : MSG_ACK (SEQ=N)
  Note over EC: 전송 성공\n(최대 3회 시도)
```

### 3-E. Task Watchdog 복구

```mermaid
sequenceDiagram
  participant TW  as TaskWatchdog
  participant TS  as TaskSensor
  participant TC  as TaskComm
  participant EC  as EC
  participant H   as Host

  Note over TS: hang 발생\n체크인 중단

  loop 100ms마다
    TW ->> TW : 각 태스크 elapsed 확인
  end

  Note over TW: TaskSensor elapsed > 3000ms\n타임아웃 감지

  TW ->> TS : osThreadTerminate()
  TW ->> TS : osThreadNew() — 재시작

  TW ->> TC : MSG_RESTART_REASON\n(reason=TASK_WD, taskId=SENSOR)
  TC ->> EC : UART 전송
  EC ->> FW : MSG_ACK

  EC ->> H  : S6F11  CEID-13  SystemRecovery\n(재시작 이유 포함)

  Note over TW: 정상 → HAL_IWDG_Refresh()
```

---

## 4. 태스크 인터랙션 다이어그램

```mermaid
graph TD
  subgraph FW["STM32 FreeRTOS — 태스크 인터랙션"]
    direction TB

    subgraph SENSORS["하드웨어 I/O"]
      SHT31["SHT31\n(I2C — 온/습도)"]
      INA219["INA219\n(I2C — 전류/전압)"]
      BTN["버튼\n(GPIO EXTI)"]
      LED["LED PA5\n(GPIO)"]
      BUZZER["부저\n(GPIO)"]
      FAN["팬\n(GPIO)"]
      UART2["UART2\n(USB VCP → EC)"]
      IWDG["IWDG\n(하드웨어 8초)"]
    end

    subgraph QUEUES["IPC 리소스"]
      QTX[("xQueueTxFrame\n송신 큐")]
      QRX[("xQueueRxCmd\n수신 명령 큐")]
      QBTN[("xQueueButtonEvent\n버튼 이벤트 큐")]
      MTX[("xMutexSensorBuf\n센서 버퍼 뮤텍스")]
    end

    subgraph TASKS["FreeRTOS 태스크"]
      TS["TaskSensor\nPriority: Normal(3)\nStack: 256w\n주기: 1,000ms"]
      TC["TaskComm\nPriority: High(4)\nStack: 512w\n이벤트 구동"]
      TB["TaskButton\nPriority: High(4)\nStack: 128w\n이벤트 구동"]
      TA["TaskActuator\nPriority: Normal(3)\nStack: 256w\n이벤트 구동"]
      TH["TaskHeartbeat\nPriority: Low(2)\nStack: 128w\n주기: 5,000ms"]
      TW["TaskWatchdog\nPriority: Realtime(5)\nStack: 256w\n주기: 100ms"]
    end

    SHT31 & INA219 -->|"I2C 읽기"| TS
    TS -->|"SensorDataPayload\n(22 bytes)"| QTX
    TS -->|"체크인 갱신"| TW

    BTN -->|"EXTI ISR"| QBTN
    TB -->|"dequeue"| QBTN
    TB -->|"ButtonEventPayload"| QTX
    TB -->|"체크인 갱신"| TW

    TH -->|"HeartbeatReq 프레임"| QTX
    TH -->|"체크인 갱신"| TW

    QTX -->|"dequeue + 프레임 빌드\n(SOF / CRC16)"| TC
    TC <-->|"HAL_UART_Transmit\nHAL_UART_Receive"| UART2
    TC -->|"수신 CMD 프레임"| QRX
    TC -->|"체크인 갱신"| TW

    QRX -->|"dequeue"| TA
    TA -->|"GPIO Write"| LED & BUZZER & FAN
    TA -->|"체크인 갱신"| TW

    TW -->|"elapsed > timeout\n→ Terminate + New"| TS & TC & TB & TA & TH
    TW -->|"전 태스크 정상\n→ Refresh"| IWDG
    TW -->|"RESTART_REASON 큐 삽입"| QTX
  end
```

**태스크 우선순위 및 Watchdog 타임아웃**

| 태스크 | 우선순위 | 스택 | 실행 주기 | WD 타임아웃 |
|--------|---------|------|---------|-----------|
| TaskWatchdog | Realtime (5) | 256w | 100ms | — (감시자) |
| TaskComm | High (4) | 512w | 이벤트 | 3,000ms |
| TaskButton | High (4) | 128w | 이벤트 | 3,000ms |
| TaskSensor | Normal (3) | 256w | 1,000ms | 3,000ms |
| TaskActuator | Normal (3) | 256w | 이벤트 | 3,000ms |
| TaskHeartbeat | Low (2) | 128w | 5,000ms | 15,000ms |

**이중 Watchdog 구조**

```mermaid
graph LR
  subgraph SW["소프트웨어 Watchdog"]
    CHECKIN["각 태스크\nwatchdog_checkin()"]
    MONITOR["TaskWatchdog\n100ms 감시"]
    RESTART["해당 태스크만\nosThreadNew() 재시작"]
    CHECKIN -->|"체크인 갱신"| MONITOR
    MONITOR -->|"타임아웃 감지"| RESTART
  end

  subgraph HW["하드웨어 IWDG"]
    FEED["HAL_IWDG_Refresh()\n(전 태스크 정상 시)"]
    RESET["MCU 전체 리셋\n(8초 후)"]
    FEED -->|"피드 끊김"| RESET
  end

  MONITOR -->|"정상 상태"| FEED
  MONITOR -->|"TaskWatchdog 자체 hang\n→ 피드 중단"| RESET
```

**메시지 큐 흐름 요약**

```
[생산자]                        [xQueueTxFrame]      [소비자]
─────────────────────────────────────────────────────────────────
TaskSensor  → SENSOR_DATA  ──┐
TaskButton  → BUTTON_EVENT ──┼──→  xQueueTxFrame  ──→  TaskComm  ──→  EC (UART)
TaskHB      → HB_REQ       ──┘
TaskWatchdog→ RESTART_REASON─┘

[생산자]                        [xQueueRxCmd]        [소비자]
─────────────────────────────────────────────────────────────────
TaskComm (수신 CMD) ──────────→  xQueueRxCmd   ──→  TaskActuator ──→ GPIO
                                                                   (LED / Fan / Buzzer)
```

---

## 부록 — 프로토콜 프레임 구조

### STM32 ↔ EC 내부 바이너리 프레임

```
┌──────┬──────┬──────┬──────┬──────┬────────────┬──────┬──────┐
│ SOF  │ TYPE │ SEQ  │LEN_L │LEN_H │  PAYLOAD   │CRC_L │CRC_H │
│ 0xAA │  1B  │  1B  │  1B  │  1B  │  0 ~ 64B   │  1B  │  1B  │
└──────┴──────┴──────┴──────┴──────┴────────────┴──────┴──────┘
  CRC 범위: TYPE ~ PAYLOAD 끝  (CRC16-CCITT, poly=0x1021, init=0xFFFF)
```

| TYPE | 방향 | 이름 | Payload |
|------|------|------|---------|
| 0x01 | FW → EC | MSG_SENSOR_DATA | SensorDataPayload (22B) |
| 0x02 | FW → EC | MSG_BUTTON_EVENT | ButtonEventPayload (1B) |
| 0x03 | FW → EC | MSG_RESTART_REASON | RestartReasonPayload (3B) |
| 0x04 | 양방향 | MSG_ACK | AckPayload (1B) |
| 0x05 | 양방향 | MSG_NACK | (1B) |
| 0x10 | EC → FW | MSG_CMD_FAN | CmdFanPayload (1B) |
| 0x11 | EC → FW | MSG_CMD_BUZZER | CmdBuzzerPayload (1B) |
| 0x12 | EC → FW | MSG_CMD_LED | CmdLedPayload (1B) |
| 0x20 | FW → EC | MSG_HEARTBEAT_REQ | (0B) |
| 0x21 | EC → FW | MSG_HEARTBEAT_ACK | (0B) |

### HSMS 메시지 헤더 (10 bytes)

```
┌────────────┬────────┬──────────┬─────────┬────────┬────────────┐
│ SESSION_ID │ STREAM │ FUNCTION │  FLAGS  │ S-TYPE │  SYS_BYTES │
│    (2B)    │ (1B)   │   (1B)   │  (1B)   │  (1B)  │    (4B)    │
└────────────┴────────┴──────────┴─────────┴────────┴────────────┘
  앞에 LENGTH (4B Big-endian) 포함 시 총 14B 헤더
```

| S/F | 방향 | 이름 | 설명 |
|-----|------|------|------|
| S1F1 | H → EC | Are You There | 장비 상태 조회 |
| S1F2 | EC → H | On Line Data | 상태 응답 |
| S1F13 | H → EC | Establish Communication Req | 세션 수립 요청 |
| S1F14 | EC → H | Establish Communication Ack | 세션 수립 응답 |
| S2F41 | H → EC | Remote Command | START / STOP / RESET |
| S2F42 | EC → H | Remote Command Ack | ACK / NACK |
| S5F1 | EC → H | Alarm Report | 알람 발생/해제 (비동기) |
| S6F11 | EC → H | Event Report | CEID 이벤트 (비동기) |

**Collection Event ID (CEID)**

| CEID | 이벤트 | 발생 시점 |
|------|--------|---------|
| 1 | EquipmentOnline | HSMS 세션 수립 |
| 3 | ProcessStart | IDLE → RUNNING |
| 4 | ProcessComplete | RUNNING → IDLE |
| 6 | AlarmSet | 알람 활성화 |
| 7 | AlarmCleared | 알람 해제 |
| 8 | SensorDataReport | 1초 주기 센서 보고 |
| 13 | SystemRecovery | Task Watchdog 재시작 |
