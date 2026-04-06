"""
CLI 상태 표시 + 명령 입력 (SDD 6.3)
curses로 화면 자동 갱신(1초)과 입력줄을 분리
"""
import curses
import threading
import time
from dataclasses import dataclass, field
from typing import List, Set, Callable


@dataclass
class AppState:
    equip_state:   str       = "UNKNOWN"
    online:        bool      = False
    temperature:   float     = 0.0
    humidity:      float     = 0.0
    current_mA:    float     = 0.0
    voltage_V:     float     = 0.0
    active_alarms: Set[int]  = field(default_factory=set)
    events:        List[str] = field(default_factory=list)


class Console:
    REFRESH_INTERVAL = 1.0  # 초

    def __init__(self, state: AppState, on_command: Callable[[str], None]):
        self._state      = state
        self._on_command = on_command
        self._stop       = threading.Event()

    def start_display(self):
        pass  # run_input_loop 안에서 curses로 처리

    def run_input_loop(self):
        curses.wrapper(self._main)

    def _main(self, stdscr):
        curses.curs_set(1)
        curses.noecho()
        stdscr.nodelay(False)

        # 자동 갱신 스레드
        def refresh_loop():
            while not self._stop.is_set():
                self._draw(stdscr)
                time.sleep(1.0)

        t = threading.Thread(target=refresh_loop, daemon=True)
        t.start()

        # 입력 버퍼
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
            sep = "─" * min(52, cols - 1)

            lines = []
            lines.append("=" * min(52, cols - 1))
            lines.append("         EQUIP-CONTROL HOST")
            lines.append("=" * min(52, cols - 1))
            lines.append(f"[State]  Equipment: {s.equip_state:<8} | "
                         f"Online: {'YES' if s.online else 'NO'}")
            lines.append(f"[Sensor] Temp: {s.temperature:.1f}C  "
                         f"Humi: {s.humidity:.1f}%  "
                         f"Curr: {s.current_mA:.1f}mA  "
                         f"Volt: {s.voltage_V:.2f}V")
            alarm_ids = ", ".join(str(a) for a in sorted(s.active_alarms)) \
                        if s.active_alarms else "없음"
            lines.append(f"[Alarm]  Active: {len(s.active_alarms)}  ({alarm_ids})")
            lines.append(sep)
            lines.append("[Events] (최근 10건)")
            for ev in s.events[-10:]:
                lines.append(f"  {ev}")
            lines.append(sep)
            lines.append("Commands: START | STOP | RESET | ACK_ALARM <alid> | QUIT")
            lines.append(f"> {buf}")

            for i, line in enumerate(lines):
                if i >= rows - 1:
                    break
                stdscr.addstr(i, 0, line[:cols - 1])

            # 커서를 입력줄 끝으로
            input_row = len(lines) - 1
            if input_row < rows:
                stdscr.move(input_row, min(2 + len(buf), cols - 1))

            stdscr.refresh()
        except curses.error:
            pass
