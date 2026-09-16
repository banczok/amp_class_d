"""Working out what is playing on the analogue input.

The digital input needs none of this — MPD knows exactly what it is
playing.  This is for a turntable, a tuner, or whatever else is plugged
into the analogue socket, where the only evidence is the sound itself.

The thing that actually keeps this cheap is not the choice of service.
It is :class:`TrackWatcher`: fingerprinting on a timer would be hundreds
of lookups a day, and fingerprinting when the music demonstrably changes
is about fifteen an hour of listening, most of which the cache answers
for free.  Every provider below benefits from that, including the free
ones, which have no quota but do have a reputation to not abuse.

Providers are deliberately swappable.  Nothing else in the service knows
which one is in use, or that there is more than one.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import os
import time
from dataclasses import dataclass, asdict

import numpy as np

log = logging.getLogger("ampd.fingerprint")


@dataclass
class Match:
    artist: str
    title: str
    album: str = ""
    art_url: str = ""
    provider: str = ""
    confidence: float = 0.0

    @property
    def key(self) -> str:
        return f"{self.artist}␟{self.title}".lower()


# ---------------------------------------------------------- providers


class Provider:
    """One way of turning audio into a name.

    ``identify`` takes 16 kHz mono signed 16-bit PCM — what
    :func:`ampd.audio.to_16k_mono_pcm` produces — and returns a
    :class:`Match` or ``None``.  It must not raise: a recogniser that is
    down should mean no title, never a broken panel.
    """

    name = "none"
    #: Seconds of audio this provider wants.
    want_seconds = 8.0
    #: Roughly what one lookup costs, for the log and the web UI.
    cost = "free"

    async def identify(self, pcm16: bytes) -> Match | None:
        raise NotImplementedError


class ShazamProvider(Provider):
    """shazamio — free, no account, and by some distance the best recall.

    It is an unofficial client: it builds Shazam's own signature locally
    and posts that, so there is no key to buy and no quota to exceed. The
    cost is that it is reverse-engineered and can stop working whenever
    the far end changes, which is exactly why everything here is behind a
    provider interface.
    """

    name = "shazam"
    want_seconds = 8.0
    cost = "free (unofficial)"

    def __init__(self) -> None:
        self._shazam = None

    async def identify(self, pcm16: bytes) -> Match | None:
        try:
            if self._shazam is None:
                from shazamio import Shazam            # noqa: PLC0415
                self._shazam = Shazam()
            wav = _wav16k(pcm16)
            out = await self._shazam.recognize(wav)
        except Exception as exc:                        # noqa: BLE001
            log.warning("shazam lookup failed: %s", exc)
            return None

        track = (out or {}).get("track") or {}
        if not track:
            return None
        images = track.get("images") or {}
        return Match(
            artist=track.get("subtitle", ""),
            title=track.get("title", ""),
            album=(track.get("sections") or [{}])[0].get("metadata", [{}])[0]
                  .get("text", "") if track.get("sections") else "",
            art_url=images.get("coverarthq") or images.get("coverart") or "",
            provider=self.name,
            confidence=1.0,
        )


class AcoustIDProvider(Provider):
    """AcoustID + MusicBrainz — free, open, and the honest fallback.

    Worth being clear about what it is for: AcoustID indexes whole-track
    Chromaprint fingerprints, so it is excellent at "what file is this"
    and much weaker at "what is playing right now".  It wants a long
    excerpt, it prefers the same master, and a turntable running half a
    percent fast will hurt it.

    Free API key from acoustid.org, no payment, no quota worth worrying
    about at fifteen lookups an hour.
    """

    name = "acoustid"
    want_seconds = 30.0
    cost = "free (API key)"

    def __init__(self, api_key: str) -> None:
        self.api_key = api_key

    async def identify(self, pcm16: bytes) -> Match | None:
        if not self.api_key:
            return None
        try:
            import acoustid                             # noqa: PLC0415
            # acoustid is blocking and does its own HTTP; keep it off the
            # event loop or the panel stutters for a second.
            return await asyncio.get_running_loop().run_in_executor(
                None, self._lookup, acoustid, pcm16)
        except Exception as exc:                        # noqa: BLE001
            log.warning("acoustid lookup failed: %s", exc)
            return None

    def _lookup(self, acoustid, pcm16: bytes) -> Match | None:
        duration = len(pcm16) // 2 / 16000
        fp = acoustid.fingerprint(16000, 1, iter([pcm16]))
        for score, _rid, title, artist in acoustid.parse_lookup_result(
                acoustid.lookup(self.api_key, fp, int(duration))):
            if title and score and score > 0.5:
                return Match(artist=artist or "", title=title,
                             provider=self.name, confidence=float(score))
        return None


class AudDProvider(Provider):
    """AudD — paid, but the cheapest of the commercial ones and the
    simplest API.  Here because it is a drop-in if the free routes stop
    working, not because it is needed.  Check current pricing before
    relying on it; it has changed more than once.
    """

    name = "audd"
    want_seconds = 10.0
    cost = "paid, per lookup"

    def __init__(self, api_token: str) -> None:
        self.api_token = api_token

    async def identify(self, pcm16: bytes) -> Match | None:
        if not self.api_token:
            return None
        try:
            import aiohttp                              # noqa: PLC0415
            form = aiohttp.FormData()
            form.add_field("api_token", self.api_token)
            form.add_field("return", "apple_music")
            form.add_field("file", _wav16k(pcm16),
                           filename="a.wav", content_type="audio/wav")
            timeout = aiohttp.ClientTimeout(total=15)
            async with aiohttp.ClientSession(timeout=timeout) as s:
                async with s.post("https://api.audd.io/", data=form) as r:
                    out = await r.json()
        except Exception as exc:                        # noqa: BLE001
            log.warning("audd lookup failed: %s", exc)
            return None

        res = (out or {}).get("result")
        if not res:
            return None
        art = ((res.get("apple_music") or {}).get("artwork") or {}).get("url", "")
        return Match(
            artist=res.get("artist", ""),
            title=res.get("title", ""),
            album=res.get("album", ""),
            art_url=art.replace("{w}", "600").replace("{h}", "600") if art else "",
            provider=self.name,
            confidence=1.0,
        )


class NullProvider(Provider):
    """Recognition switched off.  Analogue shows the input name and the
    visualiser, which is a perfectly reasonable way to run a hi-fi."""

    name = "off"

    async def identify(self, pcm16: bytes) -> Match | None:
        return None


# -------------------------------------------------------- track watch


class TrackWatcher:
    """Decides *when* to spend a lookup.

    Two signals, both cheap, both from data already arriving:

    * a gap — a stretch quiet enough to be the space between tracks
    * a shift — the long-term spectral balance moving far enough that it
      cannot be the same piece of music

    Either one arms a lookup, and a minimum interval stops a quiet
    passage in the middle of a track from triggering a burst.
    """

    #: Bands quieter than this many units below the loudest are treated
    #: as silence for shape purposes.  60 units is about 22 dB, which is
    #: where a band stops carrying any information about the music.
    FLOOR = 60.0

    def __init__(self, min_interval: float = 45.0,
                 quiet_level: int = 40, quiet_seconds: float = 1.2,
                 shift_threshold: float = 2.0,
                 tau_fast: float = 1.5, tau_slow: float = 25.0) -> None:
        self.min_interval = min_interval
        self.quiet_level = quiet_level
        self.quiet_seconds = quiet_seconds
        # 2.0 is measured, not guessed.  With the shape normalised to
        # -1..0 across 32 bands, the fast/slow distance inside a single
        # track stays under 0.7 even with every band wandering +/-5 dB,
        # while a track change peaks near 8.7.  See tests/.
        self.shift_threshold = shift_threshold
        self.tau_fast = tau_fast
        self.tau_slow = tau_slow

        self._fast = None
        self._slow = None
        self._above = False
        self._quiet_since: float | None = None
        self._gap_armed = False
        self._t = 0.0               # our own clock, fed by dt
        self._wall = 0.0
        self._last_fire = -1e9
        self._armed = True          # identify the first thing we hear

    @classmethod
    def shape(cls, band_l, band_r) -> np.ndarray:
        """The spectral shape, with the level taken out.

        The band bytes are logarithmic — 2.657 units per dB — so turning
        the volume down *subtracts* a constant from every band.  It does
        not scale them.  Dividing by the sum, which is the obvious way to
        normalise a spectrum, therefore removes nothing at all and leaves
        a volume change looking exactly like a new track.  Subtracting
        the loudest band is what actually works here.
        """
        b = (np.asarray(band_l, dtype=np.float32)
             + np.asarray(band_r, dtype=np.float32)) * 0.5
        rel = np.clip(b - float(b.max()), -cls.FLOOR, 0.0)
        return rel / cls.FLOOR                       # -1 .. 0

    def feed(self, band_l: list[int], band_r: list[int],
             dt: float | None = None) -> None:
        wall = time.monotonic()
        if dt is None:
            dt = min(1.0, wall - self._wall) if self._wall else 0.02
        self._wall = wall
        # One clock for everything, advanced by dt.  The gap timer used to
        # read the wall clock while the averages used dt, which meant the
        # two disagreed whenever frames did not arrive in real time.
        self._t += dt

        bands = (np.asarray(band_l, dtype=np.float32)
                 + np.asarray(band_r, dtype=np.float32))
        level = float(bands.max()) * 0.5

        # --- gap detection
        if level < self.quiet_level:
            if self._quiet_since is None:
                self._quiet_since = self._t
                self._gap_armed = False
            elif not self._gap_armed and                     self._t - self._quiet_since > self.quiet_seconds:
                # Once per gap, not once per frame of it.  A long silence
                # would otherwise re-arm continuously and lean entirely on
                # the rate limiter to stay affordable.
                self._armed = True
                self._gap_armed = True
            return

        if (self._quiet_since is not None
                and self._t - self._quiet_since > self.quiet_seconds):
            self._armed = True                  # music resuming after a gap
        self._quiet_since = None
        self._gap_armed = False

        # --- spectral shift
        profile = self.shape(band_l, band_r)

        if self._fast is None:
            self._fast = profile.copy()
            self._slow = profile.copy()
            return

        # Two averages, not an instantaneous reading against one average.
        # Frame-to-frame variation inside a single track is comfortably
        # larger than the difference between two tracks, so comparing a
        # 1.5 s view against a 25 s one is what separates a real change
        # from the noise.  Comparing the raw profile against a slow
        # average fired twenty times a track.
        a_fast = 1.0 - float(np.exp(-dt / self.tau_fast))
        a_slow = 1.0 - float(np.exp(-dt / self.tau_slow))
        self._fast += (profile - self._fast) * a_fast
        self._slow += (profile - self._slow) * a_slow

        dist = float(np.abs(self._fast - self._slow).sum())

        # Rising edge with hysteresis.  Without it one change arms
        # repeatedly for as long as the two averages take to reconverge.
        if dist > self.shift_threshold and not self._above:
            self._armed = True
            self._above = True
        elif dist < self.shift_threshold * 0.55:
            self._above = False

    def should_identify(self) -> bool:
        if not self._armed:
            return False
        if self._t - self._last_fire < self.min_interval:
            return False
        self._armed = False
        self._last_fire = self._t
        return True

    def force(self) -> None:
        """Someone pressed the identify button."""
        self._armed = True
        self._last_fire = -1e9


# -------------------------------------------------------------- cache


class MatchCache:
    """Remembers what a piece of audio turned out to be.

    Keyed on a coarse hash of the spectral profile, so the same track
    coming round again on the same record is answered locally.  This is
    what makes even a paid provider affordable: a listener plays the same
    albums.
    """

    def __init__(self, path: str, ttl_days: float = 120.0) -> None:
        self.path = path
        self.ttl = ttl_days * 86400
        self._data: dict[str, dict] = {}
        self.hits = 0
        self.misses = 0
        self._load()

    @staticmethod
    def key_for(band_l: list[int], band_r: list[int]) -> str:
        """A key that survives a volume change.

        Same reasoning as :meth:`TrackWatcher.shape`: the bands are in dB,
        so the level is an offset and normalising by the sum leaves it
        firmly in place.  Take the shape relative to the loudest band,
        then quantise hard - this has to collide with itself next time
        the record comes round, from a different point in the track.
        """
        shape = TrackWatcher.shape(band_l, band_r)       # -1 .. 0
        coarse = bytes(int(np.clip((1.0 + v) * 7.99, 0, 7)) for v in shape)
        return hashlib.sha1(coarse).hexdigest()[:16]

    def get(self, key: str) -> Match | None:
        row = self._data.get(key)
        if not row or time.time() - row.get("t", 0) > self.ttl:
            self.misses += 1
            return None
        self.hits += 1
        m = dict(row)
        m.pop("t", None)
        return Match(**m)

    def put(self, key: str, match: Match) -> None:
        row = asdict(match)
        row["t"] = time.time()
        self._data[key] = row
        self._save()

    def _load(self) -> None:
        try:
            with open(self.path, encoding="utf-8") as fh:
                self._data = json.load(fh)
        except (OSError, ValueError):
            self._data = {}

    def _save(self) -> None:
        try:
            os.makedirs(os.path.dirname(self.path) or ".", exist_ok=True)
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(self._data, fh)
            os.replace(tmp, self.path)
        except OSError as exc:
            log.warning("could not save the match cache: %s", exc)


# --------------------------------------------------------------- wav


def _wav16k(pcm16: bytes) -> bytes:
    """Wrap raw PCM in a WAV header.  Every provider takes a file."""
    import struct
    n = len(pcm16)
    return (b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt "
            + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
            + b"data" + struct.pack("<I", n) + pcm16)


def make_provider(name: str, **keys) -> Provider:
    name = (name or "off").lower()
    if name == "shazam":
        return ShazamProvider()
    if name == "acoustid":
        return AcoustIDProvider(keys.get("acoustid_key", ""))
    if name == "audd":
        return AudDProvider(keys.get("audd_token", ""))
    return NullProvider()
