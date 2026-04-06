"""
수신 SECS-II 메시지 처리
S1F14, S5F1, S6F11, S2F42 디코딩 → AppState 갱신
"""
import logging
import struct
from datetime import datetime
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ui.Console import AppState

logger = logging.getLogger(__name__)

# CEID 정의 (SDD 4.3 EventManager)
CEID_EQUIPMENT_ONLINE     = 1
CEID_CONTROL_STATE_CHANGE = 2
CEID_PROCESS_START        = 3
CEID_PROCESS_COMPLETE     = 4
CEID_COMMAND_RECEIVED     = 5
CEID_ALARM_SET            = 6
CEID_ALARM_CLEARED        = 7
CEID_SENSOR_DATA_REPORT   = 8


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
        S5F1 바디: L[3] {ALCD, ALID, ALTX}
        ALCD bit0=1: alarm set, bit0=0: alarm cleared
        """
        if len(body) < 4:
            return
        try:
            alcd = body[2]
            alid = body[5]
            is_set = bool(alcd & 0x01)
            if is_set:
                self._state.active_alarms.add(alid)
                self._add_event(f"ALARM SET   alid={alid}")
            else:
                self._state.active_alarms.discard(alid)
                self._add_event(f"ALARM CLEAR alid={alid}")
        except (IndexError, struct.error) as e:
            logger.error("S5F1 parse error: %s", e)

    # ── S6F11 Event Report ────────────────────────────────────────
    def _handle_s6f11(self, body: bytes):
        """
        S6F11 바디: L[3] {DATAID, CEID, RPT[]}
        CEID-8 SensorDataReport → 센서값 파싱
        """
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
            elif ceid == CEID_PROCESS_START:
                self._state.equip_state = "RUNNING"
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
        hcack  = body[2] if len(body) > 2 else 0xFF
        result = "ACK" if hcack == 0x00 else f"NACK(0x{hcack:02X})"
        self._add_event(f"S2F42 {result}")
        logger.info("S2F42: %s", result)

    def _add_event(self, text: str):
        ts = datetime.now().strftime("%H:%M:%S")
        self._state.events.append(f"{ts} | {text}")
        if len(self._state.events) > 10:
            self._state.events.pop(0)
