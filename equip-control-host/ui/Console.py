"""
CLI 상태 표시 + 명령 입력
curses로 화면 자동 갱신(1초)과 입력줄을 분리
진단 패널: S5F1 알람 발생 시 원인 후보, 점검 체크리스트, 복구 조건 표시
"""
import curses
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Set, Callable

# ── 알람 ID → 이름 매핑 (AIM-001) ────────────────────────────────
ALARM_NAMES = {
    1: "TEMP_HIGH",
    2: "TEMP_CRITICAL",
    4: "HW_INTERLOCK",
    5: "SENSOR_ERROR",
    6: "COMM_ERROR",
}

# 알람 심각도 우선순위 (높을수록 먼저 표시)
ALARM_PRIORITY = {2: 5, 5: 4, 4: 3, 1: 2, 6: 1}


@dataclass
class AlarmSnapshot:
    alid:       int
    ts:         str    # 발생 시각 HH:MM:SS
    temp:       float  # 발생 시 온도
    current_mA: float  # 발생 시 전류


@dataclass
class Diagnosis:
    alid:      int
    causes:    List[str]          # 원인 후보 (우선순위 순)
    checklist: List[str]          # 점검 항목
    checked:   List[bool]         # 점검 완료 여부


@dataclass
class AppState:
    equip_state:     str              = "UNKNOWN"
    online:          bool             = False
    temperature:     float            = 0.0
    humidity:        float            = 0.0
    current_mA:      float            = 0.0
    voltage_V:       float            = 0.0
    active_alarms:   Set[int]         = field(default_factory=set)
    events:          List[str]        = field(default_factory=list)
    # 진단 패널
    snapshots:       Dict[int, AlarmSnapshot] = field(default_factory=dict)
    diagnoses:       Dict[int, Diagnosis]     = field(default_factory=dict)

    def primary_alid(self) -> int:
        """활성 알람 중 최고 심각도 ALID 반환. 없으면 0."""
        if not self.active_alarms:
            return 0
        return max(self.active_alarms, key=lambda a: ALARM_PRIORITY.get(a, 0))

    def check_item(self, alid: int, idx: int):
        """점검 항목 완료 토글"""
        d = self.diagnoses.get(alid)
        if d and 0 <= idx < len(d.checked):
            d.checked[idx] = not d.checked[idx]


class Console:
    REFRESH_INTERVAL = 1.0

    def __init__(self, state: AppState, on_command: Callable[[str], None]):
        self._state      = state
        self._on_command = on_command
        self._stop       = threading.Event()

    def start_display(self):
        pass

    def run_input_loop(self):
        curses.wrapper(self._main)

    def _main(self, stdscr):
        curses.curs_set(1)
        curses.noecho()

        def refresh_loop():
            while not self._stop.is_set():
                self._draw(stdscr)
                time.sleep(1.0)

        t = threading.Thread(target=refresh_loop, daemon=True)
        t.start()

        buf = ""
        while not self._stop.is_set():
            self._draw(stdscr, buf)
            try:
                stdscr.timeout(100)
                ch = stdscr.getch()
            except curses.error:
                continue

            if ch == -1:
                continue
            elif ch in (curses.KEY_ENTER, ord('\n'), ord('\r')):
                line = buf.strip()
                buf = ""
                if line:
                    if line.upper() == "QUIT":
                        self._stop.set()
                        break
                    self._on_command(line)
            elif ch in (curses.KEY_BACKSPACE, 127, 8):
                buf = buf[:-1]
            elif 32 <= ch < 127:
                buf += chr(ch)

    def _draw(self, stdscr, buf: str = ""):
        s = self._state
        try:
            stdscr.erase()
            rows, cols = stdscr.getmaxyx()
            W = min(60, cols - 1)
            sep = "─" * W

            lines = []
            lines.append("=" * W)
            lines.append("          EQUIP-CONTROL HOST")
            lines.append("=" * W)
            lines.append(
                f"[State]  Equipment: {s.equip_state:<10} | "
                f"Online: {'YES' if s.online else 'NO'}"
            )
            lines.append(
                f"[Sensor] Temp: {s.temperature:.1f}C  "
                f"Curr: {s.current_mA:.0f}mA  "
                f"Volt: {s.voltage_V:.2f}V"
            )
            alarm_ids = ", ".join(str(a) for a in sorted(s.active_alarms)) \
                        if s.active_alarms else "없음"
            lines.append(f"[Alarm]  Active: {len(s.active_alarms)}  ({alarm_ids})")

            # ── 진단 패널 (활성 알람이 있을 때만) ─────────────────
            primary = s.primary_alid()
            if primary:
                lines.append(sep)
                snap = s.snapshots.get(primary)
                diag = s.diagnoses.get(primary)
                aname = ALARM_NAMES.get(primary, f"ALID={primary}")

                if snap:
                    lines.append(
                        f"[진단]   ALID={primary} {aname}"
                        f"  |  {snap.ts}"
                    )
                    lines.append(
                        f"         발생 시 Temp:{snap.temp:.1f}C"
                        f"  Curr:{snap.current_mA:.0f}mA"
                    )
                else:
                    lines.append(f"[진단]   ALID={primary} {aname}")

                if diag:
                    lines.append("[원인 후보]")
                    for i, cause in enumerate(diag.causes, 1):
                        lines.append(f"  {i}순위: {cause}")

                    lines.append(f"[점검 항목] (CHECK <번호>로 완료 표시)")
                    for i, (item, done) in enumerate(
                            zip(diag.checklist, diag.checked), 1):
                        mark = "X" if done else " "
                        lines.append(f"  [{mark}] {i}. {item}")

                # ── 복구 조건 ──────────────────────────────────────
                lines.append("[복구 조건]")
                temp_ok = s.temperature < 50.0
                lines.append(
                    f"  온도 < 50C    현재: {s.temperature:.1f}C"
                    f"  [{'O' if temp_ok else 'X'}]"
                )
                if primary in (2, 4):
                    curr_ok = 800.0 <= s.current_mA <= 1300.0
                    lines.append(
                        f"  INA219 전류 800~1300mA"
                        f"  현재: {s.current_mA:.0f}mA"
                        f"  [{'O' if curr_ok else 'X'}]"
                    )
                no_alarm = len(s.active_alarms) == 0
                lines.append(
                    f"  알람 해소     Active: {len(s.active_alarms)}건"
                    f"  [{'O' if no_alarm else 'X'}]"
                )

            lines.append(sep)
            lines.append("[Events] (최근 8건)")
            for ev in s.events[-8:]:
                lines.append(f"  {ev}")
            lines.append(sep)
            lines.append("Commands: START | STOP | RESET | CHECK <번호> | QUIT")
            lines.append(f"> {buf}")

            for i, line in enumerate(lines):
                if i >= rows - 1:
                    break
                stdscr.addstr(i, 0, line[:W])

            input_row = len(lines) - 1
            if input_row < rows:
                stdscr.move(input_row, min(2 + len(buf), W))

            stdscr.refresh()
        except curses.error:
            pass
