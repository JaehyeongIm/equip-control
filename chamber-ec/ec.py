#!/usr/bin/env python3
"""
Equipment Controller (EC) — 소형 챔버 온도제어 및 알람진단 시스템
Usage: python3 ec.py <serial_port>
  예)  python3 ec.py /dev/tty.usbmodem*
"""

import sys
import time
import threading
import curses
import serial

# ── 공유 상태 ────────────────────────────────────────────────────────────────
_lock = threading.Lock()
_state = {
    "temp":     None,
    "sp":       30.0,
    "fw_state": "UNKNOWN",
    "alarm":    "NONE",
    "events":   [],
}

MAX_EVENTS = 20

ALM_INFO = {
    "ALM-01": ("TEMP_WARNING", "온도 > SP+3°C, 5초 지속",   "온도 복귀 시 자동 해제"),
    "ALM-02": ("TEMP_ALARM",   "온도 > SP+5°C, 10초 지속",  "온도 ≤ SP-2°C 복귀 후 RESET"),
    "ALM-03": ("SENSOR_ERROR", "DHT22 3회 연속 읽기 실패",   "센서 복구 후 RESET"),
}

# ── UART RX 스레드 ───────────────────────────────────────────────────────────
def _rx_thread(ser):
    buf = b""
    while True:
        try:
            chunk = ser.read(64)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", errors="ignore").strip()
                if line:
                    _parse(line)
        except Exception:
            break


def _parse(line: str):
    ts = time.strftime("%H:%M:%S")
    with _lock:
        if line.startswith("DATA:"):
            parts = line[5:].split(",")
            if len(parts) == 4:
                try:
                    _state["temp"]     = float(parts[0])
                    _state["sp"]       = float(parts[1])
                    _state["fw_state"] = parts[2]
                    _state["alarm"]    = parts[3]
                except ValueError:
                    pass
        elif line.startswith("EVENT:") or line.startswith("ACK:") or line.startswith("NACK:"):
            _log(f"[{ts}] {line}")
        elif line == "BOOT:OK":
            _state["fw_state"] = "IDLE"
            _state["alarm"]    = "NONE"
            _log(f"[{ts}] {line}")


def _log(msg: str):
    _state["events"].append(msg)
    if len(_state["events"]) > MAX_EVENTS:
        _state["events"].pop(0)


# ── 명령 송신 ────────────────────────────────────────────────────────────────
def _send(ser, cmd: str):
    ts = time.strftime("%H:%M:%S")
    try:
        ser.write((cmd + "\r\n").encode("ascii"))
        with _lock:
            _log(f"[{ts}] >> {cmd}")
    except Exception as e:
        with _lock:
            _log(f"[{ts}] TX ERR: {e}")


# ── curses UI ────────────────────────────────────────────────────────────────
def _ui(stdscr, ser):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(400)

    curses.start_color()
    curses.init_pair(1, curses.COLOR_GREEN,  curses.COLOR_BLACK)  # HEATING
    curses.init_pair(2, curses.COLOR_YELLOW, curses.COLOR_BLACK)  # WARNING
    curses.init_pair(3, curses.COLOR_RED,    curses.COLOR_BLACK)  # ALARM
    curses.init_pair(4, curses.COLOR_CYAN,   curses.COLOR_BLACK)  # 헤더
    curses.init_pair(5, curses.COLOR_WHITE,  curses.COLOR_BLACK)  # 기본

    ibuf = ""

    while True:
        stdscr.erase()
        h, w = stdscr.getmaxyx()

        with _lock:
            temp     = _state["temp"]
            sp       = _state["sp"]
            fw_state = _state["fw_state"]
            alarm    = _state["alarm"]
            events   = list(_state["events"])

        # 상태별 색상
        if fw_state == "ALARM":
            sc = curses.color_pair(3)
        elif fw_state == "WARNING":
            sc = curses.color_pair(2)
        elif fw_state == "HEATING":
            sc = curses.color_pair(1)
        else:
            sc = curses.color_pair(5)

        row = 0
        hdr = " 소형 챔버 온도제어 및 알람진단 시스템 "
        stdscr.addstr(row, 0, hdr.center(w), curses.color_pair(4) | curses.A_BOLD)
        row += 1
        stdscr.addstr(row, 0, "─" * (w - 1), curses.color_pair(4))
        row += 1

        # 상태 패널
        temp_str = f"{temp:.1f}°C" if temp is not None else "---"
        stdscr.addstr(row, 2, "[State]  FW: ")
        stdscr.addstr(fw_state, sc | curses.A_BOLD)
        row += 1
        stdscr.addstr(row, 2, "[Sensor] Temp: ")
        stdscr.addstr(temp_str, sc | curses.A_BOLD)
        stdscr.addstr(f"   SP: {sp:.1f}°C")
        row += 1

        # 알람 패널
        row += 1
        stdscr.addstr(row, 0, "─" * (w - 1), curses.color_pair(4))
        row += 1

        if alarm in ALM_INFO:
            alm_name, alm_cond, alm_recov = ALM_INFO[alarm]
            ac = curses.color_pair(3) if fw_state == "ALARM" else curses.color_pair(2)
            stdscr.addstr(row, 2, f"[Alarm]  {alarm} {alm_name}", ac | curses.A_BOLD)
            row += 1
            stdscr.addstr(row, 4, f"발생: {alm_cond}")
            row += 1
            stdscr.addstr(row, 4, f"복구: {alm_recov}")
            row += 1

            if fw_state == "ALARM" and alarm in ("ALM-02", "ALM-03"):
                reset_thr = sp - 2.0
                row += 1
                if temp is not None:
                    ok = temp <= reset_thr
                    mark = "✓" if ok else "✗"
                    cc = curses.color_pair(1) if ok else curses.color_pair(3)
                    stdscr.addstr(row, 4, f"[복구 조건] 온도 ≤ {reset_thr:.1f}°C : {mark}"
                                         f"  (현재 {temp:.1f}°C)", cc | curses.A_BOLD)
                    row += 1
                    hint = "  → RESET 입력 가능" if ok else "  → 냉각 대기 중..."
                    stdscr.addstr(row, 4, hint, cc)
                    row += 1
        else:
            stdscr.addstr(row, 2, "[Alarm]  NONE", curses.color_pair(5))
            row += 1

        # 이벤트 로그
        row += 1
        stdscr.addstr(row, 0, "─" * (w - 1), curses.color_pair(4))
        row += 1
        stdscr.addstr(row, 2, "[ Event Log ]", curses.color_pair(4))
        row += 1

        avail = h - row - 3
        for ev in events[-avail:]:
            if row >= h - 3:
                break
            stdscr.addstr(row, 2, ev[:w - 3])
            row += 1

        # 입력창
        if h >= 3:
            stdscr.addstr(h - 3, 0, "─" * (w - 1), curses.color_pair(4))
            stdscr.addstr(h - 2, 2, "명령: SET:<°C>  START  STOP  RESET  STATUS  QUIT")
            stdscr.addstr(h - 1, 2, f"> {ibuf}"[:w - 3])

        stdscr.refresh()

        # 키 처리
        try:
            key = stdscr.get_wch()
        except Exception:
            key = -1

        if key == -1:
            continue

        if isinstance(key, str):
            if key == "\n":
                cmd = ibuf.strip().upper()
                ibuf = ""
                if cmd == "QUIT":
                    return
                if cmd:
                    _send(ser, cmd)
            elif key in ("\x7f", "\x08"):
                ibuf = ibuf[:-1]
            else:
                ibuf += key
        elif isinstance(key, int) and key in (curses.KEY_BACKSPACE, 127):
            ibuf = ibuf[:-1]


# ── 진입점 ───────────────────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        print("Usage: python3 ec.py <serial_port>")
        print("  예)  python3 ec.py /dev/tty.usbmodem*")
        sys.exit(1)

    port = sys.argv[1]
    try:
        ser = serial.Serial(port, baudrate=115200, timeout=0.1)
    except serial.SerialException as e:
        print(f"포트 열기 실패: {e}")
        sys.exit(1)

    threading.Thread(target=_rx_thread, args=(ser,), daemon=True).start()

    try:
        curses.wrapper(_ui, ser)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
