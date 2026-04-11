# 가상 공정 챔버 기반 장비 이상 진단·인터락·PM 시뮬레이터
  
Lock&Lock 밀폐 용기를 가상 공정 챔버로 삼아 히터 제어·과온 인터락·PM 스케줄링을 구현하고  
SECS/GEM(HSMS + SECS-II) 프로토콜로 Host에 실시간 보고합니다.

```
STM32 FW (C)              EC (macOS, C++)              Host (macOS, Python)
────────────              ───────────────              ────────────────────
FreeRTOS 6태스크      →   DeviceComm (UART)      →     HsmsClient (TCP)
SHT31 온습도              StateMachine                  curses GUI
INA219 전류/전압           FaultDiagnosis                챔버 상태 표시
릴레이/팬/부저 제어         InterlockChecker              알람/PM 모니터링
도어 리미트 스위치          AlarmManager (ALID 1~7)       S2F41 명령 입력
UART 바이너리 프레임        PMTracker
                          HsmsServer (HSMS/TCP)
                          ※ EC · Host 동일 MacBook, localhost:5000
```

---

## 디렉토리 구조

```
equip-control/
├── equip-control-fw/       # STM32 펌웨어 (STM32CubeIDE)
├── equip-control-ec/       # EC 소프트웨어 (C++, CMake)
├── equip-control-host/     # Host 프로그램 (Python)
├── SRS.md                  # 소프트웨어 요구사항 명세 (IEEE 29148-2018, v2.0)
├── SDD.md                  # 소프트웨어 설계 문서 (IEEE 1016-2009, v2.0)
└── ARCHITECTURE.md         # 3계층 아키텍처 다이어그램
```

---

## 하드웨어 구성

| 부품 | 역할 |
|------|------|
| STM32 NUCLEO-F411RE | 메인 MCU |
| SHT31 | 챔버 내부 온습도 측정 (I2C, 0x45) |
| INA219 | 히터 전류/전압 측정 (I2C, 0x40) |
| 카프톤 히터 12V/10W | 챔버 가열 (릴레이 ON/OFF 제어) |
| 5V 릴레이 모듈 | 히터 전원 스위칭 |
| SW-102T (72°C) | 하드웨어 과온 인터락 (히터 직렬 회로) |
| DC 잭 12V | 히터 전원 입력 |
| 5V 팬 | 챔버 냉각 (MOSFET 제어) |
| 리미트 스위치 | 챔버 도어 감지 (PA1) |
| 부저 | 알람 경보 (PB10) |
| 포텐셔미터 | 아날로그 입력 테스트 (PA0) |

### 이중 과온 인터락

```
65°C 초과 → EC 소프트웨어 ALARM 전이 → 릴레이 OFF 명령
68°C 초과 → EC 소프트웨어 INTERLOCK 전이 → 릴레이 OFF 명령
72°C 도달 → SW-102T 하드웨어 차단 → INA219 전류 소실 감지 → EC INTERLOCK 전이
도어 열림 → EC 소프트웨어 INTERLOCK 전이 → 릴레이 OFF 명령
```

---

## 1단계 — 펌웨어 빌드 및 플래시 (STM32CubeIDE)

### 사전 준비
- STM32CubeIDE 설치
- STM32 NUCLEO-F411RE 보드를 USB로 연결

### 빌드 및 플래시

```
1. STM32CubeIDE 실행
2. File → Import → General → Existing Projects into Workspace
3. equip-control-fw 폴더 선택 → Finish
4. Run (▶) 버튼 클릭
   → 자동으로 빌드 → Flash → 실행
```

### 플래시 후 동작
| LED | 상태 |
|-----|------|
| LD3 (빨간) | 항상 점등 — 3.3V 전원 |
| LD1 (빨간) | USB 연결 중 점등 — ST-LINK |
| LD2 (초록) | 챔버 상태 표시 (IDLE: OFF, RUNNING: ON, ALARM: 점멸) |

---

## 2단계 — EC 빌드 및 실행 (macOS, 터미널 1)

### 사전 준비
- CMake 설치 (`brew install cmake`)
- Xcode Command Line Tools (`xcode-select --install`)

### 빌드

```bash
cd equip-control-ec
cmake -B build && cmake --build build
```

### 실행

```bash
# 장치명 확인: ls /dev/tty.usbmodem*
./build/ec /dev/tty.usbmodem21303
```

### EC 실행 시 동작
- STM32 UART 연결 후 센서 데이터 수신 시작
- TCP 포트 5000에서 Host 연결 대기
- 콘솔에 수신 로그 출력:
  ```
  [EC] UART connected: /dev/tty.usbmodem21303
  [HSMS] Listening on port 5000
  [STATE] IDLE
  [SENSOR] temp=25.3C humi=42.1% curr=0mA volt=12.0V
  [HB] seq=0
  ```

---

## 3단계 — Host 실행 (macOS, 터미널 2)

### 사전 준비

```bash
# Python 3.8 이상
python3 --version
```

### 실행

```bash
cd equip-control-host
python3 main.py
```

### 화면 구성

```
==================================================
   가상 공정 챔버 장비 제어 시스템
==================================================
[State]   Chamber: IDLE      | Online: YES
[Sensor]  Temp: 25.3C  Humi: 42.1%  Curr: 0mA  Volt: 12.0V
[PM]      가동시간: 0.0h  사이클: 0  (PM 불필요)
[Alarm]   Active: 0  (알람 없음)
--------------------------------------------------
[Events] (최근 10건)
  10:01:23 | CEID-8 (SENSOR_DATA)
--------------------------------------------------
Commands: START | STOP | RESET | ACK_ALARM <alid> | PM_RESET | QUIT
>
```

### 명령어

| 명령 | 설명 |
|------|------|
| `START` | 챔버 가열 시작 (IDLE → HEATING) |
| `STOP` | 챔버 가열 중지 (RUNNING → COOLING) |
| `RESET` | 알람/인터락 해제 시도 (→ IDLE) |
| `ACK_ALARM <alid>` | 알람 확인 (alid: 1~7) |
| `PM_RESET` | PM 카운터 초기화 |
| `QUIT` | 프로그램 종료 |

---

## 전체 실행 순서

```
1. 하드웨어 연결 확인 (SHT31, INA219, 릴레이, 팬, 리미트 스위치)
2. STM32 보드를 MacBook에 USB 연결
3. 터미널 1 — EC 실행:
     cd equip-control-ec && ./build/ec /dev/tty.usbmodem21303
4. 터미널 2 — Host 실행:
     cd equip-control-host && python3 main.py
5. Host 화면에서 Online: YES 확인
6. 명령 입력: START → (온도 상승 관찰) → STOP → RESET
```

---

## 알람 ID 목록

| ALID | 알람명 | 발생 조건 |
|------|--------|-----------|
| 1 | TEMP_HIGH | 챔버 온도 > 65°C |
| 2 | TEMP_CRITICAL | 챔버 온도 > 68°C (인터락) |
| 3 | DOOR_OPEN | 도어 열림 감지 (인터락) |
| 4 | CURRENT_LOSS | 히터 전류 소실 (SW-102T 동작) |
| 5 | SENSOR_ERROR | SHT31 또는 INA219 오류 |
| 6 | COMM_ERROR | Heartbeat 10초 이상 미수신 |
| 7 | PM_DUE | PM 주기 도래 |

---

## 챔버 상태 머신

```
IDLE ──START──► HEATING ──온도 안정──► RUNNING
  ▲                │                     │
  │              과온/도어              STOP
RESET             │                     │
  │               ▼                     ▼
  └──── ALARM ◄──65°C        COOLING ──냉각 완료──► IDLE
         │
       68°C / 도어열림
         │
         ▼
      INTERLOCK ──RESET──► IDLE
         │
       PM 도래
         │
         ▼
    PM_REQUIRED ──PM_RESET──► IDLE
```

---

## 내부 프로토콜 프레임 구조 (STM32↔EC)

```
| SOF(1) | TYPE(1) | SEQ(1) | LEN_L(1) | LEN_H(1) | PAYLOAD(n) | CRC_L(1) | CRC_H(1) |
```

- SOF: 0xAA
- CRC: CRC16-CCITT (poly=0x1021, init=0xFFFF)

| TYPE | 방향 | 내용 |
|------|------|------|
| 0x01 | STM32→EC | MSG_SENSOR_DATA (온도/습도/전류/전압/도어) |
| 0x02 | STM32→EC | MSG_HEARTBEAT |
| 0x03 | STM32→EC | MSG_BUTTON_EVENT |
| 0x04 | STM32→EC | MSG_DOOR_EVENT |
| 0x10 | EC→STM32 | MSG_CMD_RELAY (히터 릴레이 ON/OFF) |
| 0x11 | EC→STM32 | MSG_CMD_FAN (팬 ON/OFF) |
| 0x12 | EC→STM32 | MSG_CMD_BUZZER (부저 ON/OFF) |
| 0x13 | EC→STM32 | MSG_CMD_LED (LED 패턴) |

---

## 관련 문서

- [SRS.md](SRS.md) — 소프트웨어 요구사항 명세 (IEEE 29148-2018, v2.0)
- [SDD.md](SDD.md) — 소프트웨어 설계 문서 (IEEE 1016-2009, v2.0)
- [ARCHITECTURE.md](ARCHITECTURE.md) — 3계층 아키텍처 다이어그램
