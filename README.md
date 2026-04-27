# 히터 과온 알람 진단 및 안전 복구 시스템

칠러(Chiller) CS 엔지니어 포트폴리오 프로젝트입니다.

히터(열원)와 냉각팬을 이용한 개방형 온도제어 구조로 **과온 알람 발생 → 자동 안전 정지 → 현장 진단 → 복구 조건 검증 → 재가동** 의 CS 대응 사이클 전체를 실제 하드웨어로 구현합니다.

CS 엔지니어의 핵심 역량인 **알람 원인 특정, 인터락 동작 확인, 복구 절차 집행**을 실증합니다.

---

## CS 대응 사이클

```
장비 가동 (HEATING)
  │
  │ 과온 감지 (설정 온도(SP) + 1°C, 5초 지속)
  ▼
WARNING (ALM-01) ── 냉각팬 ON / 부저 간헐 경보 / EC 진단 패널 경고 표시
  │                  PID 제어 유지, 팬 냉각으로 SP 복귀 유도
  │
  │ 과온 지속 (SP + 2°C, 10초 지속)
  ▼
ALARM (ALM-02) ──── 히터 자동 OFF / 팬 자동 ON / 부저 연속
  │                  EC 진단 패널 원인 표시 + 복구 조건 모니터링
  │
  │ CS 엔지니어 현장 대응
  │  ① 알람 코드 확인 (ALM-01 / ALM-02 / ALM-03)
  │  ② 원인 분류 (과온 원인 진단 트리 — TRG-001 참조)
  │  ③ 복구 조건 확인 (온도 ≤ SP - 2°C)
  │  ④ EC CLI: RESET 명령 입력
  │     → 조건 미충족: NACK:RESET,TEMP_HIGH (재가동 불가)
  │     → 조건 충족:  ACK:RESET → IDLE 복귀
  ▼
정상 복귀 (IDLE) ── START 명령으로 재가동
```

---

## 시스템 구성

```
┌──────────────────────────────────────────────────────┐
│  EC (Equipment Controller, macOS Python)             │
│                                                      │
│  - 알람 진단 패널 (알람 코드 / 원인 / 복구 조건)    │
│  - 복구 조건 실시간 모니터링 (온도 ≤ SP-2°C ✓/✗)  │
│  - RESET 명령 허용 / 거부 피드백                    │
│  - 센서 데이터 CSV 자동 로깅                        │
└─────────────────────┬────────────────────────────────┘
                      │ UART 115200bps (text, \r\n)
                      │ ST-LINK USB VCP
┌─────────────────────▼────────────────────────────────┐
│  FW (STM32F446RE NUCLEO, C)                          │
│                                                      │
│  - DHT22 온도 측정 (PB5/D4, 2s 주기)               │
│  - PID 히터 제어 (TIM3_CH3, 1kHz PWM)              │
│  - WARNING / ALARM 2단계 판정 및 자동 안전 대응     │
│  - 팬 릴레이 제어 (PA0) / 부저 제어 (PB10)        │
│  - RESET 복구 조건 검증 후 허용 / 거부 응답         │
└──────────────────────────────────────────────────────┘
```

---

## 알람 정의 (AIM-001 기준)

| ALID | 알람명 | 발생 조건 | FW 자동 대응 | 복구 |
|------|--------|---------|------------|------|
| ALM-01 | TEMP_WARNING | 온도 > SP + 1°C, 5초 지속 | 부저 간헐, EC WARNING 보고 | 온도 복귀 시 자동 해제 |
| ALM-02 | TEMP_ALARM | 온도 > SP + 2°C, 10초 지속 | 히터 OFF, 팬 ON, 부저 연속 | EC RESET (수동, 복구 조건 검증) |
| ALM-03 | SENSOR_ERROR | DHT22 3회 연속 읽기 실패 | 히터 OFF, 부저 ON | 센서 복구 후 EC RESET |

---

## EC 진단 패널 (ALARM 상태 예시)

```
────────────────────────────────────────────────────────
 히터 과온 알람 진단 및 안전 복구 시스템
────────────────────────────────────────────────────────
[State]       FW: ALARM
[Sensor]      Temperature: 32.5°C   Setpoint: 30.0°C   Duty: 0.0%
[Run]         Elapsed: 127.3s   Reach: 45.2s   Settle: ---
[KPI]         Peak: 32.8°C   Overshoot: 2.8°C
────────────────────────────────────────────────────────
[Alarm]  ALM-02 TEMP_ALARM
    발생: 온도 > SP+2°C, 10초 지속
    복구: 온도 ≤ SP-2°C 복귀 후 RESET

    [복구 조건] Temperature ≤ 28.0°C : ✗  (현재 32.5°C)
      → 냉각 대기 중...
────────────────────────────────────────────────────────
```

---

## 하드웨어 구성

| 부품 | 역할 | 인터페이스 |
|------|------|-----------|
| STM32 NUCLEO-F446RE | 메인 MCU (180MHz) | — |
| DHT22 | 히터 주변 온도 측정 (±0.5°C) | 단선 (PB5/D4) |
| 카프톤 필름 히터 24V/~26W | 가열 (열원) | — |
| N-ch MOSFET 모듈 | 히터 PWM 제어 (1kHz) | PB0/A3 (TIM3_CH3) |
| 릴레이 모듈 + 5V 팬 | 강제 냉각 | PA0/A0 |
| DC-DC 컨버터 (24V→5V) | 팬 전원 | — |
| 액티브 부저 (3.3V) | 알람 경보 | PB10/D6 |
| WANPTEK 파워 서플라이 | 24V / 1.5A 제한 | — |

---

## UART 통신 프로토콜

**FW → EC (주기/이벤트):**

```
DATA:28.3,30.0,HEATING,NONE,50.0,23.1,0,0.0,0.0,0.0,0,0.0   # 1초 주기
EVENT:WARN,ALM-01      # WARNING 발생
EVENT:ALARM,ALM-02     # ALARM 발생
EVENT:CLEAR,ALM-01     # 알람 해제
EVENT:SETTLED,67.8     # 안정화 확인 (SP ± 1°C, 10초 유지)
ACK:START              # 명령 수락
NACK:RESET,TEMP_HIGH   # 복구 조건 미충족 (RESET 거부)
```

**EC → FW (명령):**

```
SET:30.0   # 온도 설정점 변경
START      # 가열 시작 (IDLE → HEATING)
STOP       # 가열 정지
RESET      # 알람 복구 시도 (조건 검증 후 허용/거부)
STATUS     # 즉시 DATA 응답 요청
```

---

## 실행 방법

### 1단계 — 펌웨어 플래시 (STM32CubeIDE)

```
1. STM32CubeIDE 실행
2. chamber-fw 프로젝트 임포트
3. Run (▶) — 빌드 → Flash → 실행
```

### 2단계 — EC 실행

```bash
cd chamber-ec
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python ec.py /dev/cu.usbmodem*
```

### 재접속 시

```bash
source chamber-ec/.venv/bin/activate
python chamber-ec/ec.py /dev/cu.usbmodem*
```

### EC 명령어

| 명령 | 설명 |
|------|------|
| `SET:<온도>` | 온도 설정점 변경 (예: `SET:30.0`) |
| `START` | 가열 시작 (IDLE → HEATING) |
| `STOP` | 가열 정지 |
| `RESET` | 알람 복구 시도 (복구 조건 검증 후 허용/거부) |
| `STATUS` | 현재 상태 즉시 조회 |
| `QUIT` | EC 종료 |

---

## 디렉토리 구조

```
equip-control/
├── chamber-fw/                 # STM32 펌웨어 (C, HAL)
│   └── Core/Src/
│       ├── main.c              # 상태 머신, 알람 로직, PID, UART
│       └── dht22.c             # DHT22 온도 센서 드라이버
├── chamber-ec/                 # EC 소프트웨어 (Python)
│   ├── ec.py                   # 진단 패널 UI, UART 통신, CSV 로깅
│   └── requirements.txt
└── docs/
    ├── hw/
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
| [SCN-001](docs/hw/SCN-001.md) | 과온 알람 시나리오 및 원인별 진단 가이드 — 타임라인, 원인 A~D 진단 방법, 복구 절차 |
| [AIM-001](docs/hw/AIM-001.md) | 알람 매트릭스 — ALM-01~03 발생 조건, 자동 대응, 복구 조건 정의 |
| [TRG-001](docs/hw/TRG-001.md) | 현장 대응 절차서 — 원인 진단 트리, 단계별 조치, 복구 검증 |
| [EFS-001](docs/hw/EFS-001.md) | 장비 기능 명세 — 상태 머신, 알람 동작, UART 프로토콜 |
| [TPV-001](docs/hw/TPV-001.md) | 테스트 계획 및 검증 결과 (TC-01~05) |
| [HDS-001](docs/hw/HDS-001.md) | HW 설계 명세 — DHT22, MOSFET PWM 회로, 전원 아키텍처 |
| [IO-001](docs/hw/IO-001.md) | I/O 목록 및 배선도 |
