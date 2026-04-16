"""
수신 SECS-II 메시지 처리
S1F14, S5F1, S6F11, S2F42 디코딩 → AppState 갱신
S5F1 알람 발생 시 센서 스냅샷 캡처 및 원인 후보 자동 산출
"""
import logging
import struct
from datetime import datetime
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ui.Console import AppState

logger = logging.getLogger(__name__)

# CEID 정의
CEID_EQUIPMENT_ONLINE   = 1
CEID_PROCESS_START      = 3
CEID_PROCESS_COMPLETE   = 4
CEID_SENSOR_DATA_REPORT = 8

# 알람 분류 (AIM-001 §3): SW-IL → INTERLOCK 전이
SW_IL_ALIDS = {2, 5}  # TEMP_CRITICAL, SENSOR_ERROR


class MessageHandler:
    def __init__(self, state: "AppState"):
        self._state = state

    def handle(self, msg: dict):
        stream   = msg["stream"]
        function = msg["function"]
        body     = msg["body"]

        if stream == 1 and function == 14:
            self._handle_s1f14(body)
        elif stream == 5 and function == 1:
            self._handle_s5f1(body)
        elif stream == 6 and function == 11:
            self._handle_s6f11(body)
        elif stream == 2 and function == 42:
            self._handle_s2f42(body)
        else:
            logger.debug("Unhandled S%dF%d", stream, function)

    # ── S1F14 Establish Communication Ack ────────────────────────
    def _handle_s1f14(self, body: bytes):
        self._state.online = True
        logger.info("S1F14: Communication established")

    # ── S5F1 Alarm Report ─────────────────────────────────────────
    def _handle_s5f1(self, body: bytes):
        """
        S5F1 바디: L[3] {ALCD(U1), ALID(U1), ALTX(A)}
        ALCD bit0=1: alarm set, bit0=0: alarm cleared
        """
        if len(body) < 6:
            return
        try:
            alcd   = body[2]
            alid   = body[5]
            is_set = bool(alcd & 0x01)

            if is_set:
                self._state.active_alarms.add(alid)
                self._add_event(f"ALARM SET   alid={alid}")
                # 상태 갱신
                if alid in SW_IL_ALIDS:
                    self._state.equip_state = "INTERLOCK"
                else:
                    if self._state.equip_state not in ("INTERLOCK",):
                        self._state.equip_state = "ALARM"
                # 스냅샷 + 진단 생성
                self._capture_snapshot(alid)
                self._state.diagnoses[alid] = _compute_diagnosis(
                    alid, self._state.current_mA, self._state.temperature
                )
            else:
                self._state.active_alarms.discard(alid)
                self._add_event(f"ALARM CLEAR alid={alid}")
                # 스냅샷/진단은 참조용으로 유지 (RESET 후 clear)
        except (IndexError, struct.error) as e:
            logger.error("S5F1 parse error: %s", e)

    # ── S6F11 Event Report ────────────────────────────────────────
    def _handle_s6f11(self, body: bytes):
        if len(body) < 6:
            return
        try:
            ceid_offset = 10
            ceid = struct.unpack(">I", body[ceid_offset:ceid_offset + 4])[0]
            self._add_event(f"CEID-{ceid}")

            if ceid == CEID_SENSOR_DATA_REPORT:
                self._parse_sensor_report(body)
            elif ceid == CEID_EQUIPMENT_ONLINE:
                self._state.equip_state = "IDLE"
                self._state.online = True
            elif ceid == CEID_PROCESS_START:
                self._state.equip_state = "HEATING"
            elif ceid == CEID_PROCESS_COMPLETE:
                self._state.equip_state = "IDLE"
        except (IndexError, struct.error) as e:
            logger.error("S6F11 parse error: %s", e)

    def _parse_sensor_report(self, body: bytes):
        """센서 F4 값 4개(temp, humi, curr, volt) 순서대로 스캔"""
        try:
            i = 0
            f4_values = []
            while i < len(body) - 5:
                if body[i] == 0x91 and body[i + 1] == 0x04:
                    val = struct.unpack(">f", body[i + 2:i + 6])[0]
                    f4_values.append(val)
                    i += 6
                else:
                    i += 1
            if len(f4_values) >= 4:
                self._state.temperature = f4_values[0]
                self._state.humidity    = f4_values[1]
                self._state.current_mA  = f4_values[2]
                self._state.voltage_V   = f4_values[3]
        except struct.error:
            pass

    # ── S2F42 Command Ack ─────────────────────────────────────────
    def _handle_s2f42(self, body: bytes):
        hcack  = body[4] if len(body) > 4 else 0xFF
        result = "ACK" if hcack == 0x00 else f"NACK(0x{hcack:02X})"
        self._add_event(f"S2F42 {result}")
        if hcack == 0x00 and self._state.equip_state in ("ALARM", "INTERLOCK"):
            # RESET ACK → 진단 초기화
            self._state.snapshots.clear()
            self._state.diagnoses.clear()
            self._state.equip_state = "IDLE"
        logger.info("S2F42: %s", result)

    # ── 내부 유틸 ──────────────────────────────────────────────────
    def _capture_snapshot(self, alid: int):
        from ui.Console import AlarmSnapshot
        ts = datetime.now().strftime("%H:%M:%S")
        self._state.snapshots[alid] = AlarmSnapshot(
            alid=alid,
            ts=ts,
            temp=self._state.temperature,
            current_mA=self._state.current_mA,
        )

    def _add_event(self, text: str):
        ts = datetime.now().strftime("%H:%M:%S")
        self._state.events.append(f"{ts} | {text}")
        if len(self._state.events) > 20:
            self._state.events.pop(0)


# ── 진단 엔진 (EFS-001 §8 원인 분류 로직) ──────────────────────────
def _compute_diagnosis(alid: int, current_mA: float, temp: float):
    """ALID와 INA219 전류값으로 원인 후보와 점검 체크리스트를 생성한다."""
    from ui.Console import Diagnosis

    if alid in (1, 2):  # TEMP_HIGH / TEMP_CRITICAL
        if current_mA >= 800:
            causes = [
                "PWM Duty 과다 설정 (히터 출력 과잉)",
                "냉각 팬 동작 불량 (IRF520/5V 회로 확인)",
                "챔버 도어 밀폐 불량",
                "장비 주변 환경 온도 상승",
            ]
            checklist = [
                "Host 화면 PWM Duty 설정값 확인",
                "냉각 팬 회전 소리/진동 확인",
                "챔버 도어 닫힘 상태 육안 확인",
                "장비 주변 환경 온도 확인",
            ]
        elif current_mA < 50:
            causes = [
                "ST-22 바이메탈 조기 동작 (챔버 ~70°C 도달 가능성)",
                "히터 배선 탈거 또는 커넥터 불량",
                "MOSFET(IRLZ44N) 또는 Gate 저항 회로 이상",
            ]
            checklist = [
                "챔버 표면 온도 접촉 측정 (ST-22 동작 여부)",
                "히터 배선 탈거 및 커넥터 체결 확인",
                "MOSFET Drain-Source 및 100Ω Gate 저항 확인",
            ]
        else:  # 50~800mA — 저전류 이상
            causes = [
                "히터 커넥터 접촉 불량 (부분 단선)",
                "브레드보드 배선 느슨함 또는 접촉 저항 증가",
            ]
            checklist = [
                "히터 커넥터 핀 체결 상태 확인",
                "브레드보드 24V 레일 배선 확인",
            ]

    elif alid == 4:  # HW_INTERLOCK — ST-22 동작 감지
        causes = [
            "ST-22 바이메탈 동작 (챔버 70°C 도달)",
            "히터 회로 단선 (HEATING 중 전류 소실)",
        ]
        checklist = [
            "챔버 표면 온도 확인 (70°C 도달 여부)",
            "히터 배선 탈거 및 커넥터 체결 확인",
            "ST-22 양단 도통 확인 (충분히 냉각 후)",
        ]

    elif alid == 5:  # SENSOR_ERROR
        causes = [
            "SHT31 I2C SDA/SCL 배선 단선 또는 접촉 불량",
            "I2C 풀업 저항(4.7kΩ) 미연결 또는 단선",
            "INA219 A0 핀 배선 불량 (주소 0x40 충돌 가능)",
        ]
        checklist = [
            "SHT31 SDA(PB7)/SCL(PB6) 배선 확인",
            "I2C 풀업 저항(4.7kΩ) 3.3V 연결 확인",
            "INA219 A0 핀 GND 체결 확인",
        ]

    elif alid == 6:  # COMM_ERROR
        causes = [
            "USB 케이블 탈거 또는 접촉 불량",
            "STM32 NUCLEO ST-LINK 전원 공급 불량",
            "EC 프로세스 비정상 종료",
        ]
        checklist = [
            "STM32 NUCLEO USB 케이블 체결 상태 확인",
            "ls /dev/tty.usbmodem* 포트 인식 확인",
            "EC 프로세스 재시작 (./build/ec /dev/tty.usbmodem*)",
        ]

    else:
        causes    = ["알 수 없는 알람 — 로그 확인 필요"]
        checklist = ["EC 로그 및 STM32 시리얼 출력 확인"]

    return Diagnosis(
        alid=alid,
        causes=causes,
        checklist=checklist,
        checked=[False] * len(checklist),
    )
