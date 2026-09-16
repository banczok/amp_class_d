"""Seven sliders onto a three-band chip.

The BD37033 has exactly three parametric bands.  A seven-band graphic EQ
has seven.  There is no mapping between them, so this module does the
only honest thing available: it treats the seven sliders as a *target
curve* and searches the chip's parameter grid for the closest response
it can actually produce.  The web UI then draws both, so the places
where the hardware cannot follow are visible rather than silent.

Everything the chip can do, from the datasheet (BD37033FV-M rev 002,
select addresses 41/44/47 and 51/54/57):

    bass    f0  60 / 80 / 100 / 120 Hz      Q  0.5 / 1.0 / 1.5 / 2.0
    middle  f0  0.5 / 1 / 1.5 / 2.5 kHz     Q  0.75 / 1.0 / 1.25 / 1.5
    treble  f0  7.5 / 10 / 12.5 / 15 kHz    Q  0.75 / 1.25
    gain    0 to +/-15 dB in 1 dB steps, all three bands

That is 496 x 496 x 248 = 61 million combinations, which is too many to
try on every slider drag.  Each band's response is precomputed once on a
log frequency grid, and the search is coordinate descent: hold two bands,
brute-force the third over its own few hundred options, repeat.  The
bands barely overlap in frequency, so it converges in two or three
passes.
"""

from __future__ import annotations

from dataclasses import dataclass, asdict
from typing import Sequence

import numpy as np

# --------------------------------------------------------------- chip

BASS_F0 = (60.0, 80.0, 100.0, 120.0)
BASS_Q = (0.5, 1.0, 1.5, 2.0)

MID_F0 = (500.0, 1000.0, 1500.0, 2500.0)
MID_Q = (0.75, 1.00, 1.25, 1.50)

TREB_F0 = (7500.0, 10000.0, 12500.0, 15000.0)
TREB_Q = (0.75, 1.25)

GAIN_MIN, GAIN_MAX = -15, 15

BANDS = (
    ("bass", BASS_F0, BASS_Q),
    ("mid", MID_F0, MID_Q),
    ("treble", TREB_F0, TREB_Q),
)

# ------------------------------------------------------------ sliders

#: What the user sees.  ISO-ish octave-and-a-bit centres spanning the
#: audible range; the outer two deliberately sit near the extremes of
#: what the chip's own f0 options can reach.
SLIDER_HZ = (63.0, 160.0, 400.0, 1000.0, 2500.0, 6300.0, 16000.0)

#: A real graphic EQ slider is a peaking filter about an octave wide,
#: not a point on a line.  Interpolating between slider values instead
#: would produce a target no analogue filter could ever match.
SLIDER_Q = 1.4

# --------------------------------------------------------------- grid

#: 20 Hz to 20 kHz, log spaced.  96 points is enough to pin a filter
#: down and small enough that the whole search stays in cache.
GRID = np.geomspace(20.0, 20000.0, 96)

#: Weight the fit toward where the ear and the material actually are.
#: Without this the solver spends its effort matching 20 Hz and 20 kHz,
#: where nothing is, at the cost of the midrange.
_W = np.ones_like(GRID)
_W[GRID < 40.0] = 0.25
_W[GRID > 16000.0] = 0.25
_W /= _W.sum()


def peak_db(freq: np.ndarray, f0: float, q: float, gain_db: float) -> np.ndarray:
    """Magnitude of one analogue peaking section, in dB.

    The BD37033 is an analogue processor, so this is the s-domain
    parametric response, not a digital biquad::

        H(s) = (s^2 + (A.w0/Q)s + w0^2) / (s^2 + (w0/(A.Q))s + w0^2)

    with A = 10^(G/40), which makes the gain at f0 exactly G dB.
    """
    if gain_db == 0:
        return np.zeros_like(freq)
    a = 10.0 ** (gain_db / 40.0)
    w = freq / f0
    d = 1.0 - w * w
    num = np.sqrt(d * d + (a * w / q) ** 2)
    den = np.sqrt(d * d + (w / (a * q)) ** 2)
    return 20.0 * np.log10(num / den)


def target_curve(sliders: Sequence[float], freq: np.ndarray = GRID) -> np.ndarray:
    """The curve seven sliders are asking for."""
    if len(sliders) != len(SLIDER_HZ):
        raise ValueError(f"expected {len(SLIDER_HZ)} sliders, got {len(sliders)}")
    out = np.zeros_like(freq)
    for g, f0 in zip(sliders, SLIDER_HZ):
        out += peak_db(freq, f0, SLIDER_Q, float(g))
    return out


# ------------------------------------------------------------- result


@dataclass(frozen=True)
class BandSetting:
    """One band, as indices the chip actually takes."""

    f0_idx: int
    q_idx: int
    gain_db: int

    f0_hz: float
    q: float


@dataclass(frozen=True)
class EqSolution:
    bass: BandSetting
    mid: BandSetting
    treble: BandSetting

    #: Worst absolute miss, in dB, over the weighted band.  This is the
    #: number worth showing the user: under about 1.5 dB the difference
    #: is not audible, over 4 dB the slider is lying to them.
    max_error_db: float
    rms_error_db: float

    def as_dict(self) -> dict:
        return {
            "bass": asdict(self.bass),
            "mid": asdict(self.mid),
            "treble": asdict(self.treble),
            "max_error_db": round(self.max_error_db, 2),
            "rms_error_db": round(self.rms_error_db, 2),
        }


# -------------------------------------------------------------- solve


class _BandTable:
    """Every response one band can produce, precomputed once."""

    __slots__ = ("name", "combos", "resp")

    def __init__(self, name: str, f0s: Sequence[float], qs: Sequence[float],
                 gain_lo: int, gain_hi: int) -> None:
        self.name = name
        combos = []
        rows = []
        for i, f0 in enumerate(f0s):
            for j, q in enumerate(qs):
                for g in range(gain_lo, gain_hi + 1):
                    combos.append(BandSetting(i, j, g, f0, q))
                    rows.append(peak_db(GRID, f0, q, g))
        self.combos = combos
        self.resp = np.asarray(rows)          # (n_combos, len(GRID))


_CACHE: dict[tuple[int, int], list[_BandTable]] = {}


def _tables(gain_lo: int, gain_hi: int) -> list[_BandTable]:
    key = (gain_lo, gain_hi)
    if key not in _CACHE:
        _CACHE[key] = [
            _BandTable(n, f0s, qs, gain_lo, gain_hi) for n, f0s, qs in BANDS
        ]
    return _CACHE[key]


def solve(sliders: Sequence[float], max_boost_db: int = GAIN_MAX,
          passes: int = 4) -> EqSolution:
    """Find the chip settings whose response best matches the sliders.

    ``max_boost_db`` is the headroom ceiling the Pico enforces
    (``min(15, -3 - pga_dB)``).  It is applied here as well as there, so
    the curve the user is shown is the curve they will actually get
    rather than one the firmware will quietly clamp afterwards.
    """
    hi = int(max(0, min(GAIN_MAX, max_boost_db)))
    tables = _tables(GAIN_MIN, hi)

    target = target_curve(sliders)

    # Start from flat and let each band claim what it can.  Index 0 is
    # NOT flat - the combo list starts at GAIN_MIN - so find the entry
    # that really is 0 dB, or `current` and `chosen` disagree from the
    # first iteration and the descent runs from a corner of the space.
    chosen = [next(i for i, c in enumerate(t.combos) if c.gain_db == 0)
              for t in tables]
    current = np.zeros_like(GRID)

    for _ in range(passes):
        moved = False
        for b, tab in enumerate(tables):
            # Everything except this band.
            rest = current - tab.resp[chosen[b]]
            # Error for every option this band has, all at once.
            err = tab.resp + rest - target                  # (n, grid)
            cost = (err * err * _W).sum(axis=1)
            best = int(np.argmin(cost))
            if best != chosen[b]:
                moved = True
            chosen[b] = best
            current = rest + tab.resp[best]
        if not moved:
            break

    resid = current - target
    return EqSolution(
        bass=tables[0].combos[chosen[0]],
        mid=tables[1].combos[chosen[1]],
        treble=tables[2].combos[chosen[2]],
        max_error_db=float(np.abs(resid[_W > 0.5 / len(GRID)]).max()),
        rms_error_db=float(np.sqrt((resid * resid * _W).sum())),
    )


def achieved_curve(sol: EqSolution, freq: np.ndarray = GRID) -> np.ndarray:
    """What the chip will actually do, for drawing next to the target."""
    out = np.zeros_like(freq)
    for b in (sol.bass, sol.mid, sol.treble):
        out += peak_db(freq, b.f0_hz, b.q, b.gain_db)
    return out
