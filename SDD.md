# 소프트웨어 설계 문서 (SDD)

## 프로젝트명
SECS/GEM 기반 장비 상태 모니터링 시스템

## 문서 버전
v1.0

## 준거 표준
IEEE 1016-2009 (Systems and Software Engineering — Software Design Descriptions)

## 작성일
2026-04-04

## 작성자
임재형

## 관련 문서
- Software Requirements Specification (SRS) v1.3
- Interface Specification (IFS): STM32↔EC 및 EC↔Host 메시지 포맷 상세

---

# 1. 개요

## 1.1 목적
본 문서는 SRS v1.3에 정의된 요구사항을 충족하는 소프트웨어 모듈 구조, 인터페이스, 상태 머신, 데이터 흐름을 설계한다.

## 1.2 범위
- 1계층: STM32 NUCLEO-F411RE 펌웨어 (FreeRTOS 기반)
- 2계층: Windows EC 소프트웨어 (GEM 상태 머신, SECS-II/HSMS)
- 3계층: macOS Host 프로그램 (HSMS 클라이언트, 상태 표시/명령 UI)
- 계층 간 내부 프로토콜 (STM32↔EC), SECS-II/HSMS (EC↔Host)

## 1.3 설계 범위 외
- 구체적인 클래스 멤버 변수, 함수 구현 코드 (소스 코드 참조)
- 빌드 시스템, 배포 스크립트
- 하드웨어 회로 설계

---

# 2. 시스템 아키텍처

## 2.1 계층 분해

```
┌─────────────────────────────────────────────────────────┐
│                    STM32 펌웨어 (C)                      │
│  ┌──────────┬──────────┬───────────┬────────────────┐   │
│  │ Sensor   │ Actuator │  Comm     │ FreeRTOS Tasks │   │
│  │ Driver   │ Driver   │  Layer    │ + Watchdog     │   │
│  └──────────┴──────────┴───────────┴────────────────┘   │
└─────────────────────────┬───────────────────────────────┘
                          │ UART / USB VCP
                          │ 내부 프로토콜 (Section 5)
┌─────────────────────────▼───────────────────────────────┐
│                  EC 소프트웨어 (C++)                     │
│  ┌────────────┬───────────┬──────────┬────────────────┐  │
│  │ DeviceComm │ StateMgr  │ AlarmMgr │ EventMgr       │  │
│  │ (UART)     │ (GEM FSM) │          │                │  │
│  ├────────────┴───────────┴──────────┴────────────────┤  │
│  │            SecsGemStack (HSMS + SECS-II)           │  │
│  └────────────────────────────────────────────────────┘  │
└─────────────────────────┬───────────────────────────────┘
                          │ HSMS (TCP/IP)
                          │ SECS-II 메시지
┌─────────────────────────▼───────────────────────────────┐
│                  Host 프로그램 (C++ / Python)            │
│  ┌────────────┬───────────────────┬────────────────┐    │
│  │ HsmsClient │ MessageProcessor  │  UI / CLI      │    │
│  └────────────┴───────────────────┴────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

## 2.2 설계 원칙
- **상태 소유권 분리**: STM32는 raw device state만 보유. Equipment State / Control State의 authoritative owner는 EC (SRS Section 3.1 참조)
- **계층 간 단방향 의존**: 상위 계층이 하위 계층에 명령을 내리고, 하위 계층은 이벤트/데이터를 상위로 보고
- **모듈 독립성**: 각 모듈은 인터페이스를 통해서만 통신하며 내부 구현을 은닉 (FR-018, NFR-016)
- **설정 분리**: 임계값, 타이머 주기, 포트 번호 등은 설정 파일/구조체로 관리 (NFR-017)

---

# 3. STM32 펌웨어 설계

## 3.1 모듈 구조

```
firmware/
├── Core/
│   ├── main.c                  # 초기화 및 FreeRTOS 스케줄러 시작
│   ├── freertos.c              # 태스크 생성 및 동기화 객체 초기화
│   └── config.h                # 설정값 중앙 관리 (주기, 임계값, 핀 정의)
├── Drivers/
│   ├── sht31.c / sht31.h       # SHT31 I2C 드라이버
│   ├── ina219.c / ina219.h     # INA219 I2C 드라이버
│   ├── actuator.c / actuator.h # 팬/부저/LED 제어
│   └── button.c / button.h     # 버튼 입력 감지
├── Comm/
│   ├── uart_comm.c / .h        # UART 송수신, 프레임 파싱, CRC
│   └── protocol.h              # 내부 프로토콜 메시지 타입/구조체 정의
└── Tasks/
    ├── task_sensor.c           # TaskSensor 구현
    ├── task_comm.c             # TaskComm 구현
    ├── task_heartbeat.c        # TaskHeartbeat 구현
    ├── task_button.c           # TaskButton 구현
    ├── task_actuator.c         # TaskActuator 구현
    └── task_watchdog.c         # TaskWatchdog 구현
```

## 3.2 FreeRTOS 태스크 설계

### 3.2.1 태스크 파라미터

| 태스크명 | 우선순위 | 스택 크기 | 주기 | 담당 FR |
|---------|---------|----------|------|---------|
| TaskWatchdog | 5 (최고) | 256 words | 100ms | FR-021~024 |
| TaskComm | 4 (높음) | 512 words | 이벤트 구동 | FR-009~017 |
| TaskButton | 4 (높음) | 128 words | 이벤트 구동 | FR-008, FR-010 |
| TaskSensor | 3 (보통) | 256 words | 1,000ms (설정 가능) | FR-001~004, FR-009 |
| TaskActuator | 3 (보통) | 256 words | 이벤트 구동 | FR-005~007, FR-011 |
| TaskHeartbeat | 2 (낮음) | 128 words | 5,000ms | FR-017 |

> FreeRTOS 우선순위 숫자: 높을수록 높은 우선순위 (configMAX_PRIORITIES = 6)

### 3.2.2 태스크 간 통신 객체

| 큐/뮤텍스 이름 | 타입 | 크기 | 생산자 | 소비자 | 용도 |
|--------------|------|------|--------|--------|------|
| xQueueSensorData | Queue | 4 items | TaskSensor | TaskComm | 센서 데이터 전달 |
| xQueueTxFrame | Queue | 8 items | TaskSensor, TaskButton, TaskHeartbeat | TaskComm | 송신 프레임 대기열 |
| xQueueRxCmd | Queue | 4 items | TaskComm | TaskActuator | 수신 명령 전달 |
| xQueueButtonEvent | Queue | 4 items | TaskButton | TaskComm | 버튼 이벤트 전달 |
| xMutexSensorBuf | Mutex | - | - | - | 센서 버퍼 공유 보호 |
| xMutexActuatorState | Mutex | - | - | - | 액추에이터 상태 공유 보호 |

### 3.2.3 Task Watchdog 체크인 메커니즘

```
각 태스크 루프 시작 시:
  xTaskWatchdogCheckin(taskId)  // 체크인 타임스탬프 갱신

TaskWatchdog 루프 (100ms 주기):
  for each monitored task:
    elapsed = now - lastCheckin[taskId]
    if elapsed > timeout[taskId]:        // 기본: 주기 × 3
      vTaskDelete(taskHandle[taskId])
      xTaskCreate(taskEntry[taskId], ...)  // 해당 태스크만 재시작
      report_restart_event(taskId)         // EC에 RESTART_REASON 전송

  feedHardwareWatchdog()  // IWDG 리셋 (모든 태스크 정상 시에만)
```

TaskWatchdog이 자신을 포함한 태스크 중 하나라도 hang 판정 시 IWDG 피드를 중단 → 하드웨어 Watchdog이 MCU 전체를 재시작.

## 3.3 센서 드라이버 설계

### 3.3.1 SHT31 드라이버

```
// 인터페이스
typedef struct {
    float temperature;   // °C
    float humidity;      // %
    bool  valid;
    uint8_t error_code;  // 0=정상, 1=I2C NACK, 2=CRC 불일치, 3=범위 초과
} SHT31_Data_t;

SHT31_Status_t SHT31_Read(SHT31_Data_t *out);
```

- I2C 주소: 0x44 (ADDR 핀 LOW)
- 측정 명령: 0x2400 (Single Shot, High Repeatability, Clock Stretch Disabled)
- 유효 범위 검사: 온도 -40~125°C, 습도 0~100% (FR-004)
- I2C 읽기 실패 시 error_code 설정 후 valid = false 반환

### 3.3.2 INA219 드라이버

```
typedef struct {
    float current_mA;
    float voltage_V;
    bool  valid;
    uint8_t error_code;
} INA219_Data_t;

INA219_Status_t INA219_Read(INA219_Data_t *out);
```

- I2C 주소: 0x40
- 설정: Bus Voltage Range 32V, Gain /8, 12-bit ADC
- 유효 범위: 전류 0~3,200mA, 전압 0~26V (FR-004)

## 3.4 TaskSensor 동작 흐름

```
loop (주기: config.sensor_period_ms):
  xTaskWatchdogCheckin(TASK_SENSOR)

  sht31_ok = SHT31_Read(&sht31_data)
  ina219_ok = INA219_Read(&ina219_data)

  build SensorDataMsg:
    sht31_valid = sht31_data.valid
    ina219_valid = ina219_data.valid
    // 유효한 값만 포함, 무효 시 error_code 포함

  xQueueSend(xQueueTxFrame, &frame, timeout)

  // Graceful Degradation (FR-025):
  // 하나 장애여도 나머지 데이터는 계속 전송
  // 두 센서 모두 장애 → ERROR_CODE_DUAL_SENSOR_FAIL 포함
```

## 3.5 TaskComm 동작 흐름

```
loop (이벤트 구동):
  xTaskWatchdogCheckin(TASK_COMM)

  // 수신 처리
  if UART_RxAvailable():
    read bytes into rxBuffer
    if FrameParser_TryParse(rxBuffer, &frame):
      if CRC_Verify(frame): 
        send ACK(frame.seqNum)
        if IsDuplicate(frame.seqNum): discard
        else:
          update lastReceivedSeq
          xQueueSend(xQueueRxCmd, &frame, timeout)
      else:
        send NACK(frame.seqNum)

  // 송신 처리
  if xQueueReceive(xQueueTxFrame, &frame, 0) == pdTRUE:
    frame.seqNum = nextTxSeq++
    frame.crc = CRC16_Calculate(frame)
    UART_Transmit(frame)
    wait ACK with timeout(1000ms), retry up to 3 times (FR-014, FR-015)
```

## 3.6 Graceful Degradation 설계 (FR-025~027)

3단계 장애 격리 구조:

```
Level 1 — Functional Degradation (FR-025, FR-026):
  - 센서 1개 장애: 나머지 센서 수집/통신 계속
  - EC 통신 중단: 마지막 액추에이터 상태 유지 또는 Safety Default
    Safety Default: 팬 OFF, 부저 ON, LED ALARM

Level 2 — Task Watchdog (FR-022, FR-027):
  - 특정 태스크 hang → 해당 태스크만 재시작
  - TaskComm 재시작 중에도 TaskSensor/TaskActuator 동작 유지

Level 3 — Hardware IWDG (FR-021):
  - TaskWatchdog이 피드 중단 시 MCU 전체 재시작
  - 재시작 후 EC 재연결 및 상태 동기화 (FR-023)
```

---

# 4. EC 소프트웨어 설계 (Windows, C++)

## 4.1 모듈 구조

```
ec/
├── main.cpp                    # 진입점, 모듈 초기화 및 이벤트 루프
├── config/
│   └── ec_config.h             # 설정 구조체 (임계값, 포트, 타이머)
├── device/
│   ├── DeviceComm.h/.cpp       # STM32 UART 통신, 프레임 파싱, ACK/NACK
│   └── InternalProtocol.h      # 내부 프로토콜 메시지 타입/구조체
├── gem/
│   ├── StateMachine.h/.cpp     # Equipment State FSM (FR-028~034)
│   ├── ControlState.h/.cpp     # Control State 관리 (FR-035~039)
│   ├── AlarmManager.h/.cpp     # 알람 발생/해제/목록 (FR-040~046)
│   ├── EventManager.h/.cpp     # Collection Event 생성/보고 (FR-047~049)
│   ├── InterlockChecker.h/.cpp # 인터록 조건 판단 (FR-056~058)
│   └── DataStore.h/.cpp        # 상태/센서 데이터 보존 (FR-059~060)
├── secs/
│   ├── HsmsServer.h/.cpp       # HSMS Passive 서버, 세션 관리 (FR-050~051)
│   ├── SecsMessageEncoder.h/.cpp  # SECS-II 메시지 직렬화
│   ├── SecsMessageDecoder.h/.cpp  # SECS-II 메시지 역직렬화
│   └── SecsHandlers.h/.cpp     # S/F 핸들러 디스패치 (FR-052~055)
└── util/
    ├── Logger.h/.cpp           # 타임스탬프 로그 (NFR-015)
    └── CircularBuffer.h        # 이벤트 순환 버퍼 100개 (FR-049)
```

## 4.2 Equipment State Machine 설계 (FR-028~034)

### 4.2.1 상태 열거형 및 전이 테이블

```cpp
enum class EquipmentState { INIT, IDLE, RUN, STOP, ALARM, ERROR };

// 전이 테이블 (SRS Section 11.1)
struct Transition {
    EquipmentState from;
    EquipmentState to;
    TransitionTrigger trigger;
    std::function<bool()> guard;   // 인터록/조건 검사
    std::function<void()> action;  // 전이 시 동작 (명령 전송, 이벤트 생성)
};
```

### 4.2.2 상태 전이 시 동작

| 전이 | guard | action |
|------|-------|--------|
| INIT → IDLE | STM32 연결 + 센서 초기화 완료 | CEID-1(StateChanged), CEID-11(DeviceConnected) 생성 |
| IDLE → RUN | InterlockChecker::CheckStart() == OK | STM32 FAN_ON 명령, CEID-1, CEID-5 생성 |
| RUN → STOP | STOP 명령 수신 | STM32 FAN_OFF 명령, CEID-1 생성 |
| STOP → IDLE | 정지 처리 완료 (FAN_OFF ACK 수신) | CEID-1 생성 |
| ANY → ALARM | AlarmManager::HasActiveAlarm() | STM32 Safety Default 명령, S5F1 전송, CEID-3 생성 |
| ALARM → IDLE | 이상 조건 해소 + ACK_ALARM 수신 | S5F1(CLEAR), CEID-4 생성 |
| ANY → ERROR | FR-033 조건 충족 | STM32 Safety Default 명령, S5F1 전송, EC 재시작 요구 |

### 4.2.3 FSM 처리 흐름

```
StateMachine::ProcessEvent(Event e):
  for each transition in table where from == currentState:
    if transition.trigger == e.type && transition.guard():
      currentState = transition.to
      transition.action()
      EventManager::Emit(CEID_STATE_CHANGED)
      Logger::Log(...)
      return
  Logger::Warn("Unhandled event in state")
```

## 4.3 AlarmManager 설계 (FR-040~046)

### 4.3.1 알람 조건 판단 테이블

| ALID | 발생 조건 | 관련 데이터 소스 | 히스테리시스 |
|------|---------|--------------|------------|
| 1 (HIGH_TEMP) | temperature > config.alarm_temp_threshold | SensorData.temperature | 해제: ≤ threshold − 2.0°C |
| 2 (OVER_CURRENT) | current_mA > config.alarm_current_threshold | SensorData.current_mA | 해제: ≤ threshold |
| 3 (SENSOR_ERROR) | sensorErrorCount ≥ 3 연속 | DeviceComm 콜백 | sensorErrorCount 리셋 |
| 4 (UART_COMM_ERROR) | lastHeartbeatAge > 10,000ms | DeviceComm 타이머 | UART 재연결 완료 |

### 4.3.2 알람 상태 관리

```cpp
struct AlarmRecord {
    uint16_t alid;
    bool     active;
    uint64_t setTimestamp;
    uint64_t clearTimestamp;
};

// 내부 상태
std::array<AlarmRecord, 4> alarmTable;
int activeAlarmCount;

void AlarmManager::CheckAndSet(AlarmId id) {
    if (!alarmTable[id].active && conditionMet(id)) {
        alarmTable[id].active = true;
        alarmTable[id].setTimestamp = now();
        activeAlarmCount++;
        SecsHandlers::SendS5F1(id, ALST_SET);
        EventManager::Emit(CEID_ALARM_SET);
        StateMachine::ProcessEvent(EVENT_ALARM);
    }
}

void AlarmManager::TryClear(AlarmId id) {
    if (alarmTable[id].active && !conditionMet(id)) {
        alarmTable[id].active = false;
        activeAlarmCount--;
        SecsHandlers::SendS5F1(id, ALST_CLEAR);
        EventManager::Emit(CEID_ALARM_CLEARED);
    }
}
```

## 4.4 HSMS 서버 설계 (FR-050~055)

### 4.4.1 HSMS 세션 상태 머신

```
NOT_CONNECTED
    │ accept()
    ▼
CONNECTED (T7 타이머 시작 — 10초 내 Select 미수신 시 연결 해제)
    │ SelectReq 수신
    ▼
SELECTED
    │ Linktest (T6 주기)
    │ SECS-II 메시지 교환
    │ DeSelectReq 또는 SeparateReq 수신
    ▼
NOT_CONNECTED
```

### 4.4.2 허용 IP 접근 제어 (NFR-012)

```cpp
bool HsmsServer::OnAccept(const std::string& remoteIp) {
    if (config.allowedIps.find(remoteIp) == config.allowedIps.end()) {
        Logger::Warn("Connection rejected from " + remoteIp);
        return false;
    }
    return true;
}
```

### 4.4.3 S/F 핸들러 디스패치 테이블

| S/F | 핸들러 함수 | 담당 FR |
|-----|-----------|---------|
| S1F1 | HandleAreYouThere() | FR-052 |
| S1F13 | HandleEstablishComm() | FR-052 |
| S1F3 | HandleStatusVarRequest() | FR-053 |
| S2F41 | HandleRemoteCommand() | FR-054 |
| S5F5 | HandleAlarmListRequest() | FR-046 |
| 미정의 | HandleUnknown() → S9F* | FR-055 |

미정의 S/F 처리:
- S9F1: Unknown Device ID
- S9F3: Unknown Stream Type
- S9F5: Unknown Function Type
- S9F7: Illegal Data

### 4.4.4 Remote Command 처리 흐름 (FR-054)

```
HandleRemoteCommand(S2F41):
  // NFR-013: 세션 미수립 상태에서 수신 시 무시
  if session.state != SELECTED:
    SendS9F1(); return

  rcmd = decode S2F41
  switch rcmd:
    case START:
      if controlState != REMOTE → S2F42(CMDA=DENIED_NOT_REMOTE)
      if !InterlockChecker::CheckStart() → S2F42(CMDA=DENIED_INTERLOCK)
      else → StateMachine::ProcessEvent(CMD_START), S2F42(CMDA=ACK)

    case STOP:
      StateMachine::ProcessEvent(CMD_STOP)
      S2F42(CMDA=ACK)

    case RESET:
      if equipmentState == ERROR → S2F42(CMDA=DENIED_ERROR_STATE)
      else → StateMachine::ProcessEvent(CMD_RESET), S2F42(CMDA=ACK)

    case ACK_ALARM:
      AlarmManager::TryClear(alid)
      S2F42(CMDA=ACK)

    case SET_REMOTE / SET_LOCAL:
      ControlState::TryTransition(rcmd)
      S2F42(...)
```

## 4.5 DeviceComm 설계 (STM32↔EC UART)

```
DeviceComm::ReadLoop():
  while running:
    bytes = SerialPort::Read(readBuf, maxLen, timeout=100ms)
    rxAssembler.Feed(bytes)
    while rxAssembler.HasFrame():
      frame = rxAssembler.Pop()
      if !CRC16_Verify(frame): continue  // 묵시적 NACK
      if IsDuplicate(frame.seqNum): SendACK(); continue
      SendACK(frame.seqNum)
      DispatchFrame(frame)

  // 타이머: lastHeartbeatAge 갱신
  if (now - lastHeartbeatTime) > HEARTBEAT_TIMEOUT_MS:
    AlarmManager::CheckAndSet(ALID_UART_COMM_ERROR)
```

---

# 5. 내부 프로토콜 설계 (STM32↔EC)

## 5.1 프레임 구조

```
┌─────────┬──────┬──────┬─────────┬──────────────────┬─────────┐
│ SOF(1B) │ TYPE │ SEQ  │ LEN(2B) │ PAYLOAD (N bytes)│ CRC16   │
│  0xAA   │ (1B) │ (1B) │         │                  │  (2B)   │
└─────────┴──────┴──────┴─────────┴──────────────────┴─────────┘
Total header: 5 bytes, CRC: 2 bytes, Max payload: 255 bytes
CRC16 계산 범위: TYPE ~ PAYLOAD 끝
```

## 5.2 메시지 타입 정의

| TYPE | 이름 | 방향 | 설명 |
|------|------|------|------|
| 0x01 | SENSOR_DATA | STM32→EC | 센서 측정값 보고 |
| 0x02 | BUTTON_EVENT | STM32→EC | 버튼 입력 이벤트 |
| 0x03 | RESTART_REASON | STM32→EC | Watchdog 재시작 원인 |
| 0x04 | ACK | 양방향 | 수신 확인 |
| 0x05 | NACK | 양방향 | 수신 오류 |
| 0x10 | CMD_FAN | EC→STM32 | 팬 제어 (ON/OFF) |
| 0x11 | CMD_BUZZER | EC→STM32 | 부저 제어 (ON/OFF) |
| 0x12 | CMD_LED | EC→STM32 | LED 상태 설정 |
| 0x13 | CMD_STATE_SYNC | EC→STM32 | 재연결 후 Equipment State 동기화 |
| 0x20 | HEARTBEAT_REQ | EC→STM32 | Heartbeat 요청 |
| 0x21 | HEARTBEAT_ACK | STM32→EC | Heartbeat 응답 |

## 5.3 SENSOR_DATA 페이로드 구조

```c
typedef struct __attribute__((packed)) {
    uint8_t  flags;           // bit0=sht31_valid, bit1=ina219_valid
    uint8_t  sht31_error;     // SHT31 오류 코드 (flags.bit0=0일 때)
    uint8_t  ina219_error;    // INA219 오류 코드 (flags.bit1=0일 때)
    float    temperature;     // °C (IEEE 754, 4 bytes)
    float    humidity;        // % (4 bytes)
    float    current_mA;      // mA (4 bytes)
    float    voltage_V;       // V (4 bytes)
    uint32_t timestamp_ms;    // STM32 HAL_GetTick() 값
} SensorDataPayload_t;        // 총 22 bytes
```

## 5.4 RESTART_REASON 페이로드 구조

```c
typedef struct __attribute__((packed)) {
    uint8_t  reason_code;     // 0x01=IWDG, 0x02=TaskWatchdog, 0x03=PowerOn
    uint8_t  failed_task_id;  // TaskWatchdog 재시작 시 해당 태스크 ID
    uint8_t  restart_count;   // EC 연결 후 누적 재시작 횟수
} RestartReasonPayload_t;     // 3 bytes
```

## 5.5 ACK/NACK 구조

```c
typedef struct __attribute__((packed)) {
    uint8_t ack_seq_num;  // 응답 대상 메시지의 시퀀스 번호
} AckPayload_t;           // 1 byte
```

## 5.6 재전송 정책 (FR-014~015)

```
송신 측:
  전송 → 1초 대기 (T1 타이머)
  ACK 미수신 → 재전송 (최대 3회)
  3회 초과 → CommError 이벤트 발생, 상위 모듈에 보고

수신 측:
  CRC 불일치 → NACK 즉시 반환
  중복 seqNum → ACK 반환 후 페이로드 무시
```

---

# 6. Host 프로그램 설계 (macOS, C++ / Python)

## 6.1 모듈 구조

```
host/
├── main.py (또는 main.cpp)
├── hsms/
│   ├── HsmsClient.py       # HSMS Active 클라이언트, 세션 관리
│   └── SecsCodec.py        # SECS-II 인코딩/디코딩
├── handlers/
│   └── MessageHandler.py   # 수신 S/F 처리 (S5F1, S6F11 등)
├── ui/
│   └── Console.py          # CLI 상태 표시 및 명령 입력
└── config.py               # EC IP, 포트, 설정값
```

## 6.2 HSMS 클라이언트 연결 흐름

```
1. TCP connect(ec_ip, ec_port)
2. Send SelectReq
3. Receive SelectRsp → 세션 수립
4. Send S1F13 Establish Communication
5. Receive S1F14 → Online 확인
6. 메시지 수신 루프 시작
   - S5F1 수신 → AlarmDisplay 갱신
   - S6F11 수신 → EventLog 갱신, SensorDisplay 갱신
7. Send Linktest 주기적으로 (T6)
```

## 6.3 표시 화면 구성 (FR-061~065)

```
=== EQUIP-CONTROL HOST ===
[State]  Equipment: RUN   | Control: REMOTE
[Sensor] Temp: 28.3°C  Humidity: 55.2%  Current: 412mA  Voltage: 12.1V
[Alarm]  Active: 0
[Events] (최근 10건)
  2026-04-04 10:01:23 | CEID-8 SensorDataReport | T=28.3 H=55.2 I=412 V=12.1
  2026-04-04 10:01:22 | CEID-5 CommandReceived  | CMD=START

> Command: [START | STOP | RESET | ACK_ALARM <alid> | SET_REMOTE | SET_LOCAL]
```

## 6.4 명령 전송 흐름 (FR-065)

```
사용자 입력 → CLI Parser → S2F41 인코딩 → HsmsClient.Send()
  → 수신 S2F42 확인 → 결과 출력
  → S6F11(CEID-5) 수신 → 이벤트 로그 갱신
```

---

# 7. 주요 시퀀스 설계

## 7.1 정상 시동 시퀀스

```
STM32          EC                     Host
  │  Power ON   │                       │
  │ INIT        │                       │
  │──RESTART──▶ │                       │
  │   (TYPE=0x03, reason=PowerOn)       │
  │             │ EC 시작               │
  │◀─STATE_SYNC─│ (IDLE 동기화)         │
  │             │          ◀──CONNECT── │
  │             │          ◀──SelectReq─│
  │             │──SelectRsp──▶         │
  │             │          ◀──S1F13──── │
  │             │──S1F14──▶             │
  │             │ INIT→IDLE             │
  │             │──S6F11(CEID-1)──▶     │
  │             │──S6F11(CEID-11)──▶    │
  │──SENSOR──▶  │                       │
  │   (1초 주기) │──S6F11(CEID-8)──▶   │
```

## 7.2 Remote Command START 시퀀스

```
Host             EC                    STM32
  │──S2F41(START)──▶│                    │
  │                 │ InterlockCheck()   │
  │                 │ StateMachine: IDLE→RUN
  │                 │──CMD_FAN(ON)──▶   │
  │                 │          ◀──ACK── │
  │◀──S2F42(ACK)────│                   │
  │◀──S6F11(CEID-5)─│                   │
  │◀──S6F11(CEID-1)─│                   │
```

## 7.3 알람 발생 시퀀스

```
STM32               EC                  Host
  │──SENSOR(T=95°C)──▶│                  │
  │                   │ AlarmMgr: ALID-1 SET
  │                   │ StateMachine: ANY→ALARM
  │◀─CMD_BUZZER(ON)───│                  │
  │◀─CMD_LED(ALARM)───│                  │
  │                   │──S5F1(ALID-1,SET)──▶│
  │                   │──S6F11(CEID-3)──▶│
  │                   │──S6F11(CEID-1)──▶│
  │                   │       ◀──S2F41(ACK_ALARM,ALID=1)──│
  │                   │ AlarmMgr: ALID-1 CLEAR
  │◀─CMD_BUZZER(OFF)──│                  │
  │                   │──S5F1(ALID-1,CLEAR)──▶│
```

## 7.4 Watchdog 재시작 → 복구 시퀀스

```
STM32 (TaskWatchdog)    STM32 (MCU)    EC                Host
  │ TaskComm hang        │              │                  │
  │ IWDG feed 중단       │              │                  │
  │                      │ MCU 재시작   │                  │
  │                      │──RESTART──▶  │                  │
  │                      │  (reason=0x01, count=N)         │
  │                      │◀─STATE_SYNC──│                  │
  │                      │ 정상 수집 재개│                  │
  │                      │              │──S6F11(CEID-13)──▶│
  │                      │              │  (SystemRecovery) │
```

---

# 8. 설정 관리 설계 (NFR-017)

## 8.1 STM32 설정 구조체 (config.h)

```c
typedef struct {
    uint32_t sensor_period_ms;      // 기본: 1000
    uint32_t heartbeat_period_ms;   // 기본: 5000
    uint32_t uart_timeout_ms;       // 기본: 1000
    uint8_t  max_retransmit;        // 기본: 3
    uint32_t watchdog_timeout_ms;   // 기본: 태스크 주기 × 3
    // 센서 유효 범위
    float    temp_min, temp_max;    // -40.0, 125.0
    float    humi_min, humi_max;    // 0.0, 100.0
    float    curr_min, curr_max;    // 0.0, 3200.0
    float    volt_min, volt_max;    // 0.0, 26.0
} FirmwareConfig_t;

extern const FirmwareConfig_t g_config;  // config.c에서 초기화
```

## 8.2 EC 설정 파일 구조 (ec_config.json)

```json
{
  "device": {
    "port": "COM3",
    "baud_rate": 115200
  },
  "hsms": {
    "listen_port": 5000,
    "allowed_ips": ["127.0.0.1", "192.168.0.100"],
    "t3_reply_timeout_ms": 10000,
    "t6_linktest_period_ms": 10000,
    "t7_not_selected_timeout_ms": 10000
  },
  "alarm_thresholds": {
    "temperature_c": 80.0,
    "temperature_hysteresis_c": 2.0,
    "current_ma": 2000.0,
    "sensor_error_count": 3,
    "uart_timeout_ms": 10000
  },
  "event_log_capacity": 100
}
```

---

# 9. 오류 처리 설계

## 9.1 오류 종류별 처리 방침

| 오류 | 감지 위치 | 처리 방침 | 상위 보고 |
|------|---------|---------|---------|
| 센서 I2C 오류 | STM32 SHT31/INA219 드라이버 | 오류 코드 포함 SENSOR_DATA 전송, 계속 동작 | EC → ALID-3 |
| UART CRC 오류 | STM32/EC DeviceComm | NACK 반환, 재전송 요청 | 3회 초과 시 ALID-4 |
| UART 연결 끊김 | EC DeviceComm (Heartbeat 감시) | Safety Default 명령 시도, 재연결 대기 | ALID-4 |
| HSMS 세션 끊김 | EC HsmsServer | 재연결 대기, CEID-10 생성 | Host 재연결 시 복구 |
| 미허용 IP 연결 | EC HsmsServer | 연결 거부, 로그 기록 | - |
| ERROR 상태 | EC StateMachine | EC 재시작 필요, 자동 복구 불가 | Host 표시 |
| MCU hang | STM32 IWDG | MCU 재시작, RESTART_REASON 전송 | EC → CEID-13 |

## 9.2 ERROR 상태 판정 로직 (FR-033)

```cpp
void StateMachine::CheckErrorCondition() {
    bool watchdogRestartTooMany =
        (restartCount >= 3) && (timeSinceFirstRestart < 60000ms);
    bool dualSensorFail =
        !sht31Data.valid && !ina219Data.valid;
    bool memoryAllocFail =
        (lastAllocResult == ALLOC_FAILED);

    if (watchdogRestartTooMany || dualSensorFail || memoryAllocFail) {
        ProcessEvent(EVENT_FATAL_ERROR);  // ALARM 또는 ANY → ERROR
    }
}
```

---

# 10. 요구사항 추적 (SDD ↔ SRS)

| SDD 섹션 | 설계 내용 | 관련 FR | 관련 NFR |
|---------|---------|---------|---------|
| 3.2 FreeRTOS 태스크 | 태스크 파라미터, 큐/뮤텍스 | FR-018~020 | NFR-009, NFR-011 |
| 3.2.3 Task Watchdog | 체크인 메커니즘, 재시작 로직 | FR-021~024 | NFR-010 |
| 3.6 Graceful Degradation | 3단계 장애 격리 | FR-025~027 | NFR-011 |
| 4.2 Equipment State Machine | FSM 전이 테이블, 동작 | FR-028~034 | NFR-001, NFR-004 |
| 4.3 AlarmManager | 조건 판단, 히스테리시스 | FR-040~046 | NFR-004 |
| 4.4 HSMS 서버 | 세션 FSM, S/F 디스패치 | FR-050~055 | NFR-001, NFR-005, NFR-012, NFR-013 |
| 4.5 DeviceComm | 재전송, 타임아웃, Heartbeat | FR-012~017 | NFR-006 |
| 5. 내부 프로토콜 | 프레임 구조, 메시지 타입, CRC | FR-012~017 | NFR-004 |
| 6. Host 프로그램 | HSMS 클라이언트, CLI | FR-061~065 | NFR-014 |
| 8. 설정 관리 | config.h, ec_config.json | NFR-017 | - |
| 9. 오류 처리 | ERROR 판정, 장애 격리 | FR-033 | NFR-007, NFR-011 |
