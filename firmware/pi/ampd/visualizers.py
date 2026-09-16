"""The visualiser registry.

Rendering happens in the browser — the panel is a kiosk Chromium page, so
a visualiser is a JavaScript module, not Python.  What lives here is the
*manifest*: which modes exist, what data each one needs, which are enabled
and in what order.  The web UI reads and writes that; the panel asks for
the enabled list and cycles through it.

Adding a mode is two steps and no code change here: drop
``web/panel/vis/<id>.js`` in, and add a row to ``BUILTIN`` (or let it be
discovered — see :func:`discover`).

Everything is stereo.  The Pico sends a separate spectrum per channel and
the audio stream carries interleaved L,R, so no mode has to settle for a
mono sum.  A few genuinely want one curve; they can average.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, asdict, field
from enum import Enum
from typing import Iterable


class Needs(str, Enum):
    """What feed a mode has to have to draw anything.

    The panel uses this to decide whether the analogue audio stream is
    worth turning on — it costs 52% of the UART, and there is no reason
    to pay that for a needle that only wants an RMS number.
    """

    #: peak_l/r and rms_l/r from MSG_METER.  Always available, ~47 Hz.
    LEVEL = "level"
    #: band_l[32] and band_r[32] from MSG_METER.  Always available.
    BANDS = "bands"
    #: Time-domain samples.  Digital: from the Pi's own audio.  Analogue:
    #: needs MSG_AUDIO, so selecting one of these turns the stream on.
    WAVE = "wave"


@dataclass
class Visualizer:
    """One mode, as the manifest sees it."""

    id: str
    name: str
    needs: list[Needs]
    #: Rough ordering from plainest to busiest, used for the default sort
    #: and shown as a hint in the picker.
    complexity: int
    blurb: str
    #: Off means it is not in the swipe rotation.  It stays installed.
    enabled: bool = True
    #: Per-mode knobs the module declares, e.g. bar count or decay rate.
    params: dict = field(default_factory=dict)

    @property
    def module(self) -> str:
        return f"vis/{self.id}.js"

    @property
    def needs_audio_stream(self) -> bool:
        return Needs.WAVE in self.needs

    def to_json(self) -> dict:
        d = asdict(self)
        d["needs"] = [n.value for n in self.needs]
        d["module"] = self.module
        return d


L, B, W = Needs.LEVEL, Needs.BANDS, Needs.WAVE

#: The fifteen, in the order they were shown and numbered.  Blank is
#: sixteenth because it is an escape hatch, not a mode you scroll to.
BUILTIN: tuple[Visualizer, ...] = (
    Visualizer("vu", "VU meters", [L], 1,
               "Two needles with 300 ms ballistics and peak hold."),
    Visualizer("vudial", "Round dials", [L], 1,
               "Two circular gauges with a full sweep. Same numbers, "
               "different century."),
    Visualizer("ppm", "PPM ladder", [L], 2,
               "Segmented columns with PPM ballistics — 10 ms attack, "
               "1.5 s decay. Catches transients a VU never shows."),
    Visualizer("peakbars", "Peak bars", [L], 2,
               "Horizontal L/R bars with peak-hold ticks and a dB scale."),
    Visualizer("bars", "Spectrum bars", [B], 3,
               "The classic. Log-spaced bars per channel with falling caps."),
    Visualizer("mirror", "Mirrored bars", [B], 3,
               "Left above the centre line, right below."),
    Visualizer("led", "LED matrix", [B], 4,
               "Bars quantised into blocks, green through amber to red."),
    Visualizer("dots", "Dot matrix", [B], 4,
               "The same field as circles that grow and brighten."),
    Visualizer("dualarea", "Dual area", [B], 5,
               "Filled spectrum areas, left up and right down."),
    Visualizer("stereoblend", "Stereo blend", [B], 5,
               "Both channels as translucent filled curves, overlapping. "
               "The overlap is the correlation, and you can see it."),
    Visualizer("scope", "Waveform scope", [W], 6,
               "Two real time-domain traces with a soft glow."),
    Visualizer("fillwave", "Filled waveform", [W], 6,
               "Each channel filled and mirrored about its own axis."),
    Visualizer("mesh", "Ribbon mesh", [B], 7,
               "Phase-offset copies of both spectra, woven."),
    Visualizer("gonio", "Goniometer", [W], 8,
               "Left against right. A vertical line is mono, a circle is "
               "wide, a diagonal means one channel is out of phase."),
    Visualizer("particles", "Particle field", [B, L], 8,
               "Dots thrown on transients from each side, drifting."),
    Visualizer("blank", "Blank", [], 0,
               "Nothing at all. The right answer late at night."),
)

#: Shipped on. The rest are installed and one long-press away.
DEFAULT_ENABLED = ("vu", "bars", "mirror", "stereoblend", "scope",
                   "dualarea", "led", "blank")


class Registry:
    """The manifest, persisted as JSON next to the other settings.

    Deliberately forgiving: a state file naming a mode that no longer
    exists, or missing one that has just been added, is normal after an
    update and must not stop the panel from starting.
    """

    def __init__(self, path: str | None = None,
                 modes: Iterable[Visualizer] = BUILTIN) -> None:
        self.path = path
        self._all: dict[str, Visualizer] = {v.id: v for v in modes}
        self._order: list[str] = [v.id for v in modes]
        for v in self._all.values():
            v.enabled = v.id in DEFAULT_ENABLED
        if path and os.path.exists(path):
            self.load()

    # ------------------------------------------------------------ read

    def all(self) -> list[Visualizer]:
        return [self._all[i] for i in self._order]

    def enabled(self) -> list[Visualizer]:
        out = [self._all[i] for i in self._order if self._all[i].enabled]
        # Never hand the panel an empty rotation; it would have nothing to
        # draw and no way to get to the picker.
        return out or [self._all["blank"]]

    def get(self, vid: str) -> Visualizer | None:
        return self._all.get(vid)

    @property
    def wants_audio_stream(self) -> bool:
        """True if any enabled mode needs time-domain data."""
        return any(v.needs_audio_stream for v in self.enabled())

    def to_json(self) -> dict:
        return {
            "modes": [v.to_json() for v in self.all()],
            "wants_audio_stream": self.wants_audio_stream,
        }

    # ----------------------------------------------------------- write

    def set_enabled(self, vid: str, on: bool) -> bool:
        v = self._all.get(vid)
        if v is None:
            return False
        v.enabled = bool(on)
        self.save()
        return True

    def reorder(self, ids: list[str]) -> None:
        """Reorder to match ``ids``; anything unmentioned keeps its place
        at the end, so a stale UI cannot drop a newly added mode."""
        seen = [i for i in ids if i in self._all]
        rest = [i for i in self._order if i not in seen]
        self._order = seen + rest
        self.save()

    def set_params(self, vid: str, params: dict) -> bool:
        v = self._all.get(vid)
        if v is None:
            return False
        v.params.update(params)
        self.save()
        return True

    # ------------------------------------------------------ persistence

    def load(self) -> None:
        try:
            with open(self.path, encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, ValueError):
            return
        order = [i for i in data.get("order", []) if i in self._all]
        self._order = order + [i for i in self._order if i not in order]
        for vid, saved in (data.get("modes") or {}).items():
            v = self._all.get(vid)
            if v is None:
                continue          # a mode that was removed; ignore quietly
            v.enabled = bool(saved.get("enabled", v.enabled))
            v.params.update(saved.get("params") or {})

    def save(self) -> None:
        if not self.path:
            return
        data = {
            "order": self._order,
            "modes": {v.id: {"enabled": v.enabled, "params": v.params}
                      for v in self._all.values()},
        }
        tmp = self.path + ".tmp"
        os.makedirs(os.path.dirname(self.path) or ".", exist_ok=True)
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2)
        os.replace(tmp, self.path)      # never leave a half-written file


def discover(vis_dir: str) -> list[Visualizer]:
    """Every builtin that actually has a module on disk.

    Lets a mode be removed by deleting its file, and stops the panel
    trying to import something that is not there.
    """
    out = []
    for v in BUILTIN:
        if v.id == "blank" or os.path.exists(os.path.join(vis_dir, f"{v.id}.js")):
            out.append(v)
    return out
