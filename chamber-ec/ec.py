#!/usr/bin/env python3
"""
Equipment Controller (EC) — 소형 챔버 온도제어 및 알람진단 시스템
Usage: python3 ec.py <serial_port>
  예)  python3 ec.py /dev/tty.usbmodem*

DATA protocol examples:
  DATA:35.2,45.0,HEATING,NONE
  DATA:35.2,45.0,HEATING,NONE,82.5
  DATA:35.2,45.0,HEATING,NONE,825
  DATA:35.2,45.0,HEATING,NONE,82.5,127.3,1,127.3,45.8,0.8
"""

import sys
import time
import threading
import curses
import serial
import csv
from pathlib import Path

# ── 공유 상태 ────────────────────────────────────────────────────────────────
_lock = threading.Lock()
_state = {
    "temp":     None,
    "sp":       30.0,
    "duty":     None,       # percent, 0.0 ~ 100.0
    "elapsed":  None,       # seconds after START, optional
    "reached":  None,       # 0 or 1, optional
    "reach_s":  None,       # target reach time, optional
    "peak_temp": None,      # 최고온도, after target reached
    "overshoot": None,      # peak_temp - setpoint
    "fw_state": "UNKNOWN",
    "alarm":    "NONE",
    "events":   [],
    "sensor_logs": [],
    "last_data": "",
    "csv_path": "",
}

MAX_EVENTS = 20
MAX_SENSOR_LOGS = 8

ALM_INFO = {
    "ALM-01": ("TEMP_WARNING", "온도 > SP+3°C, 5초 지속",   "온도 복귀 시 자동 해제"),
    "ALM-02": ("TEMP_ALARM",   "온도 > SP+5°C, 10초 지속",  "온도 ≤ SP-2°C 복귀 후 RESET"),
    "ALM-03": ("SENSOR_ERROR", "DHT22 3회 연속 읽기 실패",   "센서 복구 후 RESET"),
}

# ── CSV 로그 ────────────────────────────────────────────────────────────────
_csv_file = None
_csv_writer = None

def _open_csv_log():
    global _csv_file, _csv_writer

    log_dir = Path("logs")
    log_dir.mkdir(exist_ok=True)

    path = log_dir / f"ec_sensor_{time.strftime('%Y%m%d_%H%M%S')}.csv"
    _csv_file = path.open("w", newline="", encoding="utf-8")
    _csv_writer = csv.writer(_csv_file)

    _csv_writer.writerow([
        "pc_time",
        "temp_c",
        "setpoint_c",
        "duty_pct",
        "fw_state",
        "alarm",
        "elapsed_s",
        "target_reached",
        "reach_s",
        "peak_temp_c",
        "overshoot_c",
        "raw_line",
    ])
    _csv_file.flush()

    _state["csv_path"] = str(path)

def _close_csv_log():
    global _csv_file
    if _csv_file:
        _csv_file.flush()
        _csv_file.close()
        _csv_file = None

def _write_sensor_csv(ts, temp, sp, duty, fw_state, alarm, elapsed, reached, reach_s, peak_temp, overshoot, raw_line):
    if _csv_writer is None:
        return

    _csv_writer.writerow([
        ts,
        "" if temp is None else f"{temp:.1f}",
        "" if sp is None else f"{sp:.1f}",
        "" if duty is None else f"{duty:.1f}",
        fw_state,
        alarm,
        "" if elapsed is None else f"{elapsed:.1f}",
        "" if reached is None else int(reached),
        "" if reach_s is None else f"{reach_s:.1f}",
        "" if peak_temp is None else f"{peak_temp:.1f}",
        "" if overshoot is None else f"{overshoot:.1f}",
        raw_line,
    ])
    _csv_file.flush()


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


def _parse_float(value):
    if value is None or value == "":
        return None
    return float(value)

def _normalize_duty(raw):
    """
    Firmware가 duty를 82.5(%)로 보내도 되고, 825/1000 같은 compare raw로 보내도 된다.
    100보다 큰 값은 0~1000 compare 값으로 보고 10으로 나눠 percent로 표시한다.
    """
    if raw is None:
        return None

    duty = float(raw)
    if duty > 100.0:
        duty = duty / 10.0

    if duty < 0.0:
        duty = 0.0
    if duty > 100.0:
        duty = 100.0

    return duty


def _parse(line: str):
    ts = time.strftime("%H:%M:%S")

    with _lock:
        if line.startswith("DATA:"):
            parts = line[5:].split(",")

            # expected:
            # 4 fields: temp,sp,state,alarm
            # 5 fields: temp,sp,state,alarm,duty
            # 8 fields:  temp,sp,state,alarm,duty,elapsed,reached,reach_s
            # 10 fields: temp,sp,state,alarm,duty,elapsed,reached,reach_s,peak_temp,overshoot
            if len(parts) >= 4:
                try:
                    temp     = _parse_float(parts[0])
                    sp       = _parse_float(parts[1])
                    fw_state = parts[2].strip() if parts[2] else "UNKNOWN"
                    alarm    = parts[3].strip() if parts[3] else "NONE"

                    duty = None
                    if len(parts) >= 5:
                        duty = _normalize_duty(parts[4].strip())

                    elapsed = None
                    if len(parts) >= 6:
                        elapsed = _parse_float(parts[5].strip())

                    reached = None
                    if len(parts) >= 7 and parts[6].strip() != "":
                        reached = int(float(parts[6].strip()))

                    reach_s = None
                    if len(parts) >= 8:
                        reach_s = _parse_float(parts[7].strip())

                    peak_temp = None
                    if len(parts) >= 9:
                        peak_temp = _parse_float(parts[8].strip())

                    overshoot = None
                    if len(parts) >= 10:
                        overshoot = _parse_float(parts[9].strip())

                    # Firmware가 peak/overshoot를 안 보내는 경우 EC에서 보조 계산한다.
                    # 정의: 목표온도 최초 도달 이후의 최고온도 - 목표온도
                    if peak_temp is None and temp is not None and sp is not None and reached:
                        prev_peak = _state.get("peak_temp")
                        peak_temp = temp if prev_peak is None else max(prev_peak, temp)

                    if overshoot is None and peak_temp is not None and sp is not None and reached:
                        overshoot = max(0.0, peak_temp - sp)

                    _state["temp"]     = temp
                    _state["sp"]       = sp
                    _state["fw_state"] = fw_state
                    _state["alarm"]    = alarm
                    _state["duty"]     = duty
                    _state["elapsed"]  = elapsed
                    _state["reached"]  = reached
                    _state["reach_s"]  = reach_s
                    _state["peak_temp"] = peak_temp
                    _state["overshoot"] = overshoot
                    _state["last_data"] = line

                    if temp is not None and sp is not None and duty is not None:
                        elapsed_txt = f" Elapsed={elapsed:.1f}s" if elapsed is not None else ""
                        reach_txt = f" Reach={reach_s:.1f}s" if reached and reach_s is not None else ""
                        overshoot_txt = f" OS={overshoot:.1f}C" if reached and overshoot is not None else ""
                        sensor_msg = (
                            f"[{ts}] T={temp:.1f}C SP={sp:.1f}C "
                            f"Duty={duty:.1f}% State={fw_state} Alarm={alarm}"
                            f"{elapsed_txt}{reach_txt}{overshoot_txt}"
                        )
                    else:
                        sensor_msg = f"[{ts}] {line}"

                    _state["sensor_logs"].append(sensor_msg)
                    if len(_state["sensor_logs"]) > MAX_SENSOR_LOGS:
                        _state["sensor_logs"].pop(0)

                    _write_sensor_csv(
                        ts, temp, sp, duty, fw_state, alarm,
                        elapsed, reached, reach_s, peak_temp, overshoot, line
                    )

                except (ValueError, IndexError) as e:
                    _log(f"[{ts}] DATA PARSE ERR: {line} ({e})")

        elif line.startswith("EVENT:") or line.startswith("ACK:") or line.startswith("NACK:"):
            # Run KPI reset points. START resets the overshoot run. STOP/RESET closes it.
            if line.startswith("ACK:START"):
                _state["peak_temp"] = None
                _state["overshoot"] = None
                _state["reached"] = 0
                _state["reach_s"] = None
                _state["elapsed"] = 0.0
            elif line.startswith("ACK:STOP") or line.startswith("ACK:RESET"):
                _state["elapsed"] = None
                _state["reached"] = None
                _state["reach_s"] = None
                _state["peak_temp"] = None
                _state["overshoot"] = None

            _log(f"[{ts}] {line}")

        elif line.startswith("BOOT:OK"):
            _state["fw_state"] = "IDLE"
            _state["alarm"]    = "NONE"
            _state["duty"]     = 0.0
            _state["elapsed"]  = None
            _state["reached"]  = None
            _state["reach_s"]  = None
            _state["peak_temp"] = None
            _state["overshoot"] = None
            _log(f"[{ts}] {line}")

        else:
            _log(f"[{ts}] RX: {line}")


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
def _safe_addstr(stdscr, y, x, text, attr=0):
    h, w = stdscr.getmaxyx()
    if y < 0 or y >= h or x < 0 or x >= w:
        return
    try:
        stdscr.addstr(y, x, str(text)[:max(0, w - x - 1)], attr)
    except curses.error:
        pass


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
            temp        = _state["temp"]
            sp          = _state["sp"]
            duty        = _state["duty"]
            elapsed     = _state["elapsed"]
            reached     = _state["reached"]
            reach_s     = _state["reach_s"]
            peak_temp   = _state["peak_temp"]
            overshoot   = _state["overshoot"]
            fw_state    = _state["fw_state"]
            alarm       = _state["alarm"]
            events      = list(_state["events"])
            sensor_logs = list(_state["sensor_logs"])
            csv_path    = _state["csv_path"]

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
        _safe_addstr(stdscr, row, 0, hdr.center(w), curses.color_pair(4) | curses.A_BOLD)
        row += 1
        _safe_addstr(stdscr, row, 0, "─" * (w - 1), curses.color_pair(4))
        row += 1

        # 상태 패널
        temp_str = f"{temp:.1f}°C" if temp is not None else "---"
        sp_str = f"{sp:.1f}°C" if sp is not None else "---"
        duty_str = f"{duty:.1f}%" if duty is not None else "---"
        elapsed_str = f"{elapsed:.1f}s" if elapsed is not None else "---"
        reach_str = f"{reach_s:.1f}s" if reach_s is not None and reached else "---"
        peak_str = f"{peak_temp:.1f}°C" if peak_temp is not None and reached else "---"
        overshoot_str = f"{overshoot:.1f}°C" if overshoot is not None and reached else "---"

        _safe_addstr(stdscr, row, 2, "[State]  FW: ")
        _safe_addstr(stdscr, row, 15, fw_state, sc | curses.A_BOLD)
        row += 1

        _safe_addstr(stdscr, row, 2, f"[Sensor] Temp: {temp_str}   SP: {sp_str}   Duty: {duty_str}", sc | curses.A_BOLD)
        row += 1

        _safe_addstr(stdscr, row, 2, f"[Run]    Elapsed: {elapsed_str}   Reach: {reach_str}")
        row += 1

        _safe_addstr(stdscr, row, 2, f"[KPI]    Peak: {peak_str}   Overshoot: {overshoot_str}")
        row += 1

        if csv_path:
            _safe_addstr(stdscr, row, 2, f"[CSV]    {csv_path}")
            row += 1

        # 알람 패널
        row += 1
        _safe_addstr(stdscr, row, 0, "─" * (w - 1), curses.color_pair(4))
        row += 1

        if alarm in ALM_INFO:
            alm_name, alm_cond, alm_recov = ALM_INFO[alarm]
            ac = curses.color_pair(3) if fw_state == "ALARM" else curses.color_pair(2)
            _safe_addstr(stdscr, row, 2, f"[Alarm]  {alarm} {alm_name}", ac | curses.A_BOLD)
            row += 1
            _safe_addstr(stdscr, row, 4, f"발생: {alm_cond}")
            row += 1
            _safe_addstr(stdscr, row, 4, f"복구: {alm_recov}")
            row += 1

            if fw_state == "ALARM" and alarm in ("ALM-02", "ALM-03"):
                reset_thr = sp - 2.0 if sp is not None else None
                row += 1
                if temp is not None and reset_thr is not None:
                    ok = temp <= reset_thr
                    mark = "✓" if ok else "✗"
                    cc = curses.color_pair(1) if ok else curses.color_pair(3)
                    _safe_addstr(stdscr, row, 4, f"[복구 조건] 온도 ≤ {reset_thr:.1f}°C : {mark}"
                                              f"  (현재 {temp:.1f}°C)", cc | curses.A_BOLD)
                    row += 1
                    hint = "  → RESET 입력 가능" if ok else "  → 냉각 대기 중..."
                    _safe_addstr(stdscr, row, 4, hint, cc)
                    row += 1
        else:
            _safe_addstr(stdscr, row, 2, "[Alarm]  NONE", curses.color_pair(5))
            row += 1

        # 센서 로그
        row += 1
        _safe_addstr(stdscr, row, 0, "─" * (w - 1), curses.color_pair(4))
        row += 1
        _safe_addstr(stdscr, row, 2, "[ Sensor Log ]", curses.color_pair(4))
        row += 1

        sensor_avail = min(MAX_SENSOR_LOGS, max(0, h - row - 12))
        for slog in sensor_logs[-sensor_avail:]:
            if row >= h - 8:
                break
            _safe_addstr(stdscr, row, 2, slog)
            row += 1

        # 이벤트 로그
        row += 1
        _safe_addstr(stdscr, row, 0, "─" * (w - 1), curses.color_pair(4))
        row += 1
        _safe_addstr(stdscr, row, 2, "[ Event Log ]", curses.color_pair(4))
        row += 1

        avail = h - row - 3
        for ev in events[-avail:]:
            if row >= h - 3:
                break
            _safe_addstr(stdscr, row, 2, ev)
            row += 1

        # 입력창
        if h >= 3:
            _safe_addstr(stdscr, h - 3, 0, "─" * (w - 1), curses.color_pair(4))
            _safe_addstr(stdscr, h - 2, 2, "명령: SET:<°C>  START  STOP  RESET  STATUS  QUIT")
            _safe_addstr(stdscr, h - 1, 2, f"> {ibuf}")

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

    with _lock:
        _open_csv_log()
        _log(f"[{time.strftime('%H:%M:%S')}] CSV LOG: {_state['csv_path']}")

    threading.Thread(target=_rx_thread, args=(ser,), daemon=True).start()

    try:
        curses.wrapper(_ui, ser)
    finally:
        _close_csv_log()
        ser.close()


if __name__ == "__main__":
    main()
