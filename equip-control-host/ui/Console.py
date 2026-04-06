"""
CLI 상태 표시 + 명령 입력 (SDD 6.3)

=== EQUIP-CONTROL HOST ===
[State]  Equipment: IDLE  | Online: YES
[Sensor] Temp: 25.0C  Humi: 50.0%  Curr: 100.0mA  Volt: 3.3V
[Alarm]  Active: 0
[Events]
  10:01:23 | CEID-8
  10:01:18 | CEID-1
> Command:
"""
import os
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
    COMMANDS = ["START", "STOP", "RESET", "ACK_ALARM", "QUIT"]

    def __init__(self, state: AppState, on_command: Callable[[str], None]):
        self._state      = state
        self._on_command = on_command
        self._stop       = threading.Event()

    def start_display(self):
        """백그라운드 스레드에서 1초마다 화면 갱신"""
        t = threading.Thread(target=self._display_loop, daemon=True)
        t.start()

    def _display_loop(self):
        while not self._stop.is_set():
            self._render()
            time.sleep(1.0)

    def _render(self):
        os.system("clear")
        s = self._state
        alarm_count = len(s.active_alarms)
        alarm_ids   = ", ".join(str(a) for a in sorted(s.active_alarms)) \
                      if s.active_alarms else "None"

        print("=" * 50)
        print("       EQUIP-CONTROL HOST")
        print("=" * 50)
        print(f"[State]  Equipment: {s.equip_state:<8} | "
              f"Online: {'YES' if s.online else 'NO'}")
        print(f"[Sensor] Temp: {s.temperature:.1f}C  "
              f"Humi: {s.humidity:.1f}%  "
              f"Curr: {s.current_mA:.1f}mA  "
              f"Volt: {s.voltage_V:.2f}V")
        print(f"[Alarm]  Active: {alarm_count}  "
              f"({'알람 없음' if not s.active_alarms else alarm_ids})")
        print("-" * 50)
        print("[Events] (최근 10건)")
        for ev in s.events[-10:]:
            print(f"  {ev}")
        print("-" * 50)
        print("Commands: START | STOP | RESET | ACK_ALARM <alid> | QUIT")
        print("> ", end="", flush=True)

    def run_input_loop(self):
        """메인 스레드: 사용자 입력 → 콜백 전달"""
        while True:
            try:
                line = input().strip()
            except (EOFError, KeyboardInterrupt):
                print("\n[Host] 종료합니다.")
                self._stop.set()
                break
            if not line:
                continue
            cmd = line.upper()
            if cmd == "QUIT":
                self._stop.set()
                break
            self._on_command(line)
