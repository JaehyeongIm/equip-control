# 소프트웨어 설계 문서 (SDD)

## 프로젝트명
가상 공정 챔버 기반 장비 이상 진단·인터락·PM 시뮬레이터

## 문서 버전
v2.0

## 준거 표준
IEEE 1016-2009 (Systems and Software Engineering — Software Design Descriptions)

## 작성일
2026-04-11

## 작성자
임재형

## 관련 문서
- Software Requirements Specification (SRS) v2.0
- Interface Specification (IFS): STM32↔EC 및 EC↔Host 메시지 포맷 상세

---

# 1. 개요

## 1.1 목적
본 문서는 SRS v2.0에 정의된 요구사항을 충족하는 소프트웨어 모듈 구조, 인터페이스, 상태 머신, 데이터 흐름을 설계한다.

## 1.2 범위
- 1계층: STM32 NUCLEO-F411RE 펌웨어 (FreeRTOS 기반, 챔버 하드웨어 제어)
- 2계층: macOS EC 소프트웨어 (GEM 상태 머신, 이상 진단, 인터락, PM 추적, SECS-II/HSMS)
- 3계층: macOS Host 프로그램 (HSMS 클라이언트, 챔버 상태/PM 표시/명령 curses GUI)
- 계층 간 내부 프로토콜 (STM32↔EC), SECS-II/HSMS (EC↔Host)

## 1.3 설계 범위 외
- 구체적인 클래스 멤버 변수, 함수 구현 코드 (소스 코드 참조)
- 빌드 시스템, 배포 스크립트
- 하드웨어 회로 설계

---

# 2. 시스템 아키텍처

## 2.1 계층 분해

```
┌──────────────────────────────────────────────────────────┐
│                    STM32 펌웨어 (C)                       │
│  ┌──────────┬──────────┬───────────┬────────────────┐    │
│  │ Sensor   │ Actuator │  Comm     │ FreeRTOS Tasks │    │
│  │ Driver   │ Driver   │  Layer    │ + Watchdog     │    │
│  │ SHT31    │ Relay    │           │                │    │
│  │ INA219   │ Fan/Buzz │           │                │    │
│  └──────────┴──────────┴───────────┴────────────────┘    │
│  ┌──────────────────────────────────────────────────┐     │
│  │  Door Sensor (Limit Switch)  │  LED State        │     │
│  └──────────────────────────────────────────────────┘     │
└──────────────────────────┬───────────────────────────────┘
                           │ UART / USB VCP
                           │ 내부 프로토콜 (Section 5)
┌──────────────────────────▼───────────────────────────────┐
│                  EC 소프트웨어 (C++)                      │
│  ┌────────────┬───────────┬──────────┬────────────────┐   │
│  │ DeviceComm │ StateMgr  │ AlarmMgr │ FaultDiagnosis │   │
│  │ (UART)     │ (GEM FSM) │          │                │   │
│  ├────────────┴───────────┴──────────┴────────────────┤   │
│  │  InterlockChecker  │  PMTracker                    │   │
│  ├───────────────────────────────────────────────────┤    │
│  │            SecsGemStack (HSMS + SECS-II)          │    │
│  └───────────────────────────────────────────────────┘    │
└──────────────────────────┬───────────────────────────────┘
                           │ HSMS (TCP/IP)
                           │ SECS-II 메시지
┌──────────────────────────▼───────────────────────────────┐
│                  Host 프로그램 (Python)                   │
│  ┌────────────┬───────────────────┬────────────────┐     │
│  │ HsmsClient │ MessageProcessor  │  UI / CLI      │     │
│  │            │                   │  (curses)      │     │
│  └────────────┴───────────────────┴────────────────┘     │
└──────────────────────────────────────────────────────────┘
```

## 2.2 설계 원칙
- **상태 소유권 분리**: STM32는 raw device state만 보유. Equipment State, 인터락 판단, PM 추적의 authoritative owner는 EC
- **계층 간 단방향 의존**: 상위 계층이 하위 계층에 명령을 내리고, 하위 계층은 이벤트/데이터를 상위로 보고
- **이중 인터락**: 하드웨어(SW-102T) + 소프트웨어(EC) 이중 보호
- **모듈 독립성**: 각 모듈은 인터페이스를 통해서만 통신하며 내부 구현 은닉
- **설정 분리**: 임계값, 타이머 주기, 포트 번호 등은 config.h로 관리

---

# 3. STM32 펌웨어 설계

## 3.1 모듈 구조

```
firmware/
├── Core/
│   ├── Src/
│   │   ├── main.c              # 초기화 및 FreeRTOS 스케줄러 시작
│   │   ├── freertos.c          # 태스크 생성 및 동기화 객체 초기화
│   │   ├── sht31.c             # SHT31 온습도 드라이버 (HAL I2C)
│   │   ├── uart_comm.c         # UART 송수신, 프레임 파싱, CRC
│   │   └── watchdog_mgr.c      # Task Watchdog 관리
│   └── Inc/
│       ├── config.h            # 설정값 중앙 관리 (주기, 임계값, 핀 정의)
│       ├── protocol.h          # 내부 프로토콜 메시지 타입/구조체 정의
│       ├── sht31.h
│       └── uart_comm.h
├── Drivers/
│   └── STM32F4xx_HAL_Driver/   # CubeMX 생성 HAL 드라이버
```

## 3.2 FreeRTOS 태스크 설계

### 3.2.1 태스크 파라미터

| 태스크명 | 우선순위 | 스택 크기 | 주기 | 담당 FR |
|---------|---------|----------|------|---------|
| TaskWatchdog | 5 (최고) | 256 words | 100ms | FR-044~046 |
| TaskComm | 4 (높음) | 512 words | 이벤트 구동 | FR-025~031 |
| TaskDoor | 4 (높음) | 128 words | 이벤트 구동 | FR-005~006 |
| TaskButton | 4 (높음) | 128 words | 이벤트 구동 | FR-011 |
| TaskSensor | 3 (보통) | 256 words | 1,000ms | FR-001~004, FR-025 |
| TaskActuator | 3 (보통) | 256 words | 이벤트 구동 | FR-007~010 |
| TaskHeartbeat | 2 (낮음) | 128 words | 5,000ms | FR-032 |

### 3.2.2 태스크 간 통신 객체

| 큐/뮤텍스 이름 | 타입 | 크기 | 생산자 | 소비자 |
|--------------|------|------|--------|--------|
| xQueueTxFrame | Queue | 8 items | TaskSensor, TaskDoor, TaskButton, TaskHeartbeat | TaskComm |
| xQueueRxCmd | Queue | 4 items | TaskComm | TaskActuator |
| xMutexSensorBuf | Mutex | — | TaskSensor | TaskComm |

### 3.2.3 Task Watchdog 체크인 메커니즘

```
각 태스크 루프 시작 시:
  watchdog_checkin(taskId)   // 체크인 타임스탬프 갱신

TaskWatchdog 루프 (100ms 주기):
  for each monitored task:
    elapsed = now - lastCheckin[taskId]
    if elapsed > timeout[taskId]:        // 기본: 주기 × 3
      vTaskDelete(taskHandle[taskId])
      xTaskCreate(taskEntry[taskId], ...)  // 해당 태스크만 재시작
      send RESTART_REASON frame to EC

  feedHardwareWatchdog()  // IWDG 리셋
```

## 3.3 내부 프로토콜 메시지 타입

### STM32→EC 메시지

| TYPE | 이름 | 페이로드 |
|------|------|---------|
| 0x01 | MSG_SENSOR_DATA | SensorDataPayload (온도/습도/전류/전압/오류코드) |
| 0x02 | MSG_BUTTON_EVENT | ButtonEventPayload (button_id, event_type) |
| 0x03 | MSG_RESTART_REASON | RestartReasonPayload (reason_code, task_id, count) |
| 0x04 | MSG_ACK | AckPayload (ack_seq) |
| 0x05 | MSG_NACK | — |
| 0x06 | MSG_DOOR_EVENT | DoorEventPayload (door_state: 0=닫힘, 1=열림) |

### EC→STM32 메시지

| TYPE | 이름 | 페이로드 |
|------|------|---------|
| 0x10 | MSG_CMD_RELAY | CmdRelayPayload (on: 1=ON, 0=OFF) |
| 0x11 | MSG_CMD_FAN | CmdFanPayload (on: 1=ON, 0=OFF) |
| 0x12 | MSG_CMD_BUZZER | CmdBuzzerPayload (on: 1=ON, 0=OFF) |
| 0x13 | MSG_CMD_LED | CmdLedPayload (state: IDLE/RUNNING/ALARM/INTERLOCK) |
| 0x20 | MSG_HEARTBEAT_REQ | — |
| 0x21 | MSG_HEARTBEAT_ACK | — |

## 3.4 SensorDataPayload 구조체

```c
typedef struct __attribute__((packed)) {
    uint8_t  flags;         /* bit0=sht31_valid, bit1=ina219_valid */
    uint8_t  sht31_error;   /* SENSOR_ERR_* */
    uint8_t  ina219_error;  /* SENSOR_ERR_* */
    float    temperature;   /* °C  (챔버 내부) */
    float    humidity;      /* %   (챔버 내부) */
    float    current_mA;    /* mA  (히터 전류) */
    float    voltage_V;     /* V   (히터 전압) */
    uint32_t timestamp_ms;  /* HAL_GetTick() */
} SensorDataPayload_t;
```

## 3.5 DoorEventPayload 구조체

```c
typedef struct __attribute__((packed)) {
    uint8_t door_state;     /* 0=닫힘, 1=열림 */
} DoorEventPayload_t;
```

## 3.6 TaskSensor 동작 흐름

```
loop (주기: config.sensor_period_ms = 1000ms):
  watchdog_checkin(TASK_ID_SENSOR)

  SHT31_Data sht;
  if SHT31_Read(&sht) == SHT31_OK:
    data.flags |= 0x01
    data.temperature = sht.temperature
    data.humidity    = sht.humidity
    data.sht31_error = SENSOR_ERR_NONE
  else:
    data.sht31_error = SENSOR_ERR_I2C_NACK

  INA219_Data ina;
  if INA219_Read(&ina) == INA219_OK:
    data.flags |= 0x02
    data.current_mA  = ina.current_mA
    data.voltage_V   = ina.voltage_V
    data.ina219_error = SENSOR_ERR_NONE
  else:
    data.ina219_error = SENSOR_ERR_I2C_NACK

  data.timestamp_ms = osKernelGetTickCount()

  xMutexAcquire → g_sensorBuf = data → xMutexRelease
  xQueueTxFrame ← MSG_SENSOR_DATA
  osDelay(CFG_SENSOR_PERIOD_MS)
```

## 3.7 TaskDoor 동작 흐름

```
loop (10ms 폴링):
  watchdog_checkin(TASK_ID_DOOR)

  cur_state = HAL_GPIO_ReadPin(DOOR_PORT, DOOR_PIN)
  if cur_state != last_state:
    last_state = cur_state
    DoorEventPayload payload = {cur_state}
    xQueueTxFrame ← MSG_DOOR_EVENT
  osDelay(10)
```

## 3.8 TaskActuator 동작 흐름

```
loop (이벤트 구동):
  watchdog_checkin(TASK_ID_ACTUATOR)
  xQueueRxCmd → cmd

  switch cmd.type:
    case MSG_CMD_RELAY:
      HAL_GPIO_Write(RELAY_PORT, RELAY_PIN, cmd.payload.on)
    case MSG_CMD_FAN:
      HAL_GPIO_Write(FAN_PORT, FAN_PIN, cmd.payload.on)
    case MSG_CMD_BUZZER:
      HAL_GPIO_Write(BUZZER_PORT, BUZZER_PIN, cmd.payload.on)
    case MSG_CMD_LED:
      update_led(cmd.payload.state)

  send ACK(cmd.seq)
```

## 3.9 Graceful Degradation

```
Level 1 — Functional Degradation:
  - 센서 1개 장애: 나머지 센서 수집/전송 계속
  - EC 통신 중단: 마지막 릴레이 상태 유지, 부저 ON, LED ALARM

Level 2 — Task Watchdog:
  - 특정 태스크 hang → 해당 태스크만 재시작
  - TaskComm 재시작 중에도 TaskSensor/TaskActuator 동작 유지

Level 3 — Hardware IWDG:
  - TaskWatchdog이 IWDG 피드 중단 → MCU 전체 재시작
  - 재시작 후 EC 재연결 및 상태 동기화
```

---

# 4. EC 소프트웨어 설계 (macOS, C++)

## 4.1 모듈 구조

```
equip-control-ec/
├── src/
│   ├── main.cpp                # 진입점, 모듈 초기화 및 메인 루프
│   ├── DeviceComm.h/.cpp       # STM32 UART 통신, 프레임 파싱, ACK
│   ├── StateMachine.h/.cpp     # Equipment State FSM
│   ├── AlarmManager.h/.cpp     # 알람 발생/해제 관리
│   ├── FaultDiagnosis.h/.cpp   # 이상 진단 로직 (온도/전류/센서)
│   ├── InterlockChecker.h/.cpp # 인터락 조건 판단
│   ├── PMTracker.h/.cpp        # 히터 가동 시간 및 사이클 추적
│   └── HsmsServer.h/.cpp       # HSMS Passive 서버, SECS-II 핸들러
└── CMakeLists.txt
```

## 4.2 Equipment State Machine 설계

### 4.2.1 상태 열거형

```cpp
enum class EquipState {
    IDLE,
    RUNNING,
    ALARM,
    INTERLOCK
};
```

### 4.2.2 상태 전이 테이블

| 현재 상태 | 이벤트 | 조건 (guard) | 동작 (action) | 다음 상태 |
|----------|-------|-------------|--------------|----------|
| IDLE | CMD_START | 도어 닫힘 & 인터락 없음 & 센서 정상 | 릴레이 ON, CEID-2, CEID-5 | RUNNING |
| RUNNING | CMD_STOP | — | 릴레이 OFF, 팬 ON(냉각), CEID-6, CEID-1 | IDLE |
| RUNNING | TEMP_HIGH | 온도 > 65°C | 릴레이 OFF, 팬 ON, 부저 ON, S5F1(ALID-1) | ALARM |
| RUNNING | DOOR_OPEN | 도어 열림 | 릴레이 OFF, 부저 ON, S5F1(ALID-5) | INTERLOCK |
| RUNNING | TEMP_INTERLOCK | 온도 > 68°C | 릴레이 OFF, 부저 ON | INTERLOCK |
| ALARM | RESET | 이상 원인 해소 | 팬 OFF, 부저 OFF, S5F1 CLEAR, CEID-1 | IDLE |
| INTERLOCK | RESET | 인터락 원인 해소 & 도어 닫힘 | 팬 OFF, 부저 OFF, CEID-1 | IDLE |
| ANY | SENSOR_ERROR | 센서 오류 3회 연속 | S5F1(ALID-4) | ALARM |
| ANY | HB_TIMEOUT | HB 10초 미수신 | S5F1(ALID-6) | ALARM |

### 4.2.3 FSM 처리 흐름

```cpp
void StateMachine::processEvent(EquipEvent e) {
    for (auto& t : transitionTable) {
        if (t.from == currentState && t.trigger == e && t.guard()) {
            prev = currentState;
            currentState = t.to;
            t.action();
            if (stateChangeCallback) stateChangeCallback(prev, currentState);
            return;
        }
    }
}
```

## 4.3 FaultDiagnosis 설계 (FR-012~015)

```cpp
class FaultDiagnosis {
public:
    // 센서 데이터 수신 시 호출
    DiagResult analyze(const SensorData& data, bool heaterOn);
};

struct DiagResult {
    bool tempHigh;           // 온도 > TEMP_HIGH_THRESH (65°C)
    bool tempInterlock;      // 온도 > TEMP_INTERLOCK_THRESH (68°C)
    bool heaterCurrAbnormal; // 히터 ON인데 전류 비정상
    bool sw102tTripped;      // SW-102T 동작 감지
    bool sensorError;        // SHT31 또는 INA219 오류
};
```

### 진단 로직

```
analyze(data, heaterOn):
  result.tempHigh      = (data.temperature > TEMP_HIGH_THRESH)
  result.tempInterlock = (data.temperature > TEMP_INTERLOCK_THRESH)

  if heaterOn:
    result.heaterCurrAbnormal = (data.current_mA < HEATER_CURR_MIN ||
                                  data.current_mA > HEATER_CURR_MAX)
    result.sw102tTripped = (data.current_mA < HEATER_CURR_MIN &&
                             data.temperature > SW102T_DETECT_TEMP)
  else:
    result.heaterCurrAbnormal = (data.current_mA > HEATER_OFF_CURR_MAX)

  result.sensorError = (data.sht31_error != 0 || data.ina219_error != 0)
```

## 4.4 InterlockChecker 설계 (FR-016~019)

```cpp
class InterlockChecker {
public:
    bool checkStartCondition();   // START 전 인터락 검사 (FR-018)
    bool checkDoorInterlock();    // 도어 열림 인터락 (FR-016)
    bool checkTempInterlock();    // 과온 소프트웨어 인터락 (FR-017)
    bool isInterlockClear();      // RESET 가능 여부 (FR-019)
};
```

## 4.5 PMTracker 설계 (FR-020~024)

```cpp
class PMTracker {
    uint64_t heaterOnStartMs;   // 히터 ON 시작 시각
    uint64_t totalOnTimeSec;    // 누적 가동 시간 (초)
    uint32_t cycleCount;        // 히터 ON→OFF 사이클 횟수
    bool     heaterOn;

public:
    void onHeaterOn();           // 히터 ON 시 호출
    void onHeaterOff();          // 히터 OFF 시 호출 (가동 시간 누적, 사이클++)
    bool isPMDue();              // PM 임계값 초과 여부
    void reset();                // PM_RESET 명령 시 카운터 초기화
    uint64_t getOnTimeSec();
    uint32_t getCycleCount();
};
```

### PMTracker 동작

```
onHeaterOn():
  heaterOn = true
  heaterOnStartMs = nowMs()

onHeaterOff():
  if heaterOn:
    totalOnTimeSec += (nowMs() - heaterOnStartMs) / 1000
    cycleCount++
    heaterOn = false

isPMDue():
  return (totalOnTimeSec / 3600 >= PM_HOUR_THRESHOLD ||
          cycleCount >= PM_CYCLE_THRESHOLD)
```

## 4.6 AlarmManager 설계 (FR-036~038)

### 알람 조건 판단 테이블

| ALID | 알람명 | 발생 조건 | 해제 조건 |
|------|-------|---------|---------|
| 1 | TEMP_HIGH | 온도 > 65°C | 온도 ≤ 63°C |
| 2 | TEMP_LOW | RUNNING 5분 후 온도 < 목표치 | 온도 ≥ 목표치 |
| 3 | HEATER_CURR_ABNORMAL | 히터 전류 이상 | 전류 정상 복귀 |
| 4 | SENSOR_ERROR | 센서 오류 3회 연속 | 센서 오류 해소 |
| 5 | DOOR_OPEN | 도어 열림 | — (RESET 명령 필요) |
| 6 | UART_COMM_ERROR | HB 10초 미수신 | UART 재연결 |
| 7 | PM_DUE | PM 임계값 초과 | PM_RESET 명령 |

### 알람 발생/해제 흐름

```cpp
void AlarmManager::checkAndSet(AlarmId id) {
    bool wasClear = !isActive(id);
    // 조건 확인 후 활성화
    if (wasClear && conditionMet(id)) {
        activeAlarms.set(id);
        hsmsSrv.sendAlarmReport(id, true, alarmText[id]);
        sm.processEvent(EVENT_ALARM);
    }
}

void AlarmManager::tryClear(AlarmId id) {
    bool wasSet = isActive(id);
    if (wasSet && !conditionMet(id)) {
        activeAlarms.clear(id);
        hsmsSrv.sendAlarmReport(id, false, alarmText[id]);
    }
}
```

## 4.7 HSMS 서버 설계

### HSMS 세션 상태

```
NOT_CONNECTED
    │ accept()
    ▼
CONNECTED (T7=10초 Select 대기)
    │ SelectReq 수신
    ▼
SELECTED
    │ SECS-II 메시지 교환
    │ Linktest (T6 주기)
    ▼
NOT_CONNECTED
```

### S/F 핸들러 디스패치

| S/F | 핸들러 | 담당 FR |
|-----|-------|---------|
| S1F1 | handleAreYouThere() | — |
| S1F13 | handleOnlineReq() | FR-039~040 |
| S1F3 | handleStatusVarReq() | FR-043 |
| S2F41 | handleRemoteCmd() | FR-042 |
| S5F5 | handleAlarmListReq() | FR-036 |
| 미정의 | → S9F* | — |

### Remote Command 처리 흐름

```
handleRemoteCmd(S2F41):
  cmd = decode(msg)
  switch cmd:
    "START"    → sm.processEvent(CMD_START)
    "STOP"     → sm.processEvent(CMD_STOP)
    "RESET"    → sm.processEvent(CMD_RESET)
    "ACK_ALARM <alid>" → alarmMgr.ack(alid)
    "PM_RESET" → pmTracker.reset(); alarmMgr.tryClear(ALID_PM_DUE)
  reply S2F42(CMDA_ACK or CMDA_DENIED)
```

## 4.8 EC 메인 루프

```
main():
  StateMachine  sm
  AlarmManager  alarmMgr(sm)
  FaultDiagnosis faultDiag
  InterlockChecker interlockChk
  PMTracker     pmTracker
  HsmsServer    hsmsSrv(5000, sm, alarmMgr, pmTracker)
  DeviceComm    devComm(port)

  sm.setStateChangeCallback(...)
  devComm.setFrameCallback(onFrame)

  hsmsSrv.start()
  devComm.open()

  while(true):
    sleep(1000ms)
    // Heartbeat 감시
    if lastRx > 0 && elapsed > HB_TIMEOUT:
      alarmMgr.checkAndSet(ALID_UART_COMM_ERROR)
    // PM 주기 감시
    if pmTracker.isPMDue():
      alarmMgr.checkAndSet(ALID_PM_DUE)

onFrame(ParsedFrame f):
  switch f.type:
    MSG_SENSOR_DATA:
      DiagResult r = faultDiag.analyze(sensor, heaterOn)
      if r.tempHigh   → alarmMgr.checkAndSet(ALID_TEMP_HIGH)
      if r.tempInterlock → sm.processEvent(TEMP_INTERLOCK)
      if r.heaterCurrAbnormal → alarmMgr.checkAndSet(ALID_HEATER_CURR)
      if r.sw102tTripped → sm.processEvent(SW102T_TRIP)
      hsmsSrv.sendEventReport(CEID_SENSOR_DATA, payload)

    MSG_DOOR_EVENT:
      if door_open && sm.isRunning():
        sm.processEvent(DOOR_OPEN)
        alarmMgr.checkAndSet(ALID_DOOR_OPEN)

    MSG_BUTTON_EVENT:
      if btn pressed:
        if IDLE  → sm.processEvent(CMD_START)
        if RUNNING → sm.processEvent(CMD_STOP)

    MSG_HEARTBEAT_REQ:
      update lastRxTimeMs
```

---

# 5. 내부 프로토콜 프레임 구조 (STM32↔EC)

```
| SOF(1) | TYPE(1) | SEQ(1) | LEN_L(1) | LEN_H(1) | PAYLOAD(N) | CRC_L(1) | CRC_H(1) |
```

- SOF: 0xAA
- CRC: CRC16-CCITT (poly=0x1021, init=0xFFFF)
- CRC 범위: TYPE ~ PAYLOAD 끝

---

# 6. Host 프로그램 설계 (macOS, Python)

## 6.1 모듈 구조

```
equip-control-host/
├── main.py             # 진입점, curses UI 루프
├── config.py           # EC_HOST, EC_PORT 설정
├── hsms_client.py      # HSMS 클라이언트 (TCP 연결, 메시지 송수신)
├── message_handler.py  # S/F 메시지 처리, 상태 갱신
└── ui.py               # curses 기반 챔버 상태 표시 UI
```

## 6.2 curses UI 구성

```
==================================================
       EQUIP-CONTROL HOST — 가상 공정 챔버
==================================================
[State]  Equipment: IDLE     | Online: YES
[Chamber] Temp: 24.5C  Humi: 41.0%
[Heater]  Current: 850mA  Voltage: 12.0V
[Door]    CLOSED
[PM]      On-Time: 12.5h  Cycles: 87  Status: OK
[Alarm]   Active: 0  (알람 없음)
--------------------------------------------------
[Events] (최근 10건)
  10:01:23 | CEID-2 STATE_RUNNING
  10:01:18 | CEID-5 HEATER_ON
--------------------------------------------------
Commands: START | STOP | RESET | ACK_ALARM <alid> | PM_RESET | QUIT
>
```

## 6.3 명령어 목록

| 명령 | 설명 |
|------|------|
| `START` | 챔버 IDLE → RUNNING 전환 (히터 ON) |
| `STOP` | 챔버 RUNNING → IDLE 전환 (히터 OFF, 냉각) |
| `RESET` | ALARM/INTERLOCK → IDLE 전환 |
| `ACK_ALARM <alid>` | 알람 확인 처리 (alid: 1~7) |
| `PM_RESET` | PM 카운터 초기화 |
| `QUIT` | 프로그램 종료 |

---

# 7. 핀 매핑 요약

| 부품 | GPIO | Arduino | 방향 | 비고 |
|------|------|---------|------|------|
| SHT31 SDA | PB9 | D14 | I2C | I2C1, addr=0x45 |
| SHT31 SCL | PB8 | D15 | I2C | I2C1 |
| INA219 SDA | PB9 | D14 | I2C | I2C1 공유, addr=0x40 |
| INA219 SCL | PB8 | D15 | I2C | I2C1 공유 |
| 릴레이 (히터) | PB0 | D3 | Output | 히터 회로 ON/OFF |
| 리미트 스위치 | PA1 | A1 | Input | 도어 감지, Pull-Up |
| 냉각 팬 | PB4 | D5 | Output | IRF520, 1kΩ 직렬 |
| 부저 | PB10 | D6 | Output | 알람 출력 |
| 포텐셔미터 | PA0 | A0 | Analog | ADC1_IN0 |
| LD2 LED | PA5 | — | Output | 상태 표시 |
| 버튼 (B1) | PC13 | — | Input | 내장 버튼 |
| 버튼 (로컬) | PB5 | D4 | Input | 외부 버튼 |
