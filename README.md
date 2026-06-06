# 챔버 온도 제어 펌웨어

STM32F446RE에 FreeRTOS를 올려 소형 챔버 온도를 PI 제어하는 펌웨어입니다. 과온 시 히터 차단·복구 시퀀스를 수행하며, PC측 호스트 모니터(EC)와 UART로 연결해 실시간 모니터링과 명령 처리를 합니다.

## 기술 스택

| 구분 | 기술 |
|------|------|
| MCU | STM32F446RE (ARM Cortex-M4, 84MHz) |
| FW 언어 | C |
| RTOS | FreeRTOS 10.3.1 (CMSIS-RTOS V2) |
| 개발 도구 | STM32CubeMX, STM32CubeIDE |
| EC 언어 | Python 3 |
| 통신 | UART 115200bps (ST-LINK USB VCP) |

---

## 시스템 구성

```
┌──────────────────────────────────────────────────────┐
│  EC — 호스트 모니터 (Host Monitor, macOS Python)     │
│                                                      │
│  - curses 실시간 모니터 패널 (상태 / KPI / 알람)     │
│  - 복구 조건 실시간 모니터링 (온도 ≤ SP-2°C ✓/✗)   │
│  - 센서 데이터 CSV 자동 로깅                         │
└─────────────────────┬────────────────────────────────┘
                      │ UART 115200bps (ASCII text, \r\n)
                      │ ST-LINK USB VCP
┌─────────────────────▼────────────────────────────────┐
│  FW (STM32F446RE NUCLEO, C / FreeRTOS)               │
│                                                      │
│  - DHT22 온도 측정 (PB5, 2s 주기, 단선 1-Wire)       │
│  - PI 히터 제어 (TIM3_CH3, 1kHz PWM)                │
│  - 상태 머신: IDLE → HEATING → WARNING → ALARM       │
│  - 팬 릴레이 (PA0) / 부저 (PB10) 자동 제어           │
│  - RESET 복구 조건 검증 후 ACK / NACK 응답           │
└──────────────────────────────────────────────────────┘
```

---

## FW 소프트웨어 설계

### FreeRTOS 5태스크 구조

```
 ├─[AboveNormal]─ UartRxTask   : USART2 ISR 알림 수신 → 명령 파싱 → 상태 전이
 ├─[Normal]─────  SensorTask   : DHT22 읽기 (vTaskSuspendAll 타이밍 보호)
 ├─[AboveNormal]─ ControlTask  : PI 제어 · KPI 계산 · 알람 판정 (센서 세마포어 동기화)
 ├─[Normal]─────  UartTxTask   : TX Queue 소비 + 1s 주기 DATA 전송
 └─[BelowNormal]─ ActuatorTask : 50ms 주기 부저 · LED GPIO 패턴 구동
```

| RTOS 오브젝트 | 종류 | 역할 |
|--------------|------|------|
| `g_state_mutex` | osMutex | 공유 상태(g_state, g_temp, KPI) 보호 |
| `g_sensor_sem` | osSemaphore | SensorTask → ControlTask 측정 완료 신호 |
| `g_tx_queue` | osMessageQueue (8×128B) | TX 직렬화 — 이벤트/ACK/NACK 큐잉 |
| `g_uart_rx_task_h` | TaskHandle | ISR → UartRxTask 라인 완성 알림 |

### 상태 머신

```
IDLE ──[START]──► HEATING ──[과온 > SP+1°C, 5s]──► WARNING
                     ▲           │                      │
                     │           │ [온도 복귀]           │ [과온 > SP+2°C, 10s]
                     │           ◄──────────────────────┘
                     │                                  ▼
                     └──────────[RESET + 온도≤SP-2°C]── ALARM
```

### PI 제어 알고리즘

```
e(t) = SP − T_measured
I(t) = clamp(I(t-1) + e(t) × dt,  −100, +100)
u(t) = clamp(Kp × e(t) + Ki × I(t),  0, 1000)
```

- **Kp = 200, Ki = 2, Kd = 0** — DHT22 노이즈에 의한 미분 킥 방지로 D항 비활성
- **u(t) 범위 0~1000**: TIM3_CH3 Compare 레지스터에 직접 기입 (1kHz PWM)
- **ALARM 진입 시**: u=0 강제, 적분항 초기화

### DHT22 타이밍 보호

DHT22 단선 프로토콜은 µs 단위 타이밍이 필요합니다. 태스크 선점으로 인한 타이밍 깨짐을 `vTaskSuspendAll()` / `xTaskResumeAll()` 로 방지하면서, TIM14(HAL Timebase)와 USART2 IRQ는 정상 동작을 유지합니다.

---

## 알람 정의

| ALID | 알람명 | 발생 조건 | FW 자동 대응 | 복구 |
|------|--------|---------|------------|------|
| ALM-01 | TEMP_WARNING | 온도 > SP + 1°C, 5s 지속 | 팬 ON, 부저 간헐 | 온도 복귀 시 자동 해제 |
| ALM-02 | TEMP_ALARM | 온도 > SP + 2°C, 10s 지속 | 히터 OFF, 팬 ON, 부저 연속 | RESET (온도 ≤ SP-2°C 검증 후) |
| ALM-03 | SENSOR_FAIL | DHT22 3회 연속 실패 | 히터 OFF, ALARM 전이 | RESET |

---

## UART 통신 프로토콜

**FW → EC (주기/이벤트):**

```
DATA:28.3,30.0,HEATING,NONE,50.0,23.1,0,0.0,0.0,0.0,0,0.0   # 1s 주기
EVENT:WARN,ALM-01      # WARNING 발생
EVENT:ALARM,ALM-02     # ALARM 발생
EVENT:CLEAR,ALM-01     # 알람 해제
EVENT:SETTLED,67.8     # 안정화 확인 (SP ± 1°C, 10s 유지)
ACK:START              # 명령 수락
NACK:RESET,TEMP_HIGH   # 복구 조건 미충족 거부
```

**EC → FW (명령):**

```
SET:30.0   # 온도 설정점 변경 (20°C ~ 80°C)
START      # 가열 시작 (IDLE → HEATING)
STOP       # 가열 정지
RESET      # 알람 복구 시도 (조건 검증 후 ACK/NACK)
STATUS     # 즉시 DATA 응답 요청
```

---

## EC 모니터 패널 (ALARM 상태 예시)

```
────────────────────────────────────────────────────────
 챔버 온도 제어 펌웨어 — 호스트 모니터
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
| STM32 NUCLEO-F446RE | 메인 MCU (84MHz, Cortex-M4) | — |
| DHT22 | 챔버 내부 온도 측정 (±0.5°C) | 단선 (PB5/D4) |
| 카프톤 필름 히터 24V/~26W | 챔버 내부 가열 | — |
| N-ch MOSFET 모듈 | 히터 PWM 제어 (1kHz) | PB0/A3 (TIM3_CH3) |
| 릴레이 모듈 + 5V 팬 | 챔버 환기 냉각 | PA0/A0 |
| DC-DC 컨버터 (24V→5V) | 팬 전원 | — |
| 액티브 부저 (3.3V) | 알람 경보 | PB10/D6 |
| WANPTEK 파워 서플라이 | 24V / 1.5A 제한 | — |
| 락앤락 밀폐 용기 | 챔버 본체 (환기 구멍 포함) | — |

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
| `RESET` | 알람 복구 시도 (복구 조건 검증 후 ACK/NACK) |
| `STATUS` | 현재 상태 즉시 조회 |
| `QUIT` | EC 종료 |

---

## 디렉토리 구조

```
equip-control/
├── chamber-fw/                 # STM32 펌웨어 (C, FreeRTOS, HAL)
│   └── Core/Src/
│       ├── main.c              # 5태스크 구조, 상태 머신, PI 제어, UART
│       └── dht22.c             # DHT22 단선 프로토콜 드라이버 (DWT 타이밍)
├── chamber-ec/                 # EC 소프트웨어 (Python)
│   ├── ec.py                   # curses 모니터 패널 UI, UART 통신, CSV 로깅
│   └── requirements.txt
└── docs/
    ├── sw/
    │   ├── SRS-001.md          # 소프트웨어 요구사항 명세
    │   ├── SDD-001.md          # 소프트웨어 설계 (태스크 구조, RTOS 오브젝트)
    │   └── TPV-001.md          # 테스트 계획 및 검증 보고서
    └── hw/
        ├── EFS-001.md          # Equipment Functional Specification
        ├── HDS-001.md          # Hardware Design Specification
        ├── IO-001.md           # I/O List & Wiring Diagram
        ├── AIM-001.md          # Alarm / Interlock Matrix
        ├── TRG-001.md          # Fault Handling & Recovery Guide
        └── SCN-001.md          # 과온 보호 동작 시나리오
```

---

## 핵심 문서

### 소프트웨어 개발 문서

| 문서 | 내용 |
|------|------|
| [SRS-001](docs/sw/SRS-001.md) | 소프트웨어 요구사항 명세 — REQ-F/SW/NF 요구사항 정의 |
| [SDD-001](docs/sw/SDD-001.md) | 소프트웨어 설계 — FreeRTOS 5태스크 구조도, RTOS 오브젝트 명세, PI 제어 알고리즘 |
| [TPV-001](docs/sw/TPV-001.md) | 테스트 계획 및 검증 — TC-01~05, UART 송수신 예시, 요구사항-설계-시험 추적표 |

### 시스템 / 하드웨어 문서

| 문서 | 내용 |
|------|------|
| [EFS-001](docs/hw/EFS-001.md) | 시스템 기능 명세 — 상태 머신, 알람 동작, UART 프로토콜 |
| [HDS-001](docs/hw/HDS-001.md) | HW 설계 명세 — DHT22, MOSFET PWM 회로, 전원 아키텍처 |
| [IO-001](docs/hw/IO-001.md) | I/O 목록 및 배선도 |
| [AIM-001](docs/hw/AIM-001.md) | 알람 매트릭스 — ALM-01~03 발생 조건, 자동 대응, 복구 조건 |
| [TRG-001](docs/hw/TRG-001.md) | 이상 처리·복구 가이드 — 이상 검출 동작, 복구 시퀀스, 벤치 브링업 점검 |
| [SCN-001](docs/hw/SCN-001.md) | 과온 보호 검증 시나리오 — 타임라인, 원인별 분석, 복구 시퀀스 검증 |
