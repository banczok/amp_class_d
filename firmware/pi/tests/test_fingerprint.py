"""How many lookups does an evening of listening actually cost?

That is the whole question for recognition on the analogue input.  Every
provider is cheap if you ask it once a track and expensive if you ask it
once a second, so the thing worth testing is the watcher, not the network
call.
"""

import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from ampd.fingerprint import MatchCache, Match, TrackWatcher  # noqa: E402

FPS = 47.0          # MSG_METER arrives at about this rate
DT = 1.0 / FPS


def bands(profile: np.ndarray, db: float = 0.0) -> list[int]:
    """Band bytes at a given level offset.

    The bytes are logarithmic at 2.657 units per dB, so turning the
    volume down subtracts from every band. Multiplying them instead -
    the obvious thing to write - models a tone control, not a volume
    knob, and would let a broken normalisation pass this test.
    """
    return list(np.clip(profile + db * 2.657, 0, 255).astype(int))


def main() -> int:
    rng = np.random.default_rng(7)
    fails = []

    # ---- 1. an evening: 12 tracks, four minutes each, 2 s gaps --------
    # The real rate limiter, because the cost that matters is the cost
    # with the service configured the way it will actually run.
    w = TrackWatcher(min_interval=45.0)
    fires = 0
    for _ in range(12):
        profile = rng.random(32) * 200 + 30
        for _ in range(int(240 * FPS)):
            # +/-3 dB of band-to-band wander, which is what real music
            # does frame to frame. Multiplying the bytes instead would be
            # about +/-11 dB, i.e. a different piece of music every frame.
            b = bands(profile + rng.normal(0, 3.0 * 2.657, 32))
            w.feed(b, b, dt=DT)
            fires += w.should_identify()
        for _ in range(int(2 * FPS)):
            w.feed([0] * 32, [0] * 32, dt=DT)
            fires += w.should_identify()

    per_track = fires / 12
    print(f"12 tracks x 4 min -> {fires} lookups ({per_track:.2f} per track)")
    print(f"  an 8 hour day is about {per_track * 120:.0f} lookups "
          f"before the cache takes any")
    if per_track > 3.0:
        fails.append(f"{per_track:.1f} lookups per track is too many")

    # ---- 2. turning the volume down is not a new track ---------------
    w2 = TrackWatcher(min_interval=0.0)
    p = rng.random(32) * 200 + 30
    for _ in range(int(40 * FPS)):
        b = bands(p)
        w2.feed(b, b, dt=DT)
        w2.should_identify()
    spurious = 0
    for db in np.linspace(0.0, -20.0, int(20 * FPS)):
        b = bands(p, db)
        w2.feed(b, b, dt=DT)
        spurious += w2.should_identify()
    print(f"20 s volume sweep, 0 to -20 dB -> {spurious} spurious lookups")
    if spurious:
        fails.append(f"a volume change armed {spurious} lookups")

    # ---- 3. but a real change must be caught, and quickly -------------
    w3 = TrackWatcher(min_interval=0.0)
    a = rng.random(32) * 200 + 30
    for _ in range(int(60 * FPS)):
        b = bands(a)
        w3.feed(b, b, dt=DT)
        w3.should_identify()
    c = rng.random(32) * 200 + 30
    delay = None
    for i in range(int(30 * FPS)):
        b = bands(c)
        w3.feed(b, b, dt=DT)
        if w3.should_identify() and delay is None:
            delay = i / FPS
    print(f"new material detected after "
          f"{'never' if delay is None else f'{delay:.1f} s'}")
    if delay is None or delay > 12.0:
        fails.append("a genuine track change was missed or was too slow")

    # ---- 4. a gap always arms, however similar the next track ---------
    w4 = TrackWatcher(min_interval=0.0)
    p = rng.random(32) * 200 + 30
    for _ in range(int(30 * FPS)):
        b = bands(p)
        w4.feed(b, b, dt=DT)
        w4.should_identify()
    for _ in range(int(2 * FPS)):
        w4.feed([0] * 32, [0] * 32, dt=DT)
    if not w4.should_identify():
        fails.append("a silent gap did not arm a lookup")
    else:
        print("silent gap armed a lookup: yes")

    # ---- 5. the rate limiter is the last line of defence -------------
    w5 = TrackWatcher(min_interval=45.0)
    p = rng.random(32) * 200 + 30
    fired = 0
    for i in range(int(30 * FPS)):
        prof = p if (i // int(3 * FPS)) % 2 else rng.random(32) * 200 + 30
        b = bands(prof + rng.normal(0, 8.0, 32))
        w5.feed(b, b, dt=DT)
        fired += w5.should_identify()
    print(f"pathological input, 30 s, 45 s limit -> {fired} lookups")
    if fired > 1:
        fails.append(f"the rate limiter let {fired} through in 30 s")

    # ---- 6. cache keys survive volume but separate different music ----
    k1 = MatchCache.key_for(bands(p), bands(p))
    k2 = MatchCache.key_for(bands(p, -6.0), bands(p, -6.0))
    k3 = MatchCache.key_for(bands(rng.random(32) * 200 + 30),
                            bands(rng.random(32) * 200 + 30))
    print(f"cache key across a 6 dB change: {'same' if k1 == k2 else 'DIFFERENT'}")
    print(f"cache key for other music:      {'differs' if k1 != k3 else 'COLLIDES'}")
    if k1 != k2:
        fails.append("the cache key moves with volume, so it will never hit")
    if k1 == k3:
        fails.append("the cache key collides across different music")

    tmp = os.path.join(os.path.dirname(__file__), "_cache_test.json")
    try:
        cache = MatchCache(tmp)
        cache.put(k1, Match("Elbow", "Weather to Fly", provider="shazam"))
        again = MatchCache(tmp).get(k1)
        if not again or again.title != "Weather to Fly":
            fails.append("the cache did not survive a reload")
        else:
            print(f"cache reload: {again.artist} - {again.title}")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)

    for f in fails:
        print("FAIL:", f)
    print("\nall checks passed" if not fails else f"\n{len(fails)} FAILURES")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
