# SECS/GEM 기반 장비 상태 모니터링 시스템

STM32 NUCLEO-F411RE 보드를 사용한 3-tier 구조의 장비 상태 모니터링 포트폴리오 프로젝트입니다.

```
STM32 FW (C)          EC (Windows, C++)        Host (macOS, Python)
────────────          ─────────────────        ────────────────────
FreeRTOS 6태스크  →   DeviceComm (UART)   →    HsmsClient (TCP)
센서/버튼/HB          StateMachine             Console CLI
UART 내부 프로토콜     AlarmManager
                      HsmsServer (HSMS/TCP)
```

---

## 디렉토리 구조

```
equip-control/
├── equip-control-fw/       # STM32 펌웨어 (STM32CubeIDE)
├── equip-control-ec/       # EC 소프트웨어 (C++, CMake)
├── equip-control-host/     # Host 프로그램 (Python)
├── SRS.md                  # 소프트웨어 요구사항 명세 (IEEE 29148-2018)
└── SDD.md                  # 소프트웨어 설계 문서 (IEEE 1016-2009)
```

---

## 1단계 — 펌웨어 빌드 및 플래시 (STM32CubeIDE, Windows)

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
| LD2 (초록) | EC에서 CMD_FAN 명령 수신 시 ON/OFF |

---

## 2단계 — EC 빌드 및 실행 (Windows)

### 사전 준비
- CMake 설치
- MinGW 또는 MSVC 컴파일러

### 빌드

```bash
cd equip-control-ec
cmake -B build -S .
cmake --build build
```

### 실행

```bash
# UART 포트 지정 (Windows: COM3 등, macOS: /dev/tty.usbmodem*)
./build/ec COM3
```

### EC 실행 시 동작
- STM32 UART 연결 후 센서 데이터 수신 시작
- TCP 포트 5000에서 Host 연결 대기
- 콘솔에 수신 로그 출력:
  ```
  [EC] UART connected: COM3
  [HSMS] Listening on port 5000
  [SENSOR] temp=25.0C humi=50.0% curr=100.0mA volt=3.3V
  [HB] seq=0
  ```

---

## 3단계 — Host 실행 (macOS)

### 사전 준비

```bash
# Python 3.8 이상
python3 --version
```

### 실행

```bash
cd equip-control-host

# EC IP/포트 확인 (기본값: 127.0.0.1:5000)
# EC가 다른 PC에 있다면 config.py에서 EC_HOST 수정
python3 main.py
```

### 화면 구성

```
==================================================
       EQUIP-CONTROL HOST
==================================================
[State]  Equipment: IDLE     | Online: YES
[Sensor] Temp: 25.0C  Humi: 50.0%  Curr: 100.0mA  Volt: 3.30V
[Alarm]  Active: 0  (알람 없음)
--------------------------------------------------
[Events] (최근 10건)
  10:01:23 | CEID-8
  10:01:18 | CEID-1
--------------------------------------------------
Commands: START | STOP | RESET | ACK_ALARM <alid> | QUIT
>
```

### 명령어

| 명령 | 설명 |
|------|------|
| `START` | 장비 IDLE → RUNNING 전환 |
| `STOP` | 장비 RUNNING → IDLE 전환 |
| `RESET` | 장비 ALARM/ERROR → IDLE 전환 |
| `ACK_ALARM <alid>` | 알람 해제 (alid: 1~4) |
| `QUIT` | 프로그램 종료 |

---

## 전체 실행 순서

```
1. STM32 보드를 EC PC(Windows)에 USB 연결
2. EC 실행: ./ec COM3
3. Host 실행: python3 main.py
4. Host 화면에서 Online: YES 확인
5. 명령 입력: START → STOP → RESET
```

---

## 알람 ID 목록

| ALID | 알람명 | 발생 조건 |
|------|--------|-----------|
| 1 | TEMP_HIGH | 온도 > 80°C |
| 2 | HUMIDITY_HIGH | 습도 > 95% |
| 3 | SENSOR_ERROR | SHT31 또는 INA219 오류 |
| 4 | UART_COMM_ERROR | Heartbeat 10초 이상 미수신 |

---

## 내부 프로토콜 프레임 구조 (STM32↔EC)

```
| SOF(1) | TYPE(1) | SEQ(1) | LEN_L(1) | LEN_H(1) | PAYLOAD(n) | CRC_L(1) | CRC_H(1) |
```

- SOF: 0xAA
- CRC: CRC16-CCITT (poly=0x1021, init=0xFFFF)

## 관련 문서

- [SRS.md](SRS.md) — 소프트웨어 요구사항 명세 (IEEE 29148-2018, v1.3)
- [SDD.md](SDD.md) — 소프트웨어 설계 문서 (IEEE 1016-2009, v1.0)
