"""The wire format, in Python.

A port of ``firmware/common/proto.h`` and ``proto.c``.  Two copies of a
protocol is one more than anyone wants, but the C side is shared by three
microcontrollers and this side is Python, so the duplication is real.
``tests/test_proto.py`` checks the two against each other by reading the
header, which is the next best thing to sharing the file.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Iterator

SOF = 0xA5
MAX_PAYLOAD = 250


# ------------------------------------------------------------ messages


class Msg(IntEnum):
    # Pico -> Pi
    STATE = 0x01
    METER = 0x02
    MENU = 0x03
    EVENT = 0x04
    LOG = 0x05
    IRLEARN = 0x06
    AUDIO = 0x07
    # Pi -> Pico
    CMD = 0x81
    NOWPLAYING = 0x82
    PING = 0x83
    MENU_KEY = 0x84
    HELLO = 0x85


class Cmd(IntEnum):
    POWER = 0x01
    VOLUME = 0x02
    VOLUME_REL = 0x03
    MUTE = 0x04
    INPUT = 0x05
    EQ_BYPASS = 0x06
    BALANCE = 0x07
    BASS_GAIN = 0x08
    MID_GAIN = 0x09
    TREBLE_GAIN = 0x0A
    BASS_F0 = 0x0B
    MID_F0 = 0x0C
    TREBLE_F0 = 0x0D
    BASS_Q = 0x0E
    MID_Q = 0x0F
    TREBLE_Q = 0x10
    LOUDNESS = 0x11
    PRESET_LOAD = 0x12
    PRESET_SAVE = 0x13
    ENTER_MENU = 0x14
    IR_LEARN = 0x15
    AUDIO_STREAM = 0x16
    AUDIO_MODE = 0x17


class Fn(IntEnum):
    NONE = 0
    VOL_UP = 1
    VOL_DOWN = 2
    MUTE = 3
    POWER = 4
    INPUT = 5
    EQ_BYPASS = 6
    PREV = 7
    NEXT = 8
    PLAY_PAUSE = 9
    MENU = 10
    OK = 11
    BACK = 12
    UP = 13
    DOWN = 14
    PRESET_1 = 15
    PRESET_2 = 16
    PRESET_3 = 17


class Power(IntEnum):
    STANDBY = 0
    STARTING = 1
    ON = 2
    STOPPING = 3


IN_DIGITAL = 0
IN_ANALOG = 1

EQ_ACTIVE = 0
EQ_BYPASS = 1

NP_FLAG_PLAYING = 0x01

AUDIO_RATE_HZ = 24000

METER_BANDS = 32

#: PGA2320: 0.5 dB a step, code 192 is exactly 0 dB.
PGA_CODE_0DB = 192
PGA_DB_PER_STEP = 0.5


def code_to_db(code: int) -> float:
    return PGA_DB_PER_STEP * (code - PGA_CODE_0DB)


def db_to_code(db: float) -> int:
    return max(1, min(PGA_CODE_0DB, round(PGA_CODE_0DB + db / PGA_DB_PER_STEP)))


# ---------------------------------------------------------------- crc


def crc8(data: bytes) -> int:
    """Dallas/Maxim, poly 0x31, init 0x00 — same as ``proto_crc8``."""
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x31) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def build(msg_type: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} > {MAX_PAYLOAD}")
    body = bytes([msg_type]) + payload
    return bytes([SOF, len(payload)]) + body + bytes([crc8(body)])


def build_cmd(cmd: int, value: int = 0) -> bytes:
    """``MSG_CMD`` is [id][int16 little endian]."""
    return build(Msg.CMD, bytes([cmd]) + struct.pack("<h", int(value)))


# ------------------------------------------------------------ receiver


@dataclass
class Frame:
    type: int
    payload: bytes


class Decoder:
    """Feed bytes, get frames.

    Resynchronises on its own: a bad CRC or a stray byte costs at most
    one frame, because the SOF search restarts immediately.
    """

    _S_SOF, _S_LEN, _S_TYPE, _S_DATA, _S_CRC = range(5)

    def __init__(self) -> None:
        self._state = self._S_SOF
        self._len = 0
        self._type = 0
        self._buf = bytearray()
        self.crc_errors = 0

    def feed(self, data: bytes) -> Iterator[Frame]:
        for b in data:
            f = self._byte(b)
            if f is not None:
                yield f

    def _byte(self, b: int) -> Frame | None:
        st = self._state
        if st == self._S_SOF:
            if b == SOF:
                self._state = self._S_LEN
        elif st == self._S_LEN:
            if b > MAX_PAYLOAD:
                self._state = self._S_SOF
            else:
                self._len = b
                self._buf.clear()
                self._state = self._S_TYPE
        elif st == self._S_TYPE:
            self._type = b
            self._state = self._S_DATA if self._len else self._S_CRC
        elif st == self._S_DATA:
            self._buf.append(b)
            if len(self._buf) >= self._len:
                self._state = self._S_CRC
        elif st == self._S_CRC:
            self._state = self._S_SOF
            body = bytes([self._type]) + bytes(self._buf)
            if crc8(body) == b:
                return Frame(self._type, bytes(self._buf))
            self.crc_errors += 1
        return None


# ------------------------------------------------------------- decode


@dataclass
class AmpState:
    """``MSG_STATE``, as the Pico sends it."""

    power: int = 0
    vol_code: int = 0
    balance: int = 0
    muted: bool = False
    input: int = IN_DIGITAL
    eq_path: int = EQ_ACTIVE
    tone_gain: list[int] = field(default_factory=lambda: [0, 0, 0])
    tone_f0: list[int] = field(default_factory=lambda: [0, 0, 0])
    tone_q: list[int] = field(default_factory=lambda: [0, 0, 0])
    loudness: int = 0
    headroom_db: int = 0
    max_vol: int = PGA_CODE_0DB

    @property
    def volume_db(self) -> float:
        return code_to_db(self.vol_code)

    @property
    def on(self) -> bool:
        return self.power == Power.ON

    @classmethod
    def parse(cls, p: bytes) -> "AmpState":
        if len(p) < 18:
            raise ValueError(f"MSG_STATE is {len(p)} bytes, expected 18")
        s8 = lambda v: v - 256 if v > 127 else v  # noqa: E731
        return cls(
            power=p[0],
            vol_code=p[1],
            balance=s8(p[2]),
            muted=bool(p[3]),
            input=p[4],
            eq_path=p[5],
            tone_gain=[s8(p[6]), s8(p[7]), s8(p[8])],
            tone_f0=[p[9], p[10], p[11]],
            tone_q=[p[12], p[13], p[14]],
            loudness=p[15],
            headroom_db=p[16],
            max_vol=p[17],
        )


@dataclass
class Meter:
    """``MSG_METER``: the cheap always-on feed, ~47 Hz.

    Stereo throughout — the Pico runs a transform per channel rather than
    one on the sum, because a summed spectrum cannot be un-summed here and
    every visualiser on the panel wants both channels.
    """

    peak_l: int = 0
    peak_r: int = 0
    rms_l: int = 0
    rms_r: int = 0
    band_l: list[int] = field(default_factory=lambda: [0] * METER_BANDS)
    band_r: list[int] = field(default_factory=lambda: [0] * METER_BANDS)

    @property
    def band_mono(self) -> list[int]:
        """For the few modes that genuinely want one curve."""
        return [(a + b) // 2 for a, b in zip(self.band_l, self.band_r)]

    @classmethod
    def parse(cls, p: bytes) -> "Meter":
        n = 4 + 2 * METER_BANDS
        if len(p) < n:
            raise ValueError(f"MSG_METER is {len(p)} bytes, expected {n}")
        return cls(
            p[0], p[1], p[2], p[3],
            list(p[4:4 + METER_BANDS]),
            list(p[4 + METER_BANDS:n]),
        )


@dataclass
class AudioChunk:
    seq: int
    stereo: bool
    ulaw: bytes

    @classmethod
    def parse(cls, p: bytes) -> "AudioChunk":
        if len(p) < 2:
            raise ValueError("MSG_AUDIO too short")
        return cls(p[0], bool(p[1]), p[2:])


def encode_nowplaying(playing: bool, art_id: int,
                      artist: str, title: str, album: str) -> bytes:
    """``MSG_NOWPLAYING``: flags, art id, then three NUL-terminated strings.

    Truncated to fit one frame — this ends up on a 480x480 round screen
    over a radio link, so there is no point sending a paragraph.
    """
    head = bytes([NP_FLAG_PLAYING if playing else 0, art_id & 0xFF])
    room = MAX_PAYLOAD - len(head)

    def clip(s: str, n: int) -> bytes:
        return s.encode("utf-8", "replace")[:n]

    parts = [clip(artist, 63), clip(title, 63), clip(album, 63)]
    blob = b"\0".join(parts) + b"\0"
    while len(blob) > room and max(len(x) for x in parts) > 8:
        longest = max(range(3), key=lambda i: len(parts[i]))
        parts[longest] = parts[longest][:-4]
        blob = b"\0".join(parts) + b"\0"
    return head + blob[:room]


# ------------------------------------------------------------- mu-law

def _ulaw_table() -> bytes:
    """G.711 decode table, built once."""
    exp_lut = (0, 132, 396, 924, 1980, 4092, 8316, 16764)
    out = bytearray(512)
    for u in range(256):
        v = ~u & 0xFF
        sign, exp, mant = v & 0x80, (v >> 4) & 0x07, v & 0x0F
        mag = exp_lut[exp] + (mant << (exp + 3))
        val = -mag if sign else mag
        struct.pack_into("<h", out, u * 2, val)
    return bytes(out)


ULAW_TABLE = _ulaw_table()
