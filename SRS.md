# 소프트웨어 요구사항 명세서 (SRS)

## 프로젝트명
SECS/GEM 기반 장비 상태 모니터링 시스템

## 문서 버전
v1.3

## 준거 표준
IEEE 29148-2018 (Systems and Software Engineering — Requirements Engineering)

## 작성일
2026-04-03

## 작성자
임재형

## 관련 문서
- Interface Specification (IFS): STM32↔EC 및 EC↔Host 메시지 포맷 상세
- Software Design Document (SDD): 소프트웨어 모듈 구조 및 내부 설계

---

# 1. 개요

## 1.1 목적
본 문서는 "SECS/GEM 기반 장비 상태 모니터링 시스템"의 소프트웨어 요구사항을 정의한다.

본 시스템은 실제 반도체 Fab의 3계층 구조를 재현한다.

- **STM32 NUCLEO-F411RE**: 저수준 장비 제어 (센서 수집, 액추에이터 구동, FreeRTOS 기반 태스크 스케줄링)
- **Windows 기반 EC 소프트웨어**: Equipment Controller — GEM 상태 머신 관리, SECS/GEM 통신
- **macOS 기반 Host 프로그램**: Host / MES 시뮬레이터 — 장비 감시 및 원격 명령

EC 소프트웨어는 STM32로부터 원시 센서/이벤트 데이터를 수신하여 GEM 상태 머신을 관리하고, HSMS(TCP/IP) 위에 SECS-II 메시지로 Host와 통신한다.

본 프로젝트는 반도체 장비 제어 소프트웨어의 핵심 개념인 다음 요소를 구현 대상으로 한다.

- Equipment State 관리
- Control State 관리
- Alarm 발생 및 해제
- Event Report (Collection Event)
- Data Collection (Status Variable)
- Remote Command 처리

---

## 1.2 범위

### 포함 범위
- STM32 펌웨어: FreeRTOS 기반 태스크 구조, 센서 수집, 액추에이터 제어, EC와의 내부 프로토콜 통신
- STM32 무결성: Watchdog 기반 자동 복구, Graceful Degradation, 태스크 독립 오류 처리
- EC 소프트웨어 (Windows): GEM 상태 머신, 알람/이벤트 관리, HSMS+SECS-II 통신 스택
- Host 프로그램 (macOS): 장비 상태 표시, 원격 명령 전송

### 제외 범위
- 실제 반도체 공정 제어
- SEMI 공식 인증 수준의 SECS-II 전체 구현 (주요 S/F 부분 구현)
- 다중 장비 클러스터 제어
- 생산 이력 관리 (MES/ERP 연동)
- 사용자 계정/권한 관리
- Recipe 관리

---

## 1.3 시스템 목표
1. 실제 Fab의 3계층 구조(MCU → EC → Host)를 실제 하드웨어와 소프트웨어로 재현한다.
2. HSMS(TCP/IP) 위에 SECS-II 메시지를 구현하여 EC↔Host 간 표준 통신을 구현한다.
3. GEM 상태 머신, 알람/이벤트 체계를 EC에서 구현한다.
4. 장비 상태 감시 및 인터록 기반 제어를 구현한다.
5. FreeRTOS 기반 태스크 구조로 STM32 펌웨어의 실시간성과 모듈 독립성을 확보한다.
6. Watchdog 및 Graceful Degradation으로 단일 장애가 시스템 전체 정지로 이어지지 않도록 무결성을 보장한다.
7. 정량 수용 기준을 통해 검증 가능한 시스템을 설계한다.

---

## 1.4 문서 규칙

### 요구사항 키워드 (IEEE 29148-2018 준거)
- **shall**: 반드시 구현되어야 하는 요구사항 (검증 필수)
- **should**: 구현 권장 요구사항
- **may**: 선택 구현 요구사항

### 요구사항 품질 기준 (IEEE 29148-2018)
본 문서의 모든 요구사항은 다음 특성을 충족하도록 작성한다.
- **Necessary**: 시스템 목표 달성에 필요한 요구사항만 포함
- **Unambiguous**: 단 하나의 해석만 가능하도록 정량 기준 명시
- **Verifiable**: AC(수용 기준)를 통해 합격/불합격을 판정할 수 있음
- **Traceable**: FR↔NFR↔AC 추적 매트릭스(Section 14.3)로 연결 관계 명시

---

# 2. 관련자 및 사용자

## 2.1 관련자
- 개발자: STM32 펌웨어, EC 소프트웨어, Host 프로그램 개발
- 평가자: 면접관, 기술 리뷰어, 교수/멘토
- 사용자: 장비 운영자 (Host 프로그램 사용자)

## 2.2 사용자 유형

### 운영자(Operator)
- Host 프로그램에서 장비 상태를 조회한다.
- 장비 시작/정지/리셋 명령을 전송한다.
- 알람과 이벤트를 확인한다.

### 개발자(Developer)
- 장비 상태 전이 로직을 검증한다.
- 센서/액추에이터/통신 모듈을 디버깅한다.
- 각 계층의 로그를 분석한다.

---

# 3. 시스템 개요

## 3.1 시스템 구성

```
┌──────────────────────────────────────┐
│  1계층: 장비 하드웨어                │
│  STM32 NUCLEO-F411RE                 │
│  - 센서: SHT31(온습도), INA219(전류) │
│  - 액추에이터: 팬, 부저, LED         │
│  - 로컬 입력: 버튼                   │
└───────────────┬──────────────────────┘
                │ UART / USB Virtual COM
                │ (내부 프로토콜, IFS 참조)
┌───────────────▼──────────────────────┐
│  2계층: Equipment Controller (EC)    │
│  Windows 기반 PC                     │
│  - GEM 상태 머신 관리                │
│  - 알람 / 이벤트 처리                │
│  - SECS-II / HSMS 통신 스택          │
└───────────────┬──────────────────────┘
                │ HSMS (TCP/IP)
                │ SECS-II 메시지
┌───────────────▼──────────────────────┐
│  3계층: Host / MES 시뮬레이터        │
│  macOS 기반 PC                       │
│  - 장비 상태 표시                    │
│  - 센서 데이터 / 알람 / 이벤트 표시  │
│  - 원격 명령 전송                    │
└──────────────────────────────────────┘
```

### 상태 소유권 (State Ownership)
STM32는 로우레벨 장치 상태(raw device state)만 보유한다.  
**Equipment State 및 Control State의 authoritative owner는 EC이다.**  
STM32의 센서 데이터 및 버튼 이벤트는 EC 상태 판단의 입력으로만 사용된다.

---

## 3.2 운영 시나리오
1. STM32에 전원이 인가되고 초기화가 완료된다.
2. EC 소프트웨어가 실행되어 STM32와 내부 프로토콜 연결을 수립한다.
3. EC는 HSMS Passive 모드로 Host의 연결을 대기한다.
4. Host가 EC에 HSMS 연결을 수립하고 Online 절차(S1F13/F14)를 수행한다.
5. EC의 Equipment State는 `IDLE`, Control State는 `LOCAL` 또는 `REMOTE`로 초기화된다.
6. Host가 `REMOTE` 상태에서 S2F41 Remote Command로 `START`를 전송한다.
7. EC는 인터록 조건을 검사 후 STM32에 팬 구동 명령을 내리고 상태를 `RUN`으로 전이한다.
8. STM32는 센서 데이터를 주기적으로 EC에 전송하고, EC는 S6F11 Event Report로 Host에 전달한다.
9. 임계값 초과 시 EC는 `ALARM` 상태로 전이하고 S5F1 Alarm Report를 Host에 전송한다.
10. Host는 S2F41로 `STOP`, `RESET`, `ACK_ALARM` 명령을 전송할 수 있다.

---

## 3.3 실무 구조와의 비교

| 항목 | 실제 Fab | 본 프로젝트 |
|------|----------|-------------|
| 저수준 제어 | PLC / 전용 MCU | STM32 NUCLEO-F411RE |
| EC 하드웨어 | 산업용 PC | Windows 기반 PC |
| EC↔장비 프로토콜 | EtherCAT / CANopen 등 필드버스 | UART 내부 프로토콜 (단순화) |
| EC↔Host 프로토콜 | HSMS (TCP/IP) + SECS-II | HSMS (TCP/IP) + SECS-II (동일) |
| Host | MES / EES 시스템 | macOS 기반 Host 프로그램 |
| Recipe 관리 | EC에서 Recipe 로드/실행 | 미구현 (향후 확장) |

---

# 4. 용어 정의

## 4.1 Equipment State
EC가 소유하는 장비의 동작 상태 집합

| 상태 | 설명 |
|------|------|
| INIT | 초기화 중 |
| IDLE | 대기 상태 |
| RUN | 동작 상태 |
| STOP | 정지 처리 중 (완료 시 IDLE로 전이) |
| ALARM | 이상 상태 (복구 가능) |
| ERROR | 복구 불가 심각 오류 (EC 재시작 필요) |

## 4.2 Control State
장비 제어 주체를 나타내는 상태 집합

| 상태 | 설명 |
|------|------|
| LOCAL | STM32 로컬 버튼 입력에 의해 제어 |
| REMOTE | Host 명령(S2F41)에 의해 제어 |

## 4.3 Alarm
장비 이상 조건 발생 시 EC가 생성하는 경고 신호. S5F1로 Host에 전달.

## 4.4 Collection Event (CE)
상태 전이, 명령 수신, 알람 등 시스템 중요 동작. S6F11로 Host에 전달.

## 4.5 Status Variable (SV)
Host가 S1F3으로 조회 가능한 장비 상태 데이터 항목.

## 4.6 Interlock
안전 또는 보호를 위해 특정 조건에서 장비 동작을 제한하는 로직 (EC에서 판단).

## 4.7 HSMS
High-Speed Message Services (SEMI E37). TCP/IP 기반 SECS 전송 계층 프로토콜.

## 4.8 SECS-II
SEMI Equipment Communications Standard Part 2 (SEMI E5). Stream/Function 구조 메시지 규격.

## 4.9 FreeRTOS
STM32에 탑재되는 오픈소스 실시간 운영체제(RTOS). 태스크 단위 스케줄링, 우선순위 관리, 태스크 간 동기화(Queue, Semaphore, Mutex)를 제공한다.

## 4.10 Watchdog
STM32 하드웨어 타이머 기반 자동 복구 메커니즘. 정의된 주기 내 리셋 신호가 없으면 MCU를 강제 재시작하여 펌웨어 hang 상태를 복구한다.

## 4.11 Graceful Degradation (단계적 성능 저하)
일부 서브시스템 장애 발생 시 시스템 전체를 정지하지 않고, 정상 동작 가능한 기능만으로 최소 운영 상태를 유지하는 설계 원칙.

## 4.12 Task Watchdog
FreeRTOS 각 태스크별 독립적인 생존 확인 메커니즘. 특정 태스크가 설정 시간 내 체크인하지 않으면 해당 태스크만 재시작하여 시스템 전체 영향을 최소화한다.

---

# 5. 전제조건 및 제약사항

## 5.1 전제조건
- STM32 보드와 EC PC가 USB로 연결되어 있어야 한다.
- EC PC와 Host PC가 동일 네트워크에 연결되어 있어야 한다.
- 센서 및 액추에이터가 지정 핀에 올바르게 배선되어 있어야 한다.

## 5.2 제약사항
- STM32 펌웨어는 STM32 NUCLEO-F411RE에서 동작해야 한다.
- EC 소프트웨어는 Windows 환경에서 동작해야 한다.
- Host 프로그램은 macOS 환경에서 동작해야 한다.
- EC↔Host 통신은 HSMS (TCP/IP) + SECS-II를 사용해야 한다.
- STM32↔EC 통신은 UART / USB Virtual COM을 사용해야 한다.
- 본 프로젝트는 SEMI 공식 인증 수준이 아닌 학습/포트폴리오 목적의 부분 구현이다.

---

# 6. 기능 요구사항

---

## 6.1 [STM32] 센서 수집

### FR-001 온습도 수집
STM32는 SHT31 센서로부터 FR-003에 정의된 수집 주기로 온도(단위: °C) 및 습도(단위: %) 데이터를 수집해야 한다.

### FR-002 전류/전압 수집
STM32는 INA219 센서로부터 FR-003에 정의된 수집 주기로 전류(단위: mA) 및 전압(단위: V) 데이터를 수집해야 한다.

### FR-003 수집 주기
센서 데이터 수집 주기는 설정값으로 관리되어야 하며 기본값은 1,000ms로 한다. 설정 가능 범위는 100ms 이상 10,000ms 이하로 한다.

### FR-004 데이터 유효성 검사
STM32는 수집된 센서 데이터가 아래 허용 범위를 벗어나거나 I2C 읽기 실패 시, 오류 유형 코드를 포함한 오류 메시지를 EC에 전송해야 한다.

| 센서 | 항목 | 허용 범위 |
|------|------|---------|
| SHT31 | 온도 | -40°C 이상 125°C 이하 |
| SHT31 | 습도 | 0% 이상 100% 이하 |
| INA219 | 전류 | 0mA 이상 3,200mA 이하 |
| INA219 | 전압 | 0V 이상 26V 이하 |

---

## 6.2 [STM32] 액추에이터 제어

### FR-005 팬 제어
EC의 명령에 따라 팬을 구동 또는 정지해야 한다.

### FR-006 부저 제어
EC의 명령에 따라 부저를 활성화 또는 비활성화해야 한다.

### FR-007 LED 상태 표시
EC로부터 수신한 Equipment State에 따라 IDLE / RUN / ALARM / ERROR를 구분하여 LED로 표시해야 한다.

### FR-008 로컬 버튼 처리
버튼 입력 감지 시 즉시 EC에 버튼 이벤트를 전송해야 한다.

---

## 6.3 [STM32] EC 통신 - 기능

### FR-009 센서 데이터 전송
수집된 센서 데이터를 EC에 주기적으로 전송해야 한다.

### FR-010 버튼 이벤트 전송
버튼 입력 발생 시 이벤트 메시지를 EC에 즉시 전송해야 한다.

### FR-011 명령 수신 및 실행
STM32는 EC로부터 수신한 액추에이터 제어 명령을 수신 후 50ms 이내에 실행하고, 100ms 이내에 ACK 또는 NACK를 반환해야 한다.

---

## 6.4 [STM32] EC 통신 - 프로토콜 신뢰성

### FR-012 메시지 무결성
EC와 STM32 간 모든 메시지는 오류 감지 코드(CRC 또는 체크섬)를 포함해야 한다. 수신 측은 오류 감지 코드 불일치 시 NACK를 반환해야 한다.

### FR-013 명령 응답 보장
STM32는 EC로부터 수신한 모든 명령에 대해 ACK 또는 NACK를 반환해야 한다.

### FR-014 타임아웃 처리
EC는 명령 전송 후 정의된 시간(기본값: 1초) 내 응답이 없으면 재전송해야 한다.

### FR-015 재전송 정책
EC는 최대 3회 재전송 후에도 응답이 없으면 통신 오류로 처리해야 한다.

### FR-016 시퀀스 번호
모든 메시지는 중복 수신 감지를 위한 시퀀스 번호를 포함해야 한다. 수신 측은 중복 메시지를 무시하고 최신 시퀀스 번호를 추적해야 한다.

### FR-017 Heartbeat
EC와 STM32는 정의된 주기(기본값: 5초)로 Heartbeat 메시지를 교환하여 연결 상태를 확인해야 한다.

---

## 6.5 [STM32] FreeRTOS 태스크 구조

### FR-018 태스크 분리
STM32 펌웨어는 FreeRTOS 기반으로 다음 태스크로 분리되어야 한다. 각 태스크는 독립적으로 동작하며 단일 태스크 장애가 다른 태스크에 영향을 주지 않아야 한다.

| 태스크명 | 역할 | 우선순위 |
|---------|------|---------|
| TaskSensor | 1초 주기 센서 수집 (SHT31, INA219) | 보통 |
| TaskComm | UART 수신/송신, 프레임 파싱, ACK/NACK 처리 | 높음 |
| TaskHeartbeat | 5초 주기 Heartbeat 전송 및 EC 응답 감시 | 보통 |
| TaskButton | 버튼 입력 감지 및 즉시 이벤트 전송 | 높음 |
| TaskActuator | 팬/부저/LED 제어 명령 수신 및 실행 | 보통 |
| TaskWatchdog | 각 태스크 생존 확인 및 시스템 Watchdog 리셋 | 최고 |

### FR-019 태스크 간 통신
태스크 간 데이터 전달은 FreeRTOS Queue를 사용해야 한다. 공유 자원 접근은 Mutex로 보호해야 한다.

### FR-020 태스크 우선순위 정책
Safety-Critical 처리(알람 감지, 인터록, 통신 오류)는 일반 데이터 수집 태스크보다 높은 우선순위를 가져야 한다.

---

## 6.6 [STM32] Watchdog 및 자동 복구

### FR-021 하드웨어 Watchdog
STM32 IWDG(Independent Watchdog)를 활성화해야 한다. TaskWatchdog이 정상 동작 중일 때만 Watchdog 타이머를 리셋해야 하며, 펌웨어 hang 발생 시 MCU가 자동으로 재시작되어야 한다.

### FR-022 Task Watchdog
TaskWatchdog은 각 태스크의 체크인 주기를 감시해야 한다. 태스크가 정의된 시간(기본값: 태스크 주기 × 3) 내 체크인하지 않으면 해당 태스크를 재시작해야 한다.

### FR-023 재시작 후 상태 복구
MCU 자동 재시작 후 Equipment State는 EC에 재연결 완료 시까지 INIT을 유지해야 하며, 재연결 후 EC로부터 마지막 상태를 수신하여 동기화해야 한다.

### FR-024 Watchdog 이벤트 보고
MCU 재시작 또는 태스크 재시작이 발생하면 재시작 원인 코드를 EC에 전송해야 한다. EC는 이를 CEID-13(SystemRecovery) 이벤트로 Host에 보고해야 한다.

---

## 6.7 [STM32] Graceful Degradation

### FR-025 센서 부분 장애 대응
SHT31 또는 INA219 중 하나가 장애 상태가 되더라도 나머지 센서 수집과 EC 통신은 계속 동작해야 한다. 장애 센서의 값은 전송하지 않고, 해당 센서의 오류 상태만 보고해야 한다.

### FR-026 통신 장애 시 로컬 안전 동작
EC와의 UART 통신이 중단된 경우 STM32는 마지막으로 수신한 액추에이터 상태를 유지하거나 사전 정의된 안전 상태(팬 OFF, 부저 ON, LED ALARM)로 전환해야 한다.

### FR-027 TaskComm 장애 시 최소 기능 유지
TaskComm이 재시작되는 동안 TaskSensor와 TaskActuator는 중단 없이 동작해야 한다.

---

## 6.8 [EC] 상태 관리

### FR-028 Equipment State 초기화
EC 시작 시 Equipment State를 `INIT`으로 설정해야 한다.

### FR-029 초기화 완료 후 IDLE 전이
STM32 연결 및 초기 센서 확인이 완료되면 Equipment State를 `IDLE`로 전이해야 한다.

### FR-030 RUN 전이
`START` 명령이 유효하고 인터록 조건이 만족되면 Equipment State를 `RUN`으로 전이하고 STM32에 팬 구동 명령을 내려야 한다.

### FR-031 STOP 전이
`STOP` 명령 수신 시 Equipment State를 `STOP`으로 전이하고, 처리 완료 후 `IDLE`로 전이해야 한다.

### FR-032 ALARM 전이
정의된 이상 조건 발생 시 현재 Equipment State에 관계없이 `ALARM`으로 전이해야 한다.

### FR-033 ERROR 전이
아래 조건 중 하나 이상 발생 시 Equipment State를 `ERROR`로 전이해야 하며, `RESET` 명령만으로는 복구되지 않아야 한다.

| 조건 | 설명 |
|------|------|
| STM32 Watchdog 재시작 3회 연속 | 동일 오류 반복으로 자동 복구 불가 판단 |
| SHT31 및 INA219 동시 읽기 실패 | 양 센서 모두 장애 — Graceful Degradation 불가 |
| EC 내부 메모리 할당 실패 | 상태 머신 운영 불가 수준의 자원 고갈 |

### FR-034 RESET 처리
`RESET` 명령 수신 후 이상 조건이 해소되면 Equipment State를 `IDLE`로 복귀해야 한다.

---

## 6.9 [EC] Control State 관리

### FR-035 Control State 제공
EC는 `LOCAL` 및 `REMOTE` 두 가지 Control State를 관리해야 한다.

### FR-036 REMOTE 명령 제한
Control State가 `LOCAL`일 때 Host의 제어 명령(START/STOP/RESET)은 거부되어야 하며, 거부 이벤트(CEID-5)를 생성해야 한다. S1F3, S5F5 등 조회 명령은 제한하지 않는다.

### FR-037 LOCAL 입력 허용
Control State가 `LOCAL`일 때 STM32 버튼 이벤트로 장비를 제어할 수 있어야 한다.

### FR-038 REMOTE 입력 허용
Control State가 `REMOTE`일 때 Host의 S2F41 Remote Command로 장비를 제어할 수 있어야 한다.

### FR-039 ALARM/ERROR 중 Control State 전환 제한
Equipment State가 `ALARM` 또는 `ERROR`인 동안에는 Control State 전환 명령을 거부해야 한다.

---

## 6.10 [EC] 알람 관리

### FR-040 고온 알람 (ALID-1)
STM32로부터 수신한 온도가 설정 임계값을 초과하면 ALID-1 알람을 발생시켜야 한다.

### FR-041 과전류 알람 (ALID-2)
STM32로부터 수신한 전류가 설정 임계값을 초과하면 ALID-2 알람을 발생시켜야 한다.

### FR-042 센서 오류 알람 (ALID-3)
STM32로부터 센서 오류 보고가 연속 3회 이상 수신되면 ALID-3 알람을 발생시켜야 한다.

### FR-043 UART 통신 오류 알람 (ALID-4)
STM32와의 UART 통신이 10초 이상 중단되면 ALID-4 알람을 발생시켜야 한다.

### FR-044 알람 보고
알람 발생 시 S5F1 Alarm Report 메시지를 Host에 전송해야 한다. 메시지는 ALID, 알람명, ALST(SET), Severity를 포함해야 한다.

### FR-045 알람 해제
이상 조건 해소 후 `ACK_ALARM` 또는 `RESET` 명령 수신 시 해당 알람을 해제하고 S5F1(ALST=CLEAR)을 Host에 전송해야 한다. 이상 조건이 지속 중이면 해제할 수 없다.

### FR-046 알람 목록 제공
Host의 S5F5 요청에 대해 현재 활성 알람 목록을 S5F6으로 응답해야 한다.

---

## 6.11 [EC] 이벤트 보고

### FR-047 이벤트 생성
EC는 Section 9 Collection Event List에 정의된 CEID에 해당하는 조건 발생 시 이벤트를 생성해야 한다.

### FR-048 이벤트 보고
생성된 이벤트는 S6F11 Event Report Send 메시지로 Host에 즉시 전송되어야 한다. 메시지는 CEID와 해당 Report의 SV 목록을 포함해야 한다.

### FR-049 이벤트 로그 저장
최근 100개의 이벤트를 EC 메모리에 순환 버퍼로 저장해야 한다.

---

## 6.12 [EC] SECS-II / HSMS

### FR-050 HSMS 서버 동작
EC는 HSMS Passive 모드로 동작하여 Host의 연결을 대기해야 한다.

### FR-051 HSMS 연결 관리
HSMS Select, Deselect, Separate, Linktest 절차를 지원해야 한다.

### FR-052 Online 절차
Host의 S1F13에 S1F14로 응답해야 한다.

### FR-053 상태 변수 조회 응답
Host의 S1F3에 S1F4로 요청된 SVID에 해당하는 현재 값을 응답해야 한다.

### FR-054 Remote Command 처리
Host의 S2F41을 수신하여 명령을 처리하고 S2F42로 응답해야 한다. 지원 명령 및 파라미터는 Section 9.4를 참조한다.

### FR-055 미정의 메시지 처리
정의되지 않은 S/F 수신 시 S9 시리즈 오류 메시지로 응답해야 한다.

---

## 6.13 [EC] 인터록

### FR-056 START 전 인터록
Section 10 인터록 매트릭스에 정의된 조건 중 하나라도 참이면 START를 거부해야 하며, S2F42 응답에 거부 사유 코드를 포함해야 한다.

### FR-057 RUN 중 인터록
RUN 중 치명적 조건 발생 시 즉시 ALARM 상태로 전이해야 한다.

### FR-058 안전 정지
ALARM 발생 시 STM32에 Section 10 안전 정책에 따른 액추에이터 제어 명령을 전송해야 한다.

---

## 6.14 [EC] 데이터 관리

### FR-059 활성 알람 목록 유지
현재 활성화된 알람 목록을 EC 메모리에 유지해야 한다.

### FR-060 마지막 상태 보존
마지막 Equipment State, Control State, 최근 센서 값을 Host 조회 시 반환할 수 있어야 한다.

---

## 6.15 [Host] 표시 및 명령

### FR-061 상태 화면
현재 Equipment State, Control State를 표시해야 한다.

### FR-062 센서 화면
최신 센서 데이터(온도, 습도, 전류, 전압)를 표시해야 한다.

### FR-063 알람 화면
현재 활성 알람 목록을 표시해야 한다.

### FR-064 이벤트 로그 화면
시간순 이벤트 로그(CEID, 발생 시각, SV 값)를 표시해야 한다.

### FR-065 명령 인터페이스
S2F41 Remote Command를 전송할 수 있는 CLI 또는 UI를 제공해야 한다.

---

# 7. 비기능 요구사항

## 7.1 성능 요구사항

### NFR-001 Remote Command 응답 시간
Host의 S2F41 전송 후 500ms 이내에 S2F42 응답이 반환되어야 한다.

### NFR-002 이벤트 보고 지연
EC에서 이벤트 발생 후 500ms 이내에 S6F11이 Host에 전송되어야 한다.

### NFR-003 센서 데이터 갱신 주기
센서 데이터는 기본 1초 주기로 Host에 보고되어야 한다. 실제 보고 주기와 설정 주기의 오차(jitter)는 ±100ms 이내여야 한다.

---

## 7.2 신뢰성 요구사항

### NFR-004 연속 동작 안정성
정상 환경에서 1시간 연속 동작 중 치명적 오류 없이 동작해야 하며, 이 기간 동안 이벤트 유실은 0건이어야 한다.

### NFR-005 HSMS 오류 복구
HSMS 세션 단절 후 Host가 재연결 시도 시 30초 이내에 세션이 재수립되어야 한다.

### NFR-006 STM32 통신 오류 복구
STM32 UART 통신이 일시 중단 후 재연결 시도 시 10초 이내에 데이터 수신이 재개되어야 한다.

### NFR-007 센서 실패 처리
센서 읽기 실패 시 STM32 전체가 중단되지 않아야 하며, 오류 상태를 EC에 보고해야 한다.

### NFR-008 Remote Command 일관성
START 명령을 동일 조건에서 100회 반복 전송 시, 인터록 조건 미충족 상태에서는 100회 모두 거부, 충족 상태에서는 100회 모두 수락되어야 한다.

### NFR-009 FreeRTOS 태스크 실시간성
TaskSensor의 센서 수집 주기 jitter는 ±50ms 이내여야 한다. TaskButton의 버튼 입력 감지부터 EC 전송까지의 지연은 100ms 이내여야 한다.

### NFR-010 Watchdog 복구 시간
하드웨어 Watchdog에 의한 MCU 재시작 후 STM32가 EC와 재연결을 완료하고 정상 데이터를 전송하기까지 15초 이내여야 한다.

### NFR-011 Graceful Degradation 가용성
센서 1개 장애 발생 시 나머지 센서 수집, EC 통신, 알람 보고 기능은 중단 없이 동작해야 한다. 단일 태스크 재시작이 다른 태스크 동작에 영향을 주지 않아야 한다.

---

## 7.3 보안 요구사항

### NFR-012 네트워크 접근 제한
HSMS 서버(EC)는 설정 파일에 명시된 허용 IP 주소에서 오는 연결만 수락해야 한다. 미허용 IP로부터의 연결 시도는 거부하고 로그에 기록해야 한다.

### NFR-013 명령 출처 검증
EC는 HSMS 세션이 수립되지 않은 상태에서 수신된 S2F41 Remote Command를 무시하고 S9F1 오류 메시지로 응답해야 한다.

---

## 7.4 사용성 요구사항

### NFR-014 직관적 상태 구분
LED(STM32), 로그(EC), Host 화면을 통해 Equipment State를 쉽게 구분할 수 있어야 한다.

### NFR-015 디버깅 용이성
각 계층(STM32, EC, Host)은 타임스탬프가 포함된 개발자용 디버그 로그를 제공해야 한다.

---

## 7.5 유지보수성 요구사항

### NFR-016 모듈 구조
STM32 펌웨어 및 EC 소프트웨어는 기능별로 분리된 모듈 구조로 설계되어야 한다. 상세 모듈 구조는 SDD를 참조한다.

### NFR-017 설정 분리
임계값, 주기, 포트 번호, 알람 조건 등의 설정값은 소스 코드와 분리된 설정 파일 또는 설정 구조체로 관리해야 한다.

---

# 8. 외부 인터페이스 요구사항

## 8.1 STM32 하드웨어 인터페이스

| 모듈 | 인터페이스 |
|------|-----------|
| 온습도 센서 (SHT31) | I2C |
| 전류/전압 센서 (INA219) | I2C |
| 팬 제어 | GPIO/PWM + MOSFET 드라이버 |
| 부저 | GPIO |
| 버튼 | GPIO 입력 |
| LED | GPIO 출력 |

## 8.2 STM32 ↔ EC 내부 통신 인터페이스

- **물리 계층**: UART / USB Virtual COM
- **신뢰성 요구사항**: CRC/체크섬, ACK/NACK, 시퀀스 번호, 재전송 정책 (FR-012~017 참조)
- **메시지 포맷 상세**: Interface Specification(IFS) 참조

## 8.3 EC ↔ Host SECS/GEM 인터페이스

- **전송 계층**: HSMS (SEMI E37, TCP/IP)
- **메시지 계층**: SECS-II (SEMI E5)
- **EC 역할**: HSMS Passive (서버)
- **Host 역할**: HSMS Active (클라이언트)
- **메시지 포맷 상세**: Interface Specification(IFS) 참조

### 구현 대상 SECS-II Stream/Function

| S/F | 방향 | 설명 |
|-----|------|------|
| S1F1 / S1F2 | Host→EC / EC→Host | Are You There / On Line Data |
| S1F13 / S1F14 | Host→EC / EC→Host | Establish Communication |
| S1F3 / S1F4 | Host→EC / EC→Host | Status Variable Request / Data |
| S2F41 / S2F42 | Host→EC / EC→Host | Remote Command / Acknowledge |
| S5F1 / S5F2 | EC→Host / Host→EC | Alarm Report / Acknowledge |
| S5F5 / S5F6 | Host→EC / EC→Host | Alarm List Request / Data |
| S6F11 / S6F12 | EC→Host / Host→EC | Event Report / Acknowledge |
| S9F* | EC→Host | 오류 응답 |

---

# 9. GEM 데이터 모델

## 9.1 Status Variable List (SVID)

Host가 S1F3으로 조회 가능한 상태 변수 목록.

| SVID | 변수명 | 설명 | 데이터 타입 | 단위 |
|------|--------|------|-------------|------|
| 1 | EquipmentState | 장비 동작 상태 | ASCII | - |
| 2 | ControlState | 제어 주체 상태 | ASCII | - |
| 3 | Temperature | 측정 온도 | FLOAT | °C |
| 4 | Humidity | 측정 습도 | FLOAT | % |
| 5 | Current | 부하 전류 | FLOAT | mA |
| 6 | Voltage | 전원 전압 | FLOAT | V |
| 7 | ActiveAlarmCount | 현재 활성 알람 수 | U1 | - |
| 8 | FanState | 팬 동작 상태 | BOOLEAN | - |

## 9.2 Collection Event List (CEID)

S6F11 Event Report의 발생 조건 및 포함 SV 목록.

| CEID | 이벤트명 | 발생 조건 | 포함 SVID |
|------|---------|---------|-----------|
| 1 | EquipmentStateChanged | Equipment State 전이 | 1, 7 |
| 2 | ControlStateChanged | Control State 전환 | 2 |
| 3 | AlarmSet | 알람 발생 | 1, 7 |
| 4 | AlarmCleared | 알람 해제 | 1, 7 |
| 5 | CommandReceived | Remote Command 수신 (성공/거부 모두) | 1, 2 |
| 6 | InterlockTriggered | 인터록 발동으로 명령 거부 | 1 |
| 7 | LocalButtonPressed | STM32 로컬 버튼 입력 | 2 |
| 8 | SensorDataReport | 센서 데이터 주기 보고 | 3, 4, 5, 6 |
| 9 | HostCommEstablished | HSMS 세션 수립 | - |
| 10 | HostCommLost | HSMS 세션 해제 | - |
| 11 | DeviceConnected | STM32 UART 연결 수립 | - |
| 12 | DeviceDisconnected | STM32 UART 연결 해제 | 7 |
| 13 | SystemRecovery | MCU 또는 태스크 Watchdog 재시작 발생 | 1, 7 |

## 9.3 Alarm List (ALID)

S5F1 Alarm Report에 사용되는 알람 코드표.

| ALID | 알람명 | 발생 조건 | Severity | 해제 조건 | 운영자 조치 |
|------|--------|---------|----------|---------|------------|
| 1 | HIGH_TEMPERATURE | 온도 > 설정 임계값 | WARN | 온도 ≤ 임계값 − 히스테리시스 | ACK_ALARM 전송 후 냉각 조치 확인 |
| 2 | OVER_CURRENT | 전류 > 설정 임계값 | WARN | 전류 ≤ 임계값 | ACK_ALARM 전송 후 부하 상태 점검 |
| 3 | SENSOR_ERROR | 센서 읽기 연속 3회 실패 | WARN | 센서 정상 응답 1회 이상 | RESET 명령 후 센서 배선 점검 |
| 4 | UART_COMM_ERROR | STM32 Heartbeat 미수신 10초 이상 | WARN | UART 재연결 완료 | EC 재시작 또는 USB 재연결 |

## 9.4 Remote Command Parameter List

S2F41에서 사용 가능한 명령 및 파라미터 목록.

| RCMD | 파라미터명 | 타입 | 필수 여부 | 설명 |
|------|----------|------|---------|------|
| START | 없음 | - | - | 장비 동작 시작 |
| STOP | 없음 | - | - | 장비 정지 |
| RESET | 없음 | - | - | 오류 상태 복구 |
| ACK_ALARM | ALID | U2 | 필수 | 지정 알람 확인 (0 = 전체 해제) |
| SET_REMOTE | 없음 | - | - | Control State → REMOTE |
| SET_LOCAL | 없음 | - | - | Control State → LOCAL |

---

# 10. 인터록 매트릭스

## 10.1 START 전 인터록 조건

| 조건 | 거부 명령 | Equipment State 영향 | 복구 방법 |
|------|---------|---------------------|---------|
| 활성 알람 존재 | START | ALARM 유지 | ACK_ALARM 후 조건 해소 |
| STM32 연결 미확립 | START | INIT 유지 | USB 재연결 확인 |
| 센서 초기화 미완료 | START | INIT 유지 | 대기 또는 RESET |
| 온도 임계 초과 | START | ALARM 전이 | 냉각 후 ACK_ALARM |
| 전류 센서 비정상 | START | ALARM 전이 | 점검 후 RESET |

## 10.2 RUN 중 안전 정지 정책

| 조건 | EC 동작 | STM32 액추에이터 처리 |
|------|--------|----------------------|
| ALARM 발생 | ALARM 전이 + S5F1 전송 | 팬: 정지, 부저: 활성화, LED: ALARM |
| ERROR 발생 | ERROR 전이 + S5F1 전송 | 팬: 정지, 부저: 활성화, LED: ERROR |

---

# 11. 상태 전이 규칙

## 11.1 Equipment State 전이 규칙

| 현재 상태 | 다음 상태 | 조건 |
|-----------|-----------|------|
| INIT | IDLE | STM32 연결 및 센서 초기화 완료 시 |
| INIT | ERROR | 초기화 중 복구 불가 오류 발생 시 |
| IDLE | RUN | START 명령 수신 및 인터록 조건 만족 시 |
| IDLE | ALARM | 이상 조건 발생 시 (Section 9.3 참조) |
| RUN | STOP | STOP 명령 수신 시 |
| RUN | ALARM | 이상 조건 발생 시 (Section 9.3 참조) |
| STOP | IDLE | 정지 처리 완료 시 |
| STOP | ALARM | 정지 처리 중 이상 조건 발생 시 |
| ALARM | IDLE | RESET 또는 ACK_ALARM 수신 후 이상 조건 해소 시 |
| ALARM | ERROR | 복구 불가 조건으로 악화될 경우 |
| ERROR | (없음) | EC 재시작으로만 복구 가능 |

## 11.2 Control State 전이 규칙

| 현재 상태 | 다음 상태 | 조건 |
|-----------|-----------|------|
| LOCAL | REMOTE | S2F41 SET_REMOTE 수신 + Equipment State = IDLE |
| REMOTE | LOCAL | S2F41 SET_LOCAL 수신 + Equipment State = IDLE |

> **주의**: Equipment State가 `ALARM` 또는 `ERROR`인 동안 Control State 전환 명령은 거부된다 (FR-029).

---

# 12. 수용 기준 (Acceptance Criteria)

### AC-001 기본 부팅
EC 시작 후 5초 이내에 INIT → IDLE 전이 이벤트(CEID-1)가 Host 로그에서 확인되어야 한다.

### AC-002 HSMS 연결
Host 실행 후 10초 이내에 HSMS Select 절차가 완료되고, S1F13/F14 Online 절차가 성공 응답으로 완료되어야 한다.

### AC-003 Remote Command 응답 시간
REMOTE 상태에서 S2F41 START 전송 후 500ms 이내에 S2F42 ACK 및 CEID-5 이벤트가 수신되어야 한다. (100회 반복 시 100% 충족)

### AC-004 알람 발생
온도 또는 전류 임계값 초과 시 S5F1 Alarm Report(ALST=SET)가 Host에 수신되고, STM32 부저/LED 경보가 동작해야 한다.

### AC-005 알람 해제
이상 조건 해소 후 ACK_ALARM 또는 RESET 수행 시 S5F1(ALST=CLEAR)이 수신되어야 한다.

### AC-006 센서 데이터 주기 보고
1시간 연속 동작 중 CEID-8 이벤트 보고 주기가 1초 ± 100ms 이내여야 하며, 이벤트 유실은 0건이어야 한다.

### AC-007 인터록 검증 — 100회 일관성
활성 알람 상태에서 START 명령(S2F41) 100회 전송 시 100회 모두 거부 응답(S2F42, CMDA≠0)이 반환되어야 한다.

### AC-008 Control State 전환
SET_LOCAL 후 Host의 START 명령이 거부되어야 하고, STM32 버튼으로만 제어 가능해야 한다.

### AC-009 HSMS 재연결 복구 시간
Host가 HSMS 세션을 끊고 재연결 시도 시 30초 이내에 세션이 재수립되고 S1F3 상태 조회가 정상 응답되어야 한다.

### AC-010 UART 재연결 복구 시간
STM32 USB를 5초간 분리 후 재연결 시 10초 이내에 CEID-11(DeviceConnected) 이벤트가 Host에 전송되어야 한다.

### AC-011 IDLE 중 알람 발생
IDLE 상태에서 센서 이상 발생 시 ALARM 전이, S5F1(ALST=SET), CEID-1이 Host에 전송되어야 한다.

### AC-012 프로토콜 신뢰성
STM32↔EC 구간에서 메시지 오류(CRC 불일치)를 인위적으로 주입 시, NACK 반환 및 재전송이 수행되어야 한다.

### AC-013 FreeRTOS 태스크 독립성
TaskSensor를 강제 정지(suspend)한 상태에서 TaskComm, TaskButton, TaskActuator가 정상 동작해야 하며, EC는 센서 오류 알람(ALID-3)만 수신해야 한다.

### AC-014 Watchdog 자동 복구
펌웨어 무한루프를 인위적으로 주입하여 하드웨어 Watchdog을 트리거했을 때, 15초 이내에 STM32가 재시작하고 EC와 재연결하여 CEID-13(SystemRecovery) 이벤트가 Host에 수신되어야 한다.

### AC-015 Graceful Degradation — 센서 부분 장애
SHT31 센서 연결을 물리적으로 분리한 상태에서 INA219 전류 데이터 수집, EC 통신, ALID-3 알람 보고가 중단 없이 동작해야 한다.

### AC-016 태스크 실시간성
1시간 연속 동작 중 TaskSensor 수집 주기 jitter가 ±50ms 이내임을 로그로 확인할 수 있어야 한다.

---

# 13. 향후 확장 항목

- SECS-II Stream/Function 구현 범위 확대 (S7 Recipe, S12 등)
- Recipe 관리 기능 추가
- 설비별 파라미터 다운로드/업로드
- 다중 알람 등급 정교화 (CRITICAL / WARN / INFO)
- 데이터 CSV 저장
- GUI 대시보드 고도화

---

# 14. 부록

## 14.1 요구사항 ID 체계
- FR: Functional Requirement (FR-001 ~ FR-065)
- NFR: Non-Functional Requirement (NFR-001 ~ NFR-017)
- AC: Acceptance Criteria (AC-001 ~ AC-016)

---

## 14.3 요구사항 추적 매트릭스 (RTM)

IEEE 29148-2018 Section 6.2.4 준거. FR↔NFR↔AC 연결 관계를 명시한다.

| FR | 요구사항명 | 관련 NFR | 관련 AC |
|----|---------|---------|---------|
| FR-001~004 | 센서 수집 | NFR-003, NFR-009, NFR-011 | AC-006, AC-015 |
| FR-005~008 | 액추에이터 / 버튼 | NFR-009 | AC-004, AC-008, AC-015 |
| FR-009~011 | EC 통신 기능 | NFR-003, NFR-006, NFR-009 | AC-006, AC-010 |
| FR-012~017 | 프로토콜 신뢰성 | NFR-004, NFR-006 | AC-010, AC-012 |
| FR-018~020 | FreeRTOS 태스크 구조 | NFR-009, NFR-011 | AC-013, AC-016 |
| FR-021~024 | Watchdog / 자동 복구 | NFR-010, NFR-011 | AC-014 |
| FR-025~027 | Graceful Degradation | NFR-011 | AC-013, AC-015 |
| FR-028~034 | EC 상태 관리 | NFR-001, NFR-004, NFR-008 | AC-001, AC-003, AC-007 |
| FR-035~039 | Control State 관리 | NFR-008 | AC-008 |
| FR-040~046 | 알람 관리 | NFR-004 | AC-004, AC-005, AC-011 |
| FR-047~049 | 이벤트 보고 | NFR-002, NFR-004 | AC-006, AC-014 |
| FR-050~055 | SECS-II / HSMS | NFR-001, NFR-005, NFR-013 | AC-002, AC-003, AC-009 |
| FR-056~058 | 인터록 | NFR-008 | AC-007, AC-011 |
| FR-059~060 | 데이터 관리 | NFR-004 | AC-009 |
| FR-061~065 | Host 표시 / 명령 | NFR-014 | AC-001~011 (전체 표시) |

## 14.2 개발 우선순위

### 1단계: STM32 펌웨어 MVP
- 센서 수집 (FR-001~004)
- 액추에이터 제어 (FR-005~008)
- EC 통신 기능 + 신뢰성 (FR-009~017)

### 2단계: STM32 RTOS + 무결성
- FreeRTOS 태스크 구조 (FR-018~020)
- Watchdog + 자동 복구 (FR-021~024)
- Graceful Degradation (FR-025~027)

### 3단계: EC 소프트웨어 MVP
- 상태 머신 (FR-028~034)
- 알람/이벤트 관리 (FR-040~049)
- HSMS 서버 + SECS-II 기본 S/F (FR-050~055)

### 4단계: Host 프로그램
- HSMS 클라이언트 + 상태/센서/알람/이벤트 표시 (FR-061~065)

### 5단계: 통합 검증
- 수용 기준(AC-001~016) 전항목 정량 검증
