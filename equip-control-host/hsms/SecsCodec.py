"""
SECS-II / HSMS 메시지 인코딩·디코딩
SEMI E37 (HSMS) + SEMI E5 (SECS-II) 부분 구현
"""
import struct

# ── HSMS 헤더 상수 ────────────────────────────────────────────────
HSMS_SELECT_REQ   = 0x01
HSMS_SELECT_RSP   = 0x02
HSMS_LINKTEST_REQ = 0x05
HSMS_LINKTEST_RSP = 0x06
HSMS_SEPARATE_REQ = 0x09
HSMS_DATA_MSG     = 0x00  # SType=0 → SECS-II 데이터 메시지


def build_hsms_header(session_id: int, stream: int, function: int,
                      system_bytes: int, p_bit: bool = False,
                      stype: int = HSMS_DATA_MSG) -> bytes:
    """HSMS 10바이트 헤더 생성"""
    sb_hi = session_id >> 8
    sb_lo = session_id & 0xFF
    # W-bit: Host→EC 요청 메시지(홀수 function)면 1
    w_bit = 0x80 if (function % 2 == 1 and stype == HSMS_DATA_MSG) else 0x00
    stream_byte = (stream & 0x7F) | w_bit
    return struct.pack(">BBBBBBBBBB",
                       sb_hi, sb_lo,
                       stream_byte, function,
                       p_bit, stype,
                       0x00, 0x00,
                       (system_bytes >> 8) & 0xFF, system_bytes & 0xFF)


def build_control_msg(stype: int, system_bytes: int) -> bytes:
    """SelectReq/Rsp, LinktestReq/Rsp 등 제어 메시지 (바디 없음)"""
    header = build_hsms_header(0xFFFF, 0, 0, system_bytes, stype=stype)
    length = struct.pack(">I", 10)
    return length + header


def build_data_msg(session_id: int, stream: int, function: int,
                   system_bytes: int, body: bytes = b"") -> bytes:
    """SECS-II 데이터 메시지 (length 4B + header 10B + body)"""
    header = build_hsms_header(session_id, stream, function, system_bytes)
    length = struct.pack(">I", 10 + len(body))
    return length + header + body


def parse_message(data: bytes) -> dict:
    """
    수신 바이트 파싱 → dict 반환
    {length, session_id, stream, function, w_bit, stype, system_bytes, body}
    """
    if len(data) < 14:
        return {}
    length = struct.unpack(">I", data[:4])[0]
    h = data[4:14]
    session_id   = (h[0] << 8) | h[1]
    w_bit        = bool(h[2] & 0x80)
    stream       = h[2] & 0x7F
    function     = h[3]
    stype        = h[5]
    system_bytes = (h[8] << 8) | h[9]
    body         = data[14:4 + length]
    return {
        "length": length,
        "session_id": session_id,
        "stream": stream,
        "function": function,
        "w_bit": w_bit,
        "stype": stype,
        "system_bytes": system_bytes,
        "body": body,
    }


# ── SECS-II 바디 빌더 ─────────────────────────────────────────────

def encode_list(items: list) -> bytes:
    """L[n] 리스트 아이템 인코딩"""
    n = len(items)
    header = bytes([0x01, n])  # format=L, length=n (단순화: n<64)
    return header + b"".join(items)


def encode_ascii(s: str) -> bytes:
    """A[n] ASCII 문자열"""
    enc = s.encode("ascii")
    return bytes([0x41, len(enc)]) + enc


def encode_u4(value: int) -> bytes:
    """U4 unsigned 32-bit"""
    return bytes([0xB1, 0x04]) + struct.pack(">I", value)


def encode_f4(value: float) -> bytes:
    """F4 float 32-bit"""
    return bytes([0x91, 0x04]) + struct.pack(">f", value)


def encode_boolean(value: bool) -> bytes:
    """Boolean 1바이트"""
    return bytes([0x25, 0x01, 0x01 if value else 0x00])


# ── 메시지 빌더 (이 프로젝트 사용 S/F) ───────────────────────────

def build_s1f13(session_id: int, sys_bytes: int) -> bytes:
    """S1F13 Establish Communication Request"""
    body = encode_list([encode_ascii("EQUIP-CONTROL-HOST")])
    return build_data_msg(session_id, 1, 13, sys_bytes, body)


def build_s2f41(session_id: int, sys_bytes: int, command: str) -> bytes:
    """S2F41 Host Command Send (START / STOP / RESET)"""
    body = encode_list([
        encode_ascii(command),
        encode_list([])        # CPNAMES — 없음
    ])
    return build_data_msg(session_id, 2, 41, sys_bytes, body)


def build_s5f2(session_id: int, sys_bytes: int, ackc5: int = 0) -> bytes:
    """S5F2 Enable/Disable Alarm Ack"""
    body = bytes([0x21, 0x01, ackc5])  # Binary 1바이트
    return build_data_msg(session_id, 5, 2, sys_bytes, body)
