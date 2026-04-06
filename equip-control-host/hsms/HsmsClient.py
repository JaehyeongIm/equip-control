"""
HSMS Active 클라이언트 (SEMI E37)
EC HsmsServer에 연결하여 세션 수립 후 메시지 송수신
"""
import socket
import struct
import threading
import time
import logging
from typing import Callable, Optional
from hsms.SecsCodec import (
    HSMS_SELECT_REQ, HSMS_SELECT_RSP,
    HSMS_LINKTEST_REQ, HSMS_LINKTEST_RSP,
    HSMS_DATA_MSG,
    build_control_msg, build_s1f13, parse_message,
)
import config

logger = logging.getLogger(__name__)


class HsmsClient:
    """
    상태: NOT_CONNECTED → CONNECTED → SELECTED
    """

    def __init__(self, on_message: Callable[[dict], None]):
        self._on_message = on_message
        self._sock: Optional[socket.socket] = None
        self._session_id = 0
        self._sys_bytes = 1
        self._selected = False
        self._running = False
        self._rx_thread: Optional[threading.Thread] = None
        self._lt_thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()

    # ── 연결 ──────────────────────────────────────────────────────
    def connect(self) -> bool:
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(config.HSMS_T5_SEC)
            self._sock.connect((config.EC_HOST, config.EC_PORT))
            self._sock.settimeout(None)
            logger.info("TCP connected to %s:%d", config.EC_HOST, config.EC_PORT)
        except OSError as e:
            logger.error("TCP connect failed: %s", e)
            return False

        self._running = True
        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._rx_thread.start()

        # SelectReq 전송
        sys_b = self._next_sys_bytes()
        self._send_raw(build_control_msg(HSMS_SELECT_REQ, sys_b))
        logger.info("SelectReq sent (sys=%d)", sys_b)

        # SelectRsp 대기 (T7 타임아웃)
        deadline = time.time() + config.HSMS_T7_SEC
        while time.time() < deadline:
            if self._selected:
                logger.info("Session SELECTED")
                # S1F13 Establish Communication
                self._send_raw(build_s1f13(self._session_id,
                                           self._next_sys_bytes()))
                # Linktest 타이머 시작
                self._lt_thread = threading.Thread(
                    target=self._linktest_loop, daemon=True)
                self._lt_thread.start()
                return True
            time.sleep(0.1)

        logger.error("SelectRsp timeout (T7)")
        self.disconnect()
        return False

    def disconnect(self):
        self._running = False
        self._selected = False
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        logger.info("Disconnected")

    def is_selected(self) -> bool:
        return self._selected

    # ── 메시지 전송 ───────────────────────────────────────────────
    def send(self, data: bytes):
        with self._lock:
            self._send_raw(data)

    def _send_raw(self, data: bytes):
        if self._sock:
            try:
                self._sock.sendall(data)
            except OSError as e:
                logger.error("Send error: %s", e)

    # ── RX 루프 ───────────────────────────────────────────────────
    def _rx_loop(self):
        buf = b""
        while self._running:
            try:
                chunk = self._sock.recv(4096)
                if not chunk:
                    logger.warning("Connection closed by EC")
                    self._running = False
                    break
                buf += chunk
            except OSError:
                break

            # 완전한 메시지 처리 (length 필드 기준)
            while len(buf) >= 4:
                length = struct.unpack(">I", buf[:4])[0]
                if len(buf) < 4 + length:
                    break
                msg_bytes = buf[:4 + length]
                buf = buf[4 + length:]
                self._handle_message(msg_bytes)

    def _handle_message(self, data: bytes):
        msg = parse_message(data)
        if not msg:
            return

        stype = msg["stype"]

        if stype == HSMS_SELECT_RSP:
            self._session_id = msg["session_id"]
            self._selected = True

        elif stype == HSMS_LINKTEST_RSP:
            logger.debug("LinktestRsp received")

        elif stype == HSMS_DATA_MSG:
            # SECS-II 데이터 → 상위 핸들러로 전달
            self._on_message(msg)

        else:
            logger.debug("Unhandled stype=0x%02X", stype)

    # ── Linktest 주기 전송 ────────────────────────────────────────
    def _linktest_loop(self):
        while self._running and self._selected:
            time.sleep(config.LINKTEST_INTERVAL_SEC)
            sys_b = self._next_sys_bytes()
            self._send_raw(build_control_msg(HSMS_LINKTEST_REQ, sys_b))
            logger.debug("LinktestReq sent (sys=%d)", sys_b)

    def _next_sys_bytes(self) -> int:
        v = self._sys_bytes
        self._sys_bytes = (self._sys_bytes + 1) & 0xFFFF
        return v
