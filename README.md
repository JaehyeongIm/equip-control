# SECS/GEM 기반 가상 챔버 과온 감지 및 안전 복구 시스템

Lock&Lock 밀폐 용기를 가상 공정 챔버로 삼아 **히터 과온 발생 → 자동 인터락 → 원인 진단 → 운영자 가이드 복구**의 전체 사이클을 실제 하드웨어로 구현합니다.

반도체 장비 CS 엔지니어의 현장 대응 역량을 실증하기 위한 포트폴리오 프로젝트입니다.

---

## 시스템 구성

```
┌─────────────────────┐      UART/Binary      ┌─────────────────────┐      HSMS/TCP      ┌──────────────────────┐
│   STM32 FW (C)      │ ──────────────────►   │    EC (C++, macOS)  │ ──────────────────► │  Host (Python, macOS)│
│                     │                       │                     │                    │                      │
│  FreeRTOS 멀티태스크 │                       │  GEM 상태 머신       │                    │  curses 진단 패널    │
│  SHT31 온도 측정     │ ◄──────────────────   │  ALID 알람 판정      │ ◄──────────────── │  원인 후보 표시      │
│  INA219 전류 측정    │                       │  이중 인터락 실행    │                    │  점검 체크리스트     │
│  MOSFET PWM 히터     │                       │  SECS/GEM S5F1 보고  │                    │  복구 조건 추적      │
│  SW-102T HW 인터락   │                       │  S2F41 명령 처리     │                    │  RESET 허용/거부     │
└─────────────────────┘                       └─────────────────────┘                    └──────────────────────┘
```

---

## 시나리오: 과온 (Over-Temperature)

```
HEATING 중
  │ SHT31 온도 > 65°C
  ▼
ALARM 전이 — PWM=0%, 팬 ON, 부저 ON
  │          S5F1 (ALID=1) 보고 → Host 진단 패널 활성화
  │ 온도 > 68°C (계속 상승)
  ▼
INTERLOCK 전이 — S5F1 (ALID=2) 보고
  │ 온도 72°C 도달
  ▼
SW-102T 물리 차단 — INA219 전류 소실 감지 → ALID=4
  │
  ▼
Host: 원인 분류 (전류값 기반) → 점검 항목 표시 → 복구 조건 확인 → RESET
```

---

## 이중 인터락 구조

```
65°C ── SW ALARM      EC 소프트웨어 감지, PWM=0%
68°C ── SW INTERLOCK  EC 소프트웨어 감지, RESET 필요
72°C ── HW INTERLOCK  SW-102T 바이메탈 물리 차단 (EC 독립)
```

---

## 하드웨어 구성

| 부품 | 역할 | 인터페이스 |
|------|------|-----------|
| STM32 NUCLEO-F411RE | 메인 MCU, FreeRTOS | — |
| Sensirion SHT31 | 챔버 온습도 측정 | I2C (0x44) |
| TI INA219 | 히터 전류/전압 측정 | I2C (0x40) |
| 카프톤 필름 히터 12V/10W | 챔버 가열 | — |
| IRLZ44NPBF MOSFET | 히터 PWM 제어 (1kHz) | PB0 (TIM3_CH3) |
| SW-102T 72°C 서모스탯 | HW 과온 인터락 | 히터 직렬 회로 |
| 냉각 팬 5V | 강제 냉각 (ALARM 시 자동 ON) | PB4 (IRF520) |
| 액티브 부저 3.3V | 알람 경보 | PB10 |
| Lock&Lock 밀폐용기 | 가상 공정 챔버 (~1L) | — |

---

## 알람 정의 (AIM-001 기준)

| ALID | 알람명 | 발생 조건 | 자동 대응 |
|------|--------|---------|---------|
| 1 | TEMP_HIGH | SHT31 온도 > 65°C | PWM=0%, 팬 ON, 부저 ON |
| 2 | TEMP_CRITICAL | SHT31 온도 > 68°C | INTERLOCK 전이, S5F1 보고 |
| 4 | HW_INTERLOCK | INA219 전류 < 50mA (PWM ON 중) | EC 화면 경고, 운영자 확인 |
| 5 | SENSOR_ERROR | SHT31 I2C 오류 연속 3회 | PWM=0%, S5F1 보고 |
| 6 | COMM_ERROR | Heartbeat 10초 미수신 | 제어 명령 차단 |

---

## 실행 방법

### 1단계 — 펌웨어 플래시 (STM32CubeIDE)

```
1. STM32CubeIDE 실행
2. equip-control-fw 프로젝트 임포트
3. Run (▶) — 빌드 → Flash → 실행
```

### 2단계 — EC 빌드 및 실행 (터미널 1)

```bash
cd equip-control-ec
cmake -B build && cmake --build build
./build/ec /dev/tty.usbmodem*
```

### 3단계 — Host 실행 (터미널 2)

```bash
cd equip-control-host
python3 main.py
```

### Host 명령어

| 명령 | 설명 |
|------|------|
| `START` | 챔버 가열 시작 (IDLE → HEATING) |
| `STOP` | 챔버 가열 중지 |
| `CHECK <번호>` | 점검 체크리스트 항목 완료 처리 |
| `RESET` | 복구 조건 확인 후 알람 해제 (→ IDLE) |
| `QUIT` | 프로그램 종료 |

---

## 디렉토리 구조

```
equip-control/
├── equip-control-fw/           # STM32 펌웨어 (C, FreeRTOS)
├── equip-control-ec/           # EC 소프트웨어 (C++, CMake)
├── equip-control-host/         # Host 프로그램 (Python, curses)
└── docs/
    ├── hw/                     # 하드웨어 설계 및 운영 문서
    │   ├── EFS-001.md          # Equipment Functional Specification
    │   ├── HDS-001.md          # Hardware/Electrical Design Specification
    │   ├── IO-001.md           # I/O List & Wiring Diagram
    │   ├── AIM-001.md          # Alarm / Interlock Matrix
    │   ├── TRG-001.md          # Troubleshooting & Recovery Guide
    │   └── TPV-001.md          # Test Plan & Verification Report
    └── troubleshooting/        # 개발 중 트러블슈팅 기록
        ├── TROUBLESHOOTING_LED.md
        └── troubleshooting-fan.md
```

---

## 핵심 문서

| 문서 | 내용 |
|------|------|
| [EFS-001](docs/hw/EFS-001.md) | 장비 기능 명세 (상태 머신, 시나리오, Host 진단 패널 스펙) |
| [AIM-001](docs/hw/AIM-001.md) | 알람/인터락 매트릭스 (ALID별 발생 조건, 자동 대응, 복구 조건) |
| [TRG-001](docs/hw/TRG-001.md) | 현장 대응 절차서 (원인 진단 트리, 단계별 조치, 복구 검증) |
| [HDS-001](docs/hw/HDS-001.md) | HW 설계 명세 (MOSFET PWM 회로, 이중 인터락 구조) |
| [IO-001](docs/hw/IO-001.md) | I/O 목록 및 배선도 (전체 핀 매핑, 회로도) |
| [TPV-001](docs/hw/TPV-001.md) | 테스트 계획 및 검증 결과 기록 |
