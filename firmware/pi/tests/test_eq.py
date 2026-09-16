"""How well can three bands stand in for seven?

This is not a unit test in the tick-a-box sense.  The question it answers
is whether the seven-slider UI is honest, and the number that matters is
how far the achieved curve can sit from the drawn one.  Run it directly
to see the table.
"""

import sys
import time

import numpy as np

sys.path.insert(0, __file__.rsplit("tests", 1)[0])

from ampd import eq  # noqa: E402


CASES = [
    ("flat", [0, 0, 0, 0, 0, 0, 0]),
    ("bass boost", [8, 5, 0, 0, 0, 0, 0]),
    ("treble boost", [0, 0, 0, 0, 2, 6, 8]),
    ("smiley", [7, 4, 0, -2, 0, 4, 7]),
    ("loudness-ish", [9, 4, 0, 0, 0, 2, 5]),
    ("vocal lift", [0, 0, 2, 5, 3, 0, 0]),
    ("cut mud", [0, -6, -4, 0, 0, 0, 0]),
    ("tilt down", [6, 4, 2, 0, -2, -4, -6]),
    ("tilt up", [-6, -4, -2, 0, 2, 4, 6]),
    ("notch 1k", [0, 0, 0, -10, 0, 0, 0]),
    ("everything up", [10, 10, 10, 10, 10, 10, 10]),
    ("comb (worst case)", [8, -8, 8, -8, 8, -8, 8]),
]


def main() -> int:
    # Warm the precomputed tables so the timing below is the search only.
    eq.solve([0] * 7)

    print(f"{'preset':<20} {'max err':>8} {'rms':>7} {'ms':>6}   chip settings")
    print("-" * 96)

    worst = 0.0
    for name, sliders in CASES:
        t0 = time.perf_counter()
        sol = eq.solve(sliders)
        ms = (time.perf_counter() - t0) * 1000.0

        worst = max(worst, sol.max_error_db)
        s = (f"bass {sol.bass.gain_db:+3d} dB @{sol.bass.f0_hz:>5.0f} Q{sol.bass.q:<4} | "
             f"mid {sol.mid.gain_db:+3d} @{sol.mid.f0_hz:>5.0f} Q{sol.mid.q:<4} | "
             f"treb {sol.treble.gain_db:+3d} @{sol.treble.f0_hz:>5.0f} Q{sol.treble.q}")
        print(f"{name:<20} {sol.max_error_db:>7.2f}  {sol.rms_error_db:>6.2f}  "
              f"{ms:>5.1f}   {s}")

    print()

    fails = 0

    # 1. Flat in, flat out.  If the solver invents a curve for a flat
    #    target, every other result is suspect.
    sol = eq.solve([0] * 7)
    if (sol.bass.gain_db, sol.mid.gain_db, sol.treble.gain_db) != (0, 0, 0):
        print("FAIL: a flat target did not give flat settings")
        fails += 1

    # 2. Direction.  A bass boost must not come back as a bass cut.
    for name, sliders, band, sign in [
        ("bass boost", [8, 5, 0, 0, 0, 0, 0], "bass", +1),
        ("treble boost", [0, 0, 0, 0, 2, 6, 8], "treble", +1),
        ("cut mud", [0, -6, -4, 0, 0, 0, 0], "bass", -1),
    ]:
        g = getattr(eq.solve(sliders), band).gain_db
        if g * sign <= 0:
            print(f"FAIL: {name} gave {band} {g:+d} dB, wrong direction")
            fails += 1

    # 3. The headroom ceiling has to bind.  The Pico clamps EQ boost
    #    against volume; if the UI ignores that it shows a curve the
    #    firmware will silently undo.
    sol = eq.solve([10, 10, 10, 10, 10, 10, 10], max_boost_db=3)
    if max(sol.bass.gain_db, sol.mid.gain_db, sol.treble.gain_db) > 3:
        print("FAIL: solver exceeded the headroom ceiling")
        fails += 1
    print(f"headroom clamp at +3 dB -> bass {sol.bass.gain_db:+d}, "
          f"mid {sol.mid.gain_db:+d}, treble {sol.treble.gain_db:+d}")

    # 4. Cuts are never clamped - the ceiling is about boost only.
    sol = eq.solve([-12, -12, -12, -12, -12, -12, -12], max_boost_db=0)
    if min(sol.bass.gain_db, sol.mid.gain_db, sol.treble.gain_db) >= 0:
        print("FAIL: headroom ceiling wrongly blocked a cut")
        fails += 1

    # 5. Determinism.  The UI redraws on every drag; a solver that
    #    wanders would make the achieved curve flicker.
    a = eq.solve([7, 4, 0, -2, 0, 4, 7])
    b = eq.solve([7, 4, 0, -2, 0, 4, 7])
    if a != b:
        print("FAIL: solver is not deterministic")
        fails += 1

    # 6. Fast enough to run on every slider move on a Pi 4.
    t0 = time.perf_counter()
    for _ in range(50):
        eq.solve([7, 4, 0, -2, 0, 4, 7])
    per = (time.perf_counter() - t0) / 50 * 1000
    print(f"solve takes {per:.2f} ms on this machine "
          f"(a Pi 4 is roughly 5x slower)")
    if per > 40:
        print("FAIL: too slow to run interactively")
        fails += 1

    print(f"\nworst miss across all presets: {worst:.2f} dB")
    print("all checks passed" if not fails else f"{fails} FAILURES")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
