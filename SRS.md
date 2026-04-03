# 소프트웨어 요구사항 명세서 (SRS)
## 프로젝트명
SECS/GEM 기반 장비 상태 모니터링 시스템

## 문서 버전
v0.4 (Draft)

## 작성일
2026-04-03

## 작성자
[작성자명]

---

# 1. 개요

## 1.1 목적
본 문서는 "SECS/GEM 기반 장비 상태 모니터링 시스템"의 소프트웨어 요구사항을 정의한다.

본 시스템은 실제 반도체 Fab의 3계층 구조를 그대로 재현한다.

- **STM32 NUCLEO-F411RE**: 저수준 장비 제어 (센서 수집, 액추에이터 구동)
- **Lenovo IdeaPad 3 15ABA7 (Windows, EC 소프트웨어)**: Equipment Controller — 장비 상태 관리, SECS/GEM 통신
- **MacBook Pro 16인치 M4 Pro (macOS, Host 프로그램)**: Host / MES 시뮬레이터 — 장비 감시 및 원격 명령

EC 소프트웨어는 STM32로부터 원시 센서/상태 데이터를 수신하여 GEM 상태 머신을 관리하고, HSMS(TCP/IP) 위에 SECS-II 메시지로 Host와 통신한다.

본 프로젝트는 반도체 장비 제어 소프트웨어의 핵심 개념인 다음 요소를 구현 대상으로 한다.

- Equipment State 관리
- Control State 관리
- Alarm 발생 및 해제
- Event Report
- Data Collection
- Remote Command 처리

---

## 1.2 범위

### 포함 범위
- STM32 펌웨어: 센서 수집, 액추에이터 제어, EC와의 UART 통신
- EC 소프트웨어 (Windows): GEM 상태 머신, 알람/이벤트 관리, HSMS+SECS-II 통신 스택
- Host 프로그램 (macOS): 장비 상태 표시, 원격 명령 전송 CLI/GUI

### 제외 범위
- 실제 반도체 공정 제어
- SEMI 공식 인증 수준의 완전한 SECS-II Stream/Function 구현 (주요 S/F 부분 구현)
- 다중 장비 클러스터 제어
- 생산 이력 관리(MES/ERP 연동)
- 사용자 계정/권한 관리
- Recipe 관리

---

## 1.3 시스템 목표
1. 실제 Fab의 3계층 구조(MCU → EC → Host)를 실제 하드웨어와 소프트웨어로 재현한다.
2. HSMS(TCP/IP) 위에 SECS-II 메시지를 구현하여 EC↔Host 간 실무 수준 통신을 구현한다.
3. EC에서 GEM 상태 머신, 알람/이벤트 체계를 구현한다.
4. 장비 상태 감시 및 인터록(interlock) 기반 제어를 구현한다.
5. 포트폴리오 및 취업용 프로젝트로 설명 가능한 수준의 요구사항 기반 개발을 수행한다.

---

## 1.4 문서 규칙
- MUST: 반드시 구현되어야 하는 요구사항
- SHOULD: 구현 권장 요구사항
- MAY: 선택 구현 요구사항

---

# 2. 관련자 및 사용자

## 2.1 관련자
- 개발자: STM32 펌웨어, EC 소프트웨어, Host 프로그램 개발
- 평가자: 면접관, 기술 리뷰어, 교수/멘토
- 사용자: 장비 운영자(Host 프로그램 사용자)

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

본 시스템은 실제 Fab의 3계층 구조를 재현하며, 다음 세 구성요소로 이루어진다.

```
┌─────────────────────────────────────┐
│  1계층: 장비 하드웨어               │
│  STM32 NUCLEO-F411RE                │
│  - 센서: SHT31(온습도), INA219(전류)│
│  - 액추에이터: 팬, 부저, LED        │
│  - 로컬 입력: 버튼                  │
└──────────────┬──────────────────────┘
               │ UART / USB Virtual COM
               │ (JSON 내부 프로토콜)
┌──────────────▼──────────────────────┐
│  2계층: Equipment Controller (EC)   │
│  Lenovo IdeaPad 3 15ABA7 (Windows) │
│  - GEM 상태 머신 관리               │
│  - 알람 / 이벤트 처리               │
│  - SECS-II / HSMS 통신 스택         │
└──────────────┬──────────────────────┘
               │ HSMS (TCP/IP)
               │ SECS-II 메시지
┌──────────────▼──────────────────────┐
│  3계층: Host / MES 시뮬레이터       │
│  MacBook Pro 16" M4 Pro (macOS)    │
│  - 장비 상태 표시                   │
│  - 센서 데이터 / 알람 / 이벤트 표시 │
│  - 원격 명령 전송                   │
└─────────────────────────────────────┘
```

### 1) STM32 펌웨어 (장비 하드웨어 레이어)
- 하드웨어: STM32 NUCLEO-F411RE
- 역할:
  - 센서 데이터 수집 및 EC로 전송
  - EC 명령에 따라 액추에이터 제어
  - 로컬 버튼 입력 감지 및 EC에 보고
  - EC와 UART 통신 유지

### 2) EC 소프트웨어 (Equipment Controller 레이어)
- 실행 환경: Lenovo IdeaPad 3 15ABA7 (Windows)
- 역할:
  - STM32로부터 센서/이벤트 데이터 수신
  - GEM Equipment State 머신 관리
  - 알람 조건 판단 및 알람/이벤트 생성
  - STM32 액추에이터 제어 명령 하달
  - Host와 HSMS+SECS-II로 통신

### 3) Host 프로그램 (Host / MES 레이어)
- 실행 환경: MacBook Pro 16인치 M4 Pro, SSD 512GB, RAM 48GB (macOS)
- 역할:
  - HSMS 세션 관리
  - 장비 상태/센서/알람/이벤트 수신 및 표시
  - 원격 명령 전송

### 4) Peripheral Modules (STM32 연결)
- 온습도 센서(SHT31)
- 전류 센서(INA219)
- 액티브 부저
- 냉각 팬
- LED
- 버튼
- 가변저항(선택)

---

## 3.2 운영 시나리오
1. STM32에 전원이 인가되고 초기화가 완료된다.
2. EC 소프트웨어가 실행되어 STM32와 UART 연결을 수립한다.
3. EC는 HSMS Passive 모드로 Host의 연결을 대기한다.
4. Host가 EC에 HSMS 연결을 수립하고 Online 절차(S1F13/F14)를 수행한다.
5. EC의 Equipment State는 `IDLE`, Control State는 `LOCAL` 또는 `REMOTE`로 초기화된다.
6. Host가 `REMOTE` 상태에서 Remote Command(S2F41)로 `START`를 전송한다.
7. EC는 인터록 조건을 검사 후 STM32에 팬 구동 명령을 내리고 상태를 `RUN`으로 전이한다.
8. STM32는 주기적으로 센서 데이터를 EC에 전송하고, EC는 이를 S6F11 Event Report로 Host에 전달한다.
9. 임계값 초과 시 EC는 `ALARM` 상태로 전이하고 S5F1 Alarm Report를 Host에 전송한다.
10. Host는 S2F41로 `STOP`, `RESET`, `ACK_ALARM` 명령을 전송할 수 있다.

---

## 3.3 실무 구조와의 비교

| 항목 | 실제 Fab | 본 프로젝트 |
|------|----------|-------------|
| 저수준 제어 | PLC / 전용 MCU | STM32 NUCLEO-F411RE |
| EC 하드웨어 | 산업용 PC | Lenovo IdeaPad 3 15ABA7 (Windows) |
| EC↔장비 프로토콜 | EtherCAT / CANopen 등 필드버스 | UART + JSON (단순화) |
| EC↔Host 프로토콜 | HSMS (TCP/IP) + SECS-II | HSMS (TCP/IP) + SECS-II (동일) |
| Host | MES / EES 시스템 | MacBook Pro M4 Pro Host 프로그램 |
| Recipe 관리 | EC에서 Recipe 로드/실행 | 미구현 (향후 확장) |

EC↔Host 통신은 실무와 동일한 HSMS + SECS-II를 사용한다.  
STM32↔EC 통신은 필드버스 대신 UART + JSON으로 단순화한다.

---

# 4. 용어 정의

## 4.1 Equipment State
EC가 관리하는 장비의 동작 상태 집합

- INIT: 초기화 중
- IDLE: 대기 상태
- RUN: 동작 상태
- STOP: 정지 처리 중 (완료 시 IDLE로 전이)
- ALARM: 이상 상태 (복구 가능)
- ERROR: 복구 불가 심각 오류 (EC 재시작 필요)

## 4.2 Control State
장비 제어 주체를 나타내는 상태 집합

- LOCAL: STM32 로컬 버튼 입력에 의해 제어
- REMOTE: Host 명령(SECS-II)에 의해 제어

## 4.3 Alarm
장비 이상 조건 발생 시 EC가 생성하는 경고 신호. S5F1 메시지로 Host에 전달된다.

## 4.4 Event
상태 전이, 명령 수신, 알람 발생/해제 등 중요 동작 기록. S6F11 메시지로 Host에 전달된다.

## 4.5 Interlock
안전 또는 보호를 위해 특정 조건에서 장비 동작을 제한하는 로직 (EC에서 판단).

## 4.6 HSMS
High-Speed Message Services (SEMI E37). TCP/IP 기반 SECS 전송 계층 프로토콜.

## 4.7 SECS-II
SEMI Equipment Communications Standard Part 2 (SEMI E5). Stream/Function 구조의 메시지 규격.

---

# 5. 전제조건 및 제약사항

## 5.1 전제조건
- STM32 보드와 EC(Lenovo IdeaPad)가 USB로 연결되어 있어야 한다.
- EC(Windows)와 Host(macOS)가 동일 네트워크에 연결되어 있어야 한다.
- 센서 및 액추에이터가 지정 핀에 올바르게 배선되어 있어야 한다.

## 5.2 제약사항
- STM32 펌웨어는 STM32 NUCLEO-F411RE에서 동작해야 한다.
- EC 소프트웨어는 Lenovo IdeaPad 3 15ABA7 (Windows) 환경에서 동작해야 한다.
- Host 프로그램은 MacBook Pro 16인치 M4 Pro (macOS) 환경에서 동작해야 한다.
- EC↔Host 통신은 HSMS (TCP/IP) + SECS-II를 사용한다.
- STM32↔EC 통신은 UART / USB Virtual COM + JSON을 사용한다.
- 본 프로젝트는 SEMI 공식 인증 수준이 아닌 학습/포트폴리오 목적의 부분 구현이다.

---

# 6. 기능 요구사항

기능 요구사항은 구현 레이어별로 구분한다.

---

## 6.1 [STM32 펌웨어] 센서 수집 요구사항

### FR-001 온습도 수집
SHT31 센서로부터 온도 및 습도 데이터를 주기적으로 수집해야 한다.

### FR-002 전류/전압 수집
INA219 센서로부터 부하 전류 및 전압 데이터를 주기적으로 수집해야 한다.

### FR-003 수집 주기
센서 데이터 수집 주기는 설정값으로 관리되어야 하며 기본값은 1초로 한다.

### FR-004 데이터 유효성 검사
센서 데이터가 허용 범위를 벗어나거나 읽기 실패 시 오류 상태를 EC에 보고해야 한다.

---

## 6.2 [STM32 펌웨어] 액추에이터 제어 요구사항

### FR-005 팬 제어
EC의 명령에 따라 팬을 구동 또는 정지해야 한다.

### FR-006 부저 제어
EC의 명령에 따라 부저를 활성화 또는 비활성화해야 한다.

### FR-007 LED 상태 표시
EC로부터 수신한 Equipment State에 따라 LED를 구분하여 표시해야 한다.
- IDLE / RUN / ALARM / ERROR 상태 구분

### FR-008 로컬 버튼 처리
버튼 입력 감지 시 즉시 EC에 버튼 이벤트를 전송해야 한다.

---

## 6.3 [STM32 펌웨어] EC 통신 요구사항

### FR-009 센서 데이터 전송
수집된 센서 데이터를 JSON 포맷으로 EC에 주기적으로 전송해야 한다.

### FR-010 버튼 이벤트 전송
버튼 입력 발생 시 JSON 이벤트 메시지를 EC에 즉시 전송해야 한다.

### FR-011 명령 수신 및 실행
EC로부터 수신한 액추에이터 제어 명령을 파싱하여 실행하고 ACK를 반환해야 한다.

### FR-012 통신 유지
EC와의 UART 통신이 중단된 경우 재연결을 시도하고 오류를 로컬 LED로 표시해야 한다.

---

## 6.4 [EC 소프트웨어] 상태 관리 요구사항

### FR-013 Equipment State 초기화
EC 시작 시 Equipment State를 `INIT`으로 설정해야 한다.

### FR-014 초기화 완료 후 IDLE 전이
STM32 연결 및 초기 센서 확인이 완료되면 Equipment State를 `IDLE`로 전이해야 한다.

### FR-015 RUN 전이
`START` 명령이 유효하고 인터록 조건이 만족되면 Equipment State를 `RUN`으로 전이하고 STM32에 팬 구동 명령을 내려야 한다.

### FR-016 STOP 전이
`STOP` 명령 수신 시 Equipment State를 `STOP`으로 전이하고, 처리 완료 후 `IDLE`로 전이해야 한다.

### FR-017 ALARM 전이
정의된 이상 조건 발생 시 현재 Equipment State에 관계없이 `ALARM`으로 전이해야 한다.

### FR-018 ERROR 전이
복구 불가능한 치명적 오류 발생 시 `ERROR`로 전이해야 하며, `RESET` 명령으로 복구되지 않아야 한다.

### FR-019 RESET 처리
`RESET` 명령 수신 후 이상 조건이 해소되면 Equipment State를 `IDLE`로 복귀해야 한다.

---

## 6.5 [EC 소프트웨어] Control State 요구사항

### FR-020 Control State 제공
EC는 `LOCAL` 및 `REMOTE` 두 가지 Control State를 관리해야 한다.

### FR-021 REMOTE 명령 제한
Control State가 `LOCAL`일 때 Host의 제어 명령(START/STOP/RESET)은 거부되어야 하며, 거부 이벤트를 기록해야 한다. 조회 명령(S1F3, S5F5, S6F19 등)은 제한하지 않는다.

### FR-022 LOCAL 입력 허용
Control State가 `LOCAL`일 때 STM32 버튼 이벤트로 장비를 제어할 수 있어야 한다.

### FR-023 REMOTE 입력 허용
Control State가 `REMOTE`일 때 Host의 S2F41 Remote Command로 장비를 제어할 수 있어야 한다.

### FR-024 ALARM/ERROR 중 Control State 전환 제한
Equipment State가 `ALARM` 또는 `ERROR`인 동안에는 Control State 전환 명령을 거부해야 한다.

---

## 6.6 [EC 소프트웨어] 알람 요구사항

### FR-025 고온 알람
STM32로부터 수신한 온도가 설정 임계값을 초과하면 고온 알람을 발생시켜야 한다.

### FR-026 과전류 알람
STM32로부터 수신한 전류가 설정 임계값을 초과하면 과전류 알람을 발생시켜야 한다.

### FR-027 센서 오류 알람
STM32로부터 센서 오류 보고가 연속 3회 이상 수신되면 센서 오류 알람을 발생시켜야 한다.

### FR-028 통신 오류 알람
STM32와의 UART 통신이 10초 이상 중단되면 통신 오류 알람을 발생시켜야 한다.

### FR-029 알람 보고
알람 발생 시 S5F1 Alarm Report 메시지를 Host에 전송해야 한다.

### FR-030 알람 해제
이상 조건 해소 후 `ACK_ALARM` 또는 `RESET` 명령 수신 시 알람을 해제하고 S5F1으로 해제 상태를 Host에 전송해야 한다. 이상 조건이 지속 중이면 해제할 수 없다.

### FR-031 알람 목록 제공
Host가 S5F5(Alarm List Request)를 요청하면 현재 활성 알람 목록을 S5F6으로 응답해야 한다.

---

## 6.7 [EC 소프트웨어] 이벤트 보고 요구사항

### FR-032 이벤트 생성
EC는 다음 상황에서 이벤트를 생성해야 한다.
- Equipment State 전이
- Control State 전환
- Remote Command 수신 (성공/거부 모두)
- 로컬 버튼 입력
- 알람 발생/해제
- 인터록 발생
- STM32 통신 연결/해제
- HSMS 세션 연결/해제

### FR-033 이벤트 보고
생성된 이벤트는 S6F11 Event Report Send 메시지로 Host에 즉시 전송되어야 한다.

### FR-034 이벤트 로그 저장
최근 100개의 이벤트를 EC 메모리에 순환 버퍼로 저장해야 한다.

---

## 6.8 [EC 소프트웨어] SECS-II / HSMS 요구사항

### FR-035 HSMS 서버 동작
EC는 HSMS Passive 모드로 동작하여 Host의 연결을 대기해야 한다.

### FR-036 HSMS 연결 관리
HSMS Select, Deselect, Separate, Linktest 절차를 지원해야 한다.

### FR-037 Online 절차
Host의 S1F13(Establish Communication Request)에 S1F14로 응답해야 한다.

### FR-038 상태 조회 응답
Host의 S1F3(Selected Equipment Status Request)에 S1F4로 현재 장비 상태를 응답해야 한다.

### FR-039 Remote Command 처리
Host의 S2F41(Host Command Send)을 수신하여 명령을 처리하고 S2F42로 응답해야 한다.  
지원 명령: START, STOP, RESET, ACK_ALARM, SET_REMOTE, SET_LOCAL

### FR-040 미정의 메시지 처리
정의되지 않은 S/F 수신 시 S9 시리즈 오류 메시지로 응답해야 한다.

---

## 6.9 [EC 소프트웨어] 인터록 요구사항

### FR-041 START 전 인터록
다음 조건 중 하나라도 참이면 START를 거부해야 하며, 거부 사유를 S2F42 응답에 포함해야 한다.
- 활성 알람 존재
- STM32 연결 미확립
- 센서 초기화 미완료
- 전류 센서 비정상
- 온도 임계 초과

### FR-042 RUN 중 인터록
RUN 중 치명적 조건 발생 시 즉시 ALARM 상태로 전이해야 한다.

### FR-043 안전 정지
ALARM 발생 시 STM32에 안전 정책에 따른 액추에이터 제어 명령을 전송해야 한다.

---

## 6.10 [EC 소프트웨어] 데이터 관리 요구사항

### FR-044 활성 알람 목록 유지
현재 활성화된 알람 목록을 EC 메모리에 유지해야 한다.

### FR-045 마지막 상태 보존
마지막 Equipment State, Control State, 최근 센서 값을 Host 조회 시 반환할 수 있어야 한다.

---

## 6.11 [Host 프로그램] 표시 요구사항

### FR-046 상태 화면
Host 프로그램은 현재 Equipment State, Control State를 표시해야 한다.

### FR-047 센서 화면
Host 프로그램은 최신 센서 데이터(온도, 습도, 전류, 전압)를 표시해야 한다.

### FR-048 알람 화면
Host 프로그램은 현재 활성 알람 목록을 표시해야 한다.

### FR-049 이벤트 로그 화면
Host 프로그램은 시간순 이벤트 로그를 표시해야 한다.

### FR-050 명령 인터페이스
Host 프로그램은 S2F41 Remote Command를 전송할 수 있는 CLI 또는 UI를 제공해야 한다.

---

# 7. 비기능 요구사항

## 7.1 성능 요구사항

### NFR-001 Remote Command 응답 시간
Host의 S2F41 전송 후 500ms 이내에 S2F42 응답이 반환되어야 한다.

### NFR-002 이벤트 보고 지연
EC에서 이벤트 발생 후 500ms 이내에 S6F11이 Host에 전송되어야 한다.

### NFR-003 센서 데이터 갱신 주기
센서 데이터는 기본 1초 주기로 Host에 보고되어야 한다.

---

## 7.2 신뢰성 요구사항

### NFR-004 연속 동작 안정성
정상 환경에서 최소 1시간 연속 동작 중 치명적 오류 없이 동작해야 한다.

### NFR-005 HSMS 오류 복구
HSMS 세션이 단절된 후 재연결 시 상태 조회가 정상 동작해야 한다.

### NFR-006 STM32 통신 오류 복구
STM32 UART 통신이 일시 중단 후 재연결 시 데이터 수신이 재개되어야 한다.

### NFR-007 센서 실패 처리
센서 읽기 실패 시 STM32 전체가 중단되지 않아야 하며, 오류 상태를 EC에 보고해야 한다.

---

## 7.3 사용성 요구사항

### NFR-008 직관적 상태 구분
LED(STM32), 로그(EC), Host 화면을 통해 장비 상태를 쉽게 구분할 수 있어야 한다.

### NFR-009 디버깅 용이성
각 계층(STM32, EC, Host)은 개발자용 디버그 로그를 제공해야 한다.

---

## 7.4 유지보수성 요구사항

### NFR-010 STM32 모듈 구조
STM32 펌웨어는 다음 모듈로 분리되어야 한다.
- sensor_driver (SHT31, INA219)
- actuator_driver (팬, 부저, LED)
- uart_comm (EC와의 통신)
- button_handler

### NFR-011 EC 소프트웨어 모듈 구조
EC 소프트웨어는 다음 모듈로 분리되어야 한다.
- state_manager
- alarm_manager
- event_manager
- interlock_manager
- device_comm (STM32 UART 통신)
- secs_hsms (HSMS 세션 관리)
- secs_message (SECS-II 메시지 파싱/생성)
- command_handler

### NFR-012 설정 분리
임계값, 주기, 포트 번호, 알람 조건 등의 설정값은 설정 파일 또는 설정 구조체로 분리해야 한다.

---

# 8. 외부 인터페이스 요구사항

## 8.1 STM32 하드웨어 인터페이스
- 온습도 센서(SHT31): I2C
- 전류 센서(INA219): I2C
- 팬 제어: GPIO/PWM + MOSFET 드라이버
- 부저: GPIO
- 버튼: GPIO 입력
- LED: GPIO 출력

## 8.2 STM32 ↔ EC 통신 인터페이스 (내부 프로토콜)
- 물리 계층: UART / USB Virtual COM
- 메시지 포맷: JSON (개행 구분)
- 방향: 양방향 (STM32→EC: 센서/이벤트, EC→STM32: 액추에이터 명령)

### STM32 → EC 메시지 예시

```json
{ "type": "sensor", "temp": 34.5, "humidity": 42.1, "current_mA": 128.0, "voltage_V": 5.02, "ts": 1710000001 }
```

```json
{ "type": "sensor_error", "sensor": "SHT31", "consecutive_fails": 3, "ts": 1710000010 }
```

```json
{ "type": "button", "action": "pressed", "ts": 1710000020 }
```

### EC → STM32 메시지 예시

```json
{ "type": "cmd", "target": "fan", "action": "on" }
```

```json
{ "type": "cmd", "target": "buzzer", "action": "on" }
```

```json
{ "type": "cmd", "target": "led", "state": "ALARM" }
```

## 8.3 EC ↔ Host 통신 인터페이스 (SECS/GEM)
- 전송 계층: HSMS (SEMI E37, TCP/IP)
- 메시지 계층: SECS-II (SEMI E5, Stream/Function)
- EC 역할: HSMS Passive (서버)
- Host 역할: HSMS Active (클라이언트)

### 구현 대상 SECS-II Stream/Function

| S/F | 방향 | 설명 |
|-----|------|------|
| S1F1 / S1F2 | Host→EC / EC→Host | Are You There / On Line Data |
| S1F13 / S1F14 | Host→EC / EC→Host | Establish Communication |
| S1F3 / S1F4 | Host→EC / EC→Host | Status Request / Data |
| S2F41 / S2F42 | Host→EC / EC→Host | Remote Command |
| S5F1 / S5F2 | EC→Host / Host→EC | Alarm Report / Acknowledge |
| S5F5 / S5F6 | Host→EC / EC→Host | Alarm List Request / Data |
| S6F11 / S6F12 | EC→Host / Host→EC | Event Report / Acknowledge |
| S9F* | EC→Host | 오류 응답 |

---

# 9. 상태 전이 규칙

## 9.1 Equipment State 전이 규칙

| 현재 상태 | 다음 상태 | 조건 |
|-----------|-----------|------|
| INIT | IDLE | STM32 연결 및 센서 초기화 완료 시 |
| INIT | ERROR | 초기화 중 복구 불가 오류 발생 시 |
| IDLE | RUN | START 명령 수신 및 인터록 조건 만족 시 |
| IDLE | ALARM | 이상 조건 발생 시 (센서 오류, 통신 오류 등) |
| RUN | STOP | STOP 명령 수신 시 |
| RUN | ALARM | 고온/과전류/센서 오류/STM32 통신 오류 발생 시 |
| STOP | IDLE | 정지 처리 완료 시 |
| STOP | ALARM | 정지 처리 중 이상 조건 발생 시 |
| ALARM | IDLE | RESET 또는 ACK_ALARM 수신 후 이상 조건 해소 시 |
| ALARM | ERROR | 복구 불가 조건으로 악화될 경우 |
| ERROR | (없음) | EC 재시작으로만 복구 가능 |

## 9.2 Control State 전이 규칙

| 현재 상태 | 다음 상태 | 조건 |
|-----------|-----------|------|
| LOCAL | REMOTE | S2F41 SET_REMOTE 명령 성공 시 (Equipment State가 IDLE인 경우만 허용) |
| REMOTE | LOCAL | S2F41 SET_LOCAL 명령 성공 시 (Equipment State가 IDLE인 경우만 허용) |

> **주의**: Equipment State가 `ALARM` 또는 `ERROR`인 동안에는 Control State 전환을 거부한다 (FR-024 참조).

---

# 10. 수용 기준 (Acceptance Criteria)

### AC-001 기본 부팅
EC 시작 후 5초 이내에 INIT → IDLE 전이가 Host 이벤트 로그에서 확인되어야 한다.

### AC-002 HSMS 연결
Host 실행 후 HSMS 세션이 수립되고 S1F13/F14 Online 절차가 완료되어야 한다.

### AC-003 원격 시작
REMOTE 상태에서 Host가 S2F41 START 전송 후 500ms 이내에 S2F42 ACK 및 RUN 상태 전이 이벤트(S6F11)가 수신되어야 한다.

### AC-004 알람 발생
온도 또는 전류 임계값 초과 시 S5F1 Alarm Report가 Host에 전송되고, STM32 로컬 경보(부저/LED)가 동작해야 한다.

### AC-005 알람 해제
이상 조건 해소 후 ACK_ALARM 또는 RESET 수행 시 알람 해제 S5F1이 수신되어야 한다.

### AC-006 데이터 수집
센서 데이터가 1초 주기로 S6F11 Event Report를 통해 Host 로그에 표시되어야 한다.

### AC-007 인터록 검증
활성 알람 상태에서 START 명령(S2F41) 전송 시 거부 응답(S2F42, CMDA≠0)이 반환되어야 한다.

### AC-008 Control State 전환
REMOTE → LOCAL 전환 후 Host의 START 명령이 거부되어야 하고, STM32 버튼으로만 제어 가능해야 한다.

### AC-009 HSMS 재연결
Host가 HSMS 세션을 끊고 재연결 후 S1F3 상태 조회가 정상 응답되어야 한다.

### AC-010 IDLE 중 알람
IDLE 상태에서 센서 이상 발생 시 ALARM 전이 및 S5F1이 Host에 전송되어야 한다.

---

# 11. 향후 확장 항목

## 11.1 확장 가능 기능
- SECS-II Stream/Function 구현 범위 확대 (S7 Recipe, S12 등)
- Recipe 관리 기능 추가
- 설비별 파라미터 다운로드/업로드
- 다중 알람 등급(CRITICAL/WARN/INFO)
- 데이터 CSV 저장
- GUI 대시보드 고도화
- STM32↔EC 통신 고도화 (바이너리 프레임, CRC 검증)

---

# 12. 부록

## 12.1 요구사항 ID 체계
- FR: Functional Requirement
- NFR: Non-Functional Requirement
- AC: Acceptance Criteria

## 12.2 개발 우선순위

### 1단계: STM32 펌웨어 MVP
- 센서 수집 (SHT31, INA219)
- 액추에이터 제어 (팬, 부저, LED)
- EC와의 UART JSON 통신

### 2단계: EC 소프트웨어 MVP
- STM32 UART 수신 및 파싱
- 상태 머신 구현
- 알람/이벤트 관리
- HSMS 서버 + SECS-II 기본 S/F 구현

### 3단계: Host 프로그램
- HSMS 클라이언트 연결
- 상태/센서/알람/이벤트 표시
- Remote Command CLI

### 4단계: 통합 검증
- 3계층 End-to-End 시나리오 검증
- 수용 기준(AC) 전항목 확인
