"""Check ampd/proto.py against firmware/common/proto.h.

Two copies of a protocol drift.  This reads the C header and asserts that
every message id, command id and function id matches, so the drift shows
up here rather than as a control that silently stops working.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from ampd import proto  # noqa: E402

HEADER = os.path.normpath(os.path.join(ROOT, "..", "common", "proto.h"))


def c_defines(text: str) -> dict[str, int]:
    out = {}
    for name, val in re.findall(r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b",
                                text, re.M):
        out[name] = int(val, 0)
    return out


def c_enum(text: str, name: str) -> dict[str, int]:
    """Read a C enum with implicit numbering."""
    m = re.search(r"typedef enum \{(.*?)\}\s*" + name, text, re.S)
    if not m:
        raise AssertionError(f"enum {name} not found in the header")
    out, nxt = {}, 0
    for tok in m.group(1).replace("\n", " ").split(","):
        tok = re.sub(r"/\*.*?\*/", " ", tok).strip()
        if not tok:
            continue
        if "=" in tok:
            k, v = tok.split("=", 1)
            nxt = int(v.strip(), 0)
            tok = k.strip()
        out[tok.strip()] = nxt
        nxt += 1
    return out


def main() -> int:
    if not os.path.exists(HEADER):
        print(f"cannot find {HEADER}")
        return 1

    with open(HEADER, encoding="utf-8") as fh:
        text = fh.read()

    d = c_defines(text)
    fails = []

    def same(label: str, py, c):
        if py != c:
            fails.append(f"{label}: python {py} vs C {c}")

    # --- message ids
    for m in proto.Msg:
        same(f"MSG_{m.name}", int(m), d.get(f"MSG_{m.name}"))

    # --- command ids
    for c in proto.Cmd:
        same(f"CMD_{c.name}", int(c), d.get(f"CMD_{c.name}"))

    # --- framing constants
    same("PROTO_SOF", proto.SOF, d.get("PROTO_SOF"))
    same("PROTO_MAX_PAYLOAD", proto.MAX_PAYLOAD, d.get("PROTO_MAX_PAYLOAD"))
    same("AUDIO_RATE_HZ", proto.AUDIO_RATE_HZ, d.get("AUDIO_RATE_HZ"))
    same("NP_FLAG_PLAYING", proto.NP_FLAG_PLAYING, d.get("NP_FLAG_PLAYING"))

    # --- function ids, which are an enum not defines
    fn = c_enum(text, "fn_id_t")
    for f in proto.Fn:
        same(f"FN_{f.name}", int(f), fn.get(f"FN_{f.name}"))
    extra = set(fn) - {f"FN_{f.name}" for f in proto.Fn} - {"FN_COUNT"}
    if extra:
        fails.append(f"C has function ids python does not: {sorted(extra)}")

    print(f"checked {len(proto.Msg)} messages, {len(proto.Cmd)} commands, "
          f"{len(proto.Fn)} functions against the header")

    # --- CRC against a frame computed by hand from the C algorithm
    body = bytes([proto.Msg.PING])
    if proto.crc8(body) != _ref_crc8(body):
        fails.append("crc8 disagrees with the reference implementation")

    # --- codec round trip, including a resync after garbage
    dec = proto.Decoder()
    wire = b"\x11\x22" + proto.build(proto.Msg.LOG, b"hello") + b"\xa5\x00"
    got = list(dec.feed(wire))
    if len(got) != 1 or got[0].payload != b"hello":
        fails.append(f"decoder did not resync past garbage: {got}")

    # --- a corrupted frame must be dropped, not delivered
    dec = proto.Decoder()
    bad = bytearray(proto.build(proto.Msg.LOG, b"hello"))
    bad[-1] ^= 0xFF
    if list(dec.feed(bytes(bad))) or dec.crc_errors != 1:
        fails.append("a bad CRC was not rejected")

    # --- now-playing has to fit one frame even with silly metadata
    blob = proto.encode_nowplaying(True, 7, "A" * 200, "B" * 200, "C" * 200)
    if len(blob) > proto.MAX_PAYLOAD:
        fails.append(f"encode_nowplaying overflowed: {len(blob)} bytes")
    if blob[0] != proto.NP_FLAG_PLAYING or blob[1] != 7:
        fails.append("encode_nowplaying header wrong")

    # --- mu-law table matches G.711 at the anchor points
    import struct
    def dec_ulaw(u):
        return struct.unpack_from("<h", proto.ULAW_TABLE, u * 2)[0]
    # G.711 inverts every bit on the way out, so 0xFF is idle, 0x00 is
    # the most negative code and 0x80 the most positive - not the other
    # way round, which is the easy mistake to make here.
    anchors = {0xFF: 0, 0x7F: 0}
    for code, want in anchors.items():
        if dec_ulaw(code) != want:
            fails.append(f"mu-law {code:#04x} -> {dec_ulaw(code)}, expected {want}")
    if dec_ulaw(0x00) > -32000:
        fails.append(f"mu-law 0x00 -> {dec_ulaw(0x00)}, expected most negative")
    if dec_ulaw(0x80) < 32000:
        fails.append(f"mu-law 0x80 -> {dec_ulaw(0x80)}, expected most positive")
    if dec_ulaw(0x00) != -dec_ulaw(0x80):
        fails.append("mu-law table is not symmetric")

    for f in fails:
        print("FAIL:", f)
    print("all checks passed" if not fails else f"{len(fails)} FAILURES")
    return 1 if fails else 0


def _ref_crc8(data: bytes) -> int:
    """Independent restatement of the C loop, written from the header."""
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) & 0xFF) ^ 0x31
            else:
                crc = (crc << 1) & 0xFF
    return crc


if __name__ == "__main__":
    raise SystemExit(main())
