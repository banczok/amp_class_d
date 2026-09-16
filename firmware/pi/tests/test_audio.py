"""Does the audio path actually reconstruct what the Pico sent?

The whole analogue side depends on this: if the ring interleaves wrong,
the goniometer draws nonsense; if the resampler aliases, fingerprints
stop matching. Both fail silently on hardware, so check them here.
"""

import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from ampd import audio, proto  # noqa: E402


def ulaw_encode(pcm: np.ndarray) -> bytes:
    """The encoder from meter.c, so this test round-trips the real thing."""
    out = bytearray()
    for s in np.clip(pcm * 32767.0, -32768, 32767).astype(int):
        v, sign = int(s), 0
        if v < 0:
            sign, v = 0x80, -v
        v = min(v, 32635) + 0x84
        exp, mask = 7, 0x4000
        while (v & mask) == 0 and exp > 0:
            exp -= 1
            mask >>= 1
        mant = (v >> (exp + 3)) & 0x0F
        out.append(~(sign | (exp << 4) | mant) & 0xFF)
    return bytes(out)


def main() -> int:
    fails = []
    rate = audio.RATE

    # ---- 1. stereo interleave survives a round trip -----------------
    t = np.arange(rate // 4) / rate
    left = 0.7 * np.sin(2 * np.pi * 220 * t).astype(np.float32)
    right = 0.4 * np.sin(2 * np.pi * 880 * t).astype(np.float32)

    inter = np.empty(left.size * 2, dtype=np.float32)
    inter[0::2] = left
    inter[1::2] = right
    payload = ulaw_encode(inter)

    ring = audio.AudioRing(seconds=2.0)
    # Feed it the way the link does: in 240-byte frames.
    for i in range(0, len(payload), 240):
        ring.feed(proto.AudioChunk(seq=(i // 240) & 0xFF, stereo=True,
                                   ulaw=payload[i:i + 240]))

    got = ring.latest(left.size)
    err_l = np.abs(got[0] - left).max()
    err_r = np.abs(got[1] - right).max()
    print(f"stereo round trip: worst error L {err_l:.4f}  R {err_r:.4f}")
    if err_l > 0.02 or err_r > 0.02:
        fails.append("mu-law round trip is worse than the codec should be")

    # A swapped interleave would show up as the channels trading
    # frequencies, which the amplitudes catch:
    if not (abs(np.abs(got[0]).max() - 0.7) < 0.05
            and abs(np.abs(got[1]).max() - 0.4) < 0.05):
        fails.append("channels are swapped or mixed")

    # ---- 2. frames that do not divide evenly ------------------------
    # 240 bytes is 120 stereo pairs, but a partial drain gives odd sizes.
    ring2 = audio.AudioRing(seconds=2.0)
    pos, n = 0, 0
    for size in (238, 2, 100, 140, 240, 60):
        if pos >= len(payload):
            break
        ring2.feed(proto.AudioChunk(n & 0xFF, True, payload[pos:pos + size]))
        pos += size
        n += 1
    g2 = ring2.latest(pos // 2)
    if np.abs(g2[0]).max() < 0.5:
        fails.append("ragged frame sizes broke the interleave")
    print(f"ragged frames: L peak {np.abs(g2[0]).max():.3f} (expect ~0.7)")

    # ---- 3. the ring wraps without a seam ---------------------------
    small = audio.AudioRing(seconds=0.1)
    tone = np.sin(2 * np.pi * 100 * np.arange(rate) / rate).astype(np.float32)
    mono = ulaw_encode(tone)
    for i in range(0, len(mono), 240):
        small.feed(proto.AudioChunk(0, False, mono[i:i + 240]))
    tail = small.latest(small.n)
    jumps = np.abs(np.diff(tail[0]))
    print(f"ring wrap: largest sample-to-sample jump {jumps.max():.4f}")
    if jumps.max() > 0.2:
        fails.append("ring wrap leaves a discontinuity")

    # ---- 4. the spectrum lands in the right band --------------------
    an = audio.Analyser()
    for f in (100.0, 1000.0, 5000.0):
        r = audio.AudioRing(seconds=1.0)
        tt = np.arange(rate) / rate
        sig = 0.8 * np.sin(2 * np.pi * f * tt).astype(np.float32)
        inter2 = np.empty(sig.size * 2, dtype=np.float32)
        inter2[0::2] = sig
        inter2[1::2] = sig
        enc = ulaw_encode(inter2[:8192])
        for i in range(0, len(enc), 240):
            r.feed(proto.AudioChunk(0, True, enc[i:i + 240]))
        bl, _ = an.spectrum(r)
        peak = int(np.argmax(bl))
        lo, hi = an.edges_hz[peak], an.edges_hz[peak + 1]
        ok = lo * 0.75 <= f <= hi * 1.34
        print(f"tone {f:>6.0f} Hz -> band {peak:2d} "
              f"({lo:.0f}-{hi:.0f} Hz) {'ok' if ok else 'WRONG'}")
        if not ok:
            fails.append(f"{f} Hz landed in band {peak} ({lo:.0f}-{hi:.0f})")

    # ---- 5. the 24k -> 16k decimation must not alias -----------------
    # A 10 kHz tone is above the 8 kHz Nyquist of the output and must be
    # filtered away, not folded down to 6 kHz where it would corrupt a
    # fingerprint.
    tt = np.arange(rate) / rate
    for f, should_survive in ((1000.0, True), (10000.0, False)):
        sig = np.sin(2 * np.pi * f * tt).astype(np.float32)
        pcm = audio.to_16k_mono_pcm(sig)
        got = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        level = float(np.abs(got).max())
        print(f"resample {f:>6.0f} Hz -> peak {level:.3f} "
              f"({'kept' if should_survive else 'rejected'})")
        if should_survive and level < 0.5:
            fails.append(f"{f} Hz was lost by the resampler")
        if not should_survive and level > 0.2:
            fails.append(f"{f} Hz aliased through at {level:.2f}")

    if pcm:
        n_out = len(audio.to_16k_mono_pcm(sig)) // 2
        print(f"resample length: {n_out} samples from {rate} "
              f"(expect ~16000)")
        if abs(n_out - 16000) > 100:
            fails.append(f"resampled to {n_out} samples, expected ~16000")

    # ---- 6. envelope reduction keeps the shape ----------------------
    r = audio.AudioRing(seconds=1.0)
    tt2 = np.arange(4096) / rate
    sig = np.sin(2 * np.pi * 50 * tt2).astype(np.float32)
    inter3 = np.empty(sig.size * 2, dtype=np.float32)
    inter3[0::2] = sig
    inter3[1::2] = -sig
    enc = ulaw_encode(inter3)
    for i in range(0, len(enc), 240):
        r.feed(proto.AudioChunk(0, True, enc[i:i + 240]))
    el, er = audio.Analyser.envelope(r, 128, seconds=0.05)
    if max(abs(v) for v in el) < 0.5:
        fails.append("envelope flattened the signal")
    # Inverted channels must come back inverted - the goniometer needs it.
    corr = float(np.corrcoef(el, er)[0, 1])
    print(f"envelope: {len(el)} points, L/R correlation {corr:+.2f} "
          f"(expect about -1)")
    if corr > -0.8:
        fails.append(f"envelope lost the channel phase relationship ({corr:+.2f})")

    for f in fails:
        print("FAIL:", f)
    print("\nall checks passed" if not fails else f"\n{len(fails)} FAILURES")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
