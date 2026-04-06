"""
CLI 상태 표시 + 명령 입력 (SDD 6.3)
백그라운드 스레드가 1초마다 화면 갱신
"""
import os
import sys
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
        self._lock       = threading.Lock()
        self._running    = False
        self._bg_thread  = None

    def _render(self):
        os.system("cls" if os.name == "nt" else "clear")
        s = self._state
        alarm_ids = ", ".join(str(a) for a in sorted(s.active_alarms)) \
                    if s.active_alarms else "없음"

        print("=" * 52)
        print("         EQUIP-CONTROL HOST")
        print("=" * 52)
        print(f"[State]  Equipment: {s.equip_state:<8} | "
              f"Online: {'YES' if s.online else 'NO'}")
        print(f"[Sensor] Temp: {s.temperature:.1f}C  "
              f"Humi: {s.humidity:.1f}%  "
              f"Curr: {s.current_mA:.1f}mA  "
              f"Volt: {s.voltage_V:.2f}V")
        print(f"[Alarm]  Active: {len(s.active_alarms)}  ({alarm_ids})")
        print("-" * 52)
        print("[Events] (최근 10건)")
        for ev in s.events[-10:]:
            print(f"  {ev}")
        print("-" * 52)
        print("Commands: START | STOP | RESET | ACK_ALARM <alid> | QUIT")
        print("> ", end="", flush=True)

    def _bg_refresh(self):
        while self._running:
            time.sleep(self.REFRESH_INTERVAL)
            if self._running:
                with self._lock:
                    self._render()

    def start_display(self):
        self._running = True
        self._bg_thread = threading.Thread(target=self._bg_refresh, daemon=True)
        self._bg_thread.start()

    def run_input_loop(self):
        self._render()
        while True:
            try:
                line = sys.stdin.readline()
                if not line:  # EOF
                    break
                line = line.strip()
            except KeyboardInterrupt:
                print("\n[Host] 종료합니다.")
                break

            if not line:
                with self._lock:
                    self._render()
                continue

            if line.upper() == "QUIT":
                break

            self._on_command(line)
            with self._lock:
                self._render()

        self._running = False
