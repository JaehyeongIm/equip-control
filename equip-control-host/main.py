"""
equip-control-host 진입점
EC(HsmsServer)에 연결 → 센서/알람/이벤트 표시 → 명령 전송
"""
import logging
import sys
import time

from hsms.HsmsClient import HsmsClient
from hsms.SecsCodec import build_s2f41
from handlers.MessageHandler import MessageHandler
from ui.Console import AppState, Console
import config

logging.basicConfig(
    level=logging.WARNING,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)


def main():
    state   = AppState()
    handler = MessageHandler(state)
    client  = HsmsClient(on_message=handler.handle)

    # ── 사용자 명령 → S2F41 전송 ──────────────────────────────
    def on_command(line: str):
        parts = line.upper().split()
        if not parts:
            return
        cmd = parts[0]

        if cmd in ("START", "STOP", "RESET"):
            if not client.is_selected():
                print("[Host] EC에 연결되어 있지 않습니다.")
                return
            msg = build_s2f41(
                session_id=0,
                sys_bytes=0,
                command=cmd,
            )
            client.send(msg)
            state.events.append(f"→ S2F41 {cmd}")

        elif cmd == "ACK_ALARM" and len(parts) > 1:
            try:
                alid = int(parts[1])
                state.active_alarms.discard(alid)
                state.events.append(f"→ ACK_ALARM alid={alid}")
            except ValueError:
                print("[Host] alid는 숫자여야 합니다.")
        else:
            print(f"[Host] 알 수 없는 명령: {line}")

    # ── CLI 시작 ───────────────────────────────────────────────
    console = Console(state, on_command)
    console.start_display()

    # ── EC 연결 (실패 시 재시도) ───────────────────────────────
    print(f"[Host] EC 연결 중 {config.EC_HOST}:{config.EC_PORT} ...")
    while True:
        if client.connect():
            state.events.append("EC 연결됨")
            break
        print(f"[Host] 연결 실패 — {config.HSMS_T5_SEC}초 후 재시도")
        time.sleep(config.HSMS_T5_SEC)

    # ── 입력 루프 (메인 스레드) ────────────────────────────────
    console.run_input_loop()
    client.disconnect()
    sys.exit(0)


if __name__ == "__main__":
    main()
