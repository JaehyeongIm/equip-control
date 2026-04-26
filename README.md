# 소형 챔버 온도제어 및 알람진단 시스템

밀폐 용기를 소형 챔버로 삼아 **PID 히터 제어 → 과온 WARNING/ALARM 발생 → 자동 안전 정지 → CS 진단 → 복구**의 전체 사이클을 실제 하드웨어로 구현합니다.

칠러(Chiller) CS 엔지니어 포트폴리오 프로젝트입니다. 온도 제어 정밀도, 2단계 알람 체계, 현장 대응 복구 절차를 실증합니다.

---

## 시스템 구성

```
┌──────────────────────────────────────────────────────┐
│  EC (Equipment Controller, macOS C++)                │
│                                                      │
│  - SET SP / START / STOP / RESET 명령               │
│  - 온도 트렌드 실시간 모니터링                       │
│  - WARNING / ALARM 진단 패널 표시                    │
│  - 복구 조건 확인 및 RESET 명령                      │
└─────────────────────┬────────────────────────────────┘
                      │ UART 115200bps (text, \r\n)
                      │ ST-LINK USB VCP
┌─────────────────────▼────────────────────────────────┐
│  FW (STM32F446RE NUCLEO, C)                          │
│                                                      │
│  - DS18B20 온도 측정 (OneWire, 750ms)               │
│  - PID 히터 PWM 제어 (TIM3_CH3, 1kHz)              │
│  - 팬 릴레이 제어 (PA0)                             │
│  - 부저 제어 (PB10, WARNING/ALARM 경보)             │
│  - 도어 스위치 감지 (PA1, 페일세이프)               │
│  - WARNING / ALARM 판정 및 EC 보고                  │
└──────────────────────────────────────────────────────┘
```

---

## 알람 시나리오: 과온 (Over-Temperature)

```
HEATING (PID 제어 중)
  │
  │ DS18B20 > SP + 3°C, 5초 지속
  ▼
WARNING (ALM-01) — 부저 간헐, EC 경고, PID 유지
  │
  │ DS18B20 > SP + 5°C, 10초 지속
  ▼
ALARM (ALM-02) — 히터 OFF, 팬 ON, 부저 연속
  │              EC 진단 패널 활성화
  │
  │ 온도 ≤ SP - 2°C 복귀 후 EC RESET
  ▼
IDLE — 정상 복귀
```

---

## 알람 정의 (AIM-001 기준)

| ALID | 알람명 | 발생 조건 | FW 자동 대응 | 복구 |
|------|--------|---------|------------|------|
| ALM-01 | TEMP_WARNING | DS18B20 > SP + 3°C, 5초 지속 | 부저 간헐, EC WARNING 보고 | 온도 복귀 시 자동 해제 |
| ALM-02 | TEMP_ALARM | DS18B20 > SP + 5°C, 10초 지속 | 히터 OFF, 팬 ON, 부저 연속 | EC RESET (수동) |
| ALM-03 | SENSOR_ERROR | DS18B20 3회 연속 읽기 실패 | 히터 OFF, 부저 ON | 센서 복구 후 EC RESET |
| ALM-04 | COMM_ERROR | Heartbeat 10초 미수신 | FW 히터 OFF (안전 모드) | EC 재연결 후 자동 해제 |
| ALM-05 | DOOR_OPEN | 도어 스위치 PA1 HIGH | 히터 OFF, EC 경고 | 도어 닫힘 시 자동 해제 |

---

## 하드웨어 구성

| 부품 | 역할 | 인터페이스 |
|------|------|-----------|
| STM32 NUCLEO-F446RE | 메인 MCU (180MHz) | — |
| DS18B20 | 챔버 온도 측정 (±0.5°C, 12-bit) | OneWire (PB5/D4) |
| 카프톤 필름 히터 24V/~26W | 챔버 가열 | — |
| N-ch MOSFET 모듈 | 히터 PWM 제어 (1kHz) | PB0/A3 (TIM3_CH3) |
| 릴레이 모듈 + 5V 팬 | 강제 냉각 | PA0/A0 |
| DC-DC 컨버터 (24V→5V) | 팬 전원 | — |
| 액티브 부저 (3.3V) | 알람 경보 | PB10/D6 |
| 도어 리미트 스위치 (NO) | 도어 감지 (페일세이프) | PA1/A1 |
| WANPTEK 파워 서플라이 | 24V / 1.5A 제한 | — |
| 밀폐 용기 (~1L) | 소형 챔버 | — |

---

## UART 통신 프로토콜

**FW → EC (주기/이벤트):**

```
DATA:28.3,30.0,HEATING,NONE      # 온도, SP, 상태, 알람 (1초 주기)
EVENT:WARN,ALM-01                # WARNING 발생
EVENT:ALARM,ALM-02               # ALARM 발생
EVENT:CLEAR,ALM-01               # 알람 해제
HB                               # Heartbeat (5초 주기)
ACK:START                        # 명령 수락
NACK:RESET,TEMP_HIGH             # 명령 거부 (복구 조건 미충족)
```

**EC → FW (명령):**

```
SET:30.0    # 온도 설정점 변경
START       # 가열 시작 (IDLE → HEATING)
STOP        # 가열 정지
RESET       # 알람 복구 (조건 충족 시 → IDLE)
STATUS      # 즉시 DATA 응답 요청
```

---

## 실행 방법

### 1단계 — 펌웨어 플래시 (STM32CubeIDE)

```
1. STM32CubeIDE 실행
2. equip-control-fw 프로젝트 임포트
3. Run (▶) — 빌드 → Flash → 실행
```

### 2단계 — EC 빌드 및 실행

```bash
cd equip-control-ec
cmake -B build && cmake --build build
./build/ec /dev/tty.usbmodem*
```

### EC 명령어

| 명령 | 설명 |
|------|------|
| `SET:<온도>` | 온도 설정점 변경 (예: `SET:30.0`) |
| `START` | 가열 시작 (IDLE → HEATING) |
| `STOP` | 가열 정지 |
| `RESET` | 알람 복구 (복구 조건 충족 시 → IDLE) |
| `STATUS` | 현재 상태 즉시 조회 |
| `QUIT` | EC 종료 |

---

## 디렉토리 구조

```
equip-control/
├── equip-control-fw/           # STM32 펌웨어 (C, HAL)
│   └── Core/Src/
│       ├── main.c
│       ├── ds18b20.c           # OneWire 온도 센서 드라이버
│       └── uart_comm.c         # EC 통신 처리
├── equip-control-ec/           # EC 소프트웨어 (C++, CMake)
├── heater-test/                # 단계별 하드웨어 테스트 (개발용)
└── docs/
    ├── hw/                     # 설계 및 운영 문서
    │   ├── EFS-001.md          # Equipment Functional Specification
    │   ├── HDS-001.md          # Hardware Design Specification
    │   ├── IO-001.md           # I/O List & Wiring Diagram
    │   ├── AIM-001.md          # Alarm / Interlock Matrix
    │   ├── TRG-001.md          # Troubleshooting & Recovery Guide
    │   └── TPV-001.md          # Test Plan & Verification Report
    └── troubleshooting/        # 개발 중 트러블슈팅 기록
```

---

## 핵심 문서

| 문서 | 내용 |
|------|------|
| [EFS-001](docs/hw/EFS-001.md) | 장비 기능 명세 (상태 머신, 알람 동작, UART 프로토콜, EC 진단 패널) |
| [AIM-001](docs/hw/AIM-001.md) | 알람 매트릭스 (ALM-01~05 발생 조건, 자동 대응, 복구 조건) |
| [TRG-001](docs/hw/TRG-001.md) | 현장 대응 절차서 (원인 진단 트리, 단계별 조치, 복구 검증) |
| [HDS-001](docs/hw/HDS-001.md) | HW 설계 명세 (DS18B20, MOSFET PWM 회로, 전원 아키텍처) |
| [IO-001](docs/hw/IO-001.md) | I/O 목록 및 배선도 (전체 핀 매핑, 배선 구성) |
| [TPV-001](docs/hw/TPV-001.md) | 테스트 계획 및 검증 결과 기록 (TC-01~07) |
