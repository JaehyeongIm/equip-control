"""
CLI 상태 표시 + 명령 입력 (SDD 6.3)
입력 직전에 화면을 렌더링하여 타이핑 중 화면 갱신 충돌 방지
"""
import os
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
    def __init__(self, state: AppState, on_command: Callable[[str], None]):
        self._state      = state
        self._on_command = on_command

    def _render(self):
        os.system("clear")
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

    def start_display(self):
        pass  # 백그라운드 렌더링 불필요 — 입력 직전에 렌더링

    def run_input_loop(self):
        """매 입력마다 화면 갱신 후 프롬프트 표시"""
        while True:
            self._render()
            try:
                line = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                print("\n[Host] 종료합니다.")
                break

            if not line:
                continue

            if line.upper() == "QUIT":
                break

            self._on_command(line)
