"""The service loop: what runs, when, and — mostly — when not.

Three policies live here and they are the whole point of the file.

**Metadata comes from the cheapest source that knows.**  Digital is
moOde's MPD and there is nothing to improve on.  Analogue asks a WiiM if
there is one, because it knows exactly and costs a LAN request.  Only if
neither can answer does anything listen to the audio.

**Fingerprinting happens at the start of music, not on a timer.**  The
watcher arms on a gap or a spectral change; the rate limiter caps it
regardless; the cache answers the second time a record comes round.  Two
lookups a track, measured.

**The audio stream is off unless something needs it.**  It is 52% of the
UART.  It comes on for a timed capture when a fingerprint is due, or
continuously only while a waveform visualiser is on screen *and* the
analogue input is selected.  On digital it never comes on at all: the Pi
already has the audio.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import time
from dataclasses import dataclass, field

from . import proto
from .audio import AudioRing, Analyser, to_16k_mono_pcm
from .config import Config
from .fingerprint import MatchCache, TrackWatcher, make_provider
from .link import Link
from .sources import (FingerprintSource, LinkPlaySource, MpdSource,
                      NowPlaying, Resolver)
from .visualizers import Needs, Registry

log = logging.getLogger("ampd.service")


@dataclass
class State:
    amp: proto.AmpState = field(default_factory=proto.AmpState)
    meter: proto.Meter = field(default_factory=proto.Meter)
    now: NowPlaying = field(default_factory=NowPlaying)
    #: Bumped whenever the cover changes, so the knob knows to re-fetch.
    art_id: int = 0
    audio_streaming: bool = False
    menu: dict | None = None

    def to_json(self) -> dict:
        a = self.amp
        return {
            "power": a.power,
            "volume_db": round(a.volume_db, 1),
            "vol_code": a.vol_code,
            "max_vol": a.max_vol,
            "muted": a.muted,
            "input": a.input,
            "eq_path": a.eq_path,
            "headroom_db": a.headroom_db,
            "now": {
                "artist": self.now.artist,
                "title": self.now.title,
                "album": self.now.album,
                "playing": self.now.playing,
                "elapsed": round(self.now.elapsed, 1),
                "duration": round(self.now.duration, 1),
                "origin": self.now.origin,
                "exact": self.now.exact,
                "art_id": self.art_id,
            },
            "audio_streaming": self.audio_streaming,
        }


class Service:
    def __init__(self, cfg: Config | None = None) -> None:
        self.cfg = cfg or Config.load()
        self.state = State()

        self.link = Link(self.cfg.serial_device, self.cfg.serial_baud)
        self.ring = AudioRing()
        self.analyser = Analyser()
        self.registry = Registry(self.cfg.visualizers_path)

        self.watcher = TrackWatcher(
            min_interval=self.cfg.fingerprint_min_interval_s)
        self.cache = MatchCache(self.cfg.match_cache_path)
        self.provider = make_provider(
            self.cfg.fingerprint_provider,
            acoustid_key=self.cfg.acoustid_key,
            audd_token=self.cfg.audd_token)

        self.mpd = MpdSource(self.cfg.mpd_host, self.cfg.mpd_port)
        self.linkplay = (LinkPlaySource(self.cfg.linkplay_host,
                                        poll_interval=self.cfg.linkplay_poll_s)
                         if self.cfg.linkplay_host else None)
        self.fingerprint = FingerprintSource(
            self.provider, self.watcher, self.cache)

        sources = [self.mpd, self.fingerprint]
        if self.linkplay:
            sources.insert(1, self.linkplay)
        self.resolver = Resolver(sources)

        self._subscribers: set[asyncio.Queue] = set()
        self._tasks: list[asyncio.Task] = []
        self._capture_until = 0.0
        self._last_meter_t = 0.0

        self.link.on(proto.Msg.STATE, self._on_state)
        self.link.on(proto.Msg.METER, self._on_meter)
        self.link.on(proto.Msg.AUDIO, self._on_audio)
        self.link.on(proto.Msg.EVENT, self._on_event)

    # ---------------------------------------------------------- frames

    def _on_state(self, frame: proto.Frame) -> None:
        try:
            before = self.state.amp.input
            self.state.amp = proto.AmpState.parse(frame.payload)
        except ValueError as exc:
            log.warning("bad MSG_STATE: %s", exc)
            return
        if self.state.amp.input != before:
            # The input moved. Whatever we knew about the old one is not
            # true of the new one.
            self.state.now = NowPlaying()
            self.watcher.force()
        self._publish()

    def _on_meter(self, frame: proto.Frame) -> None:
        try:
            self.state.meter = proto.Meter.parse(frame.payload)
        except ValueError:
            return

        now = time.monotonic()
        dt = min(1.0, now - self._last_meter_t) if self._last_meter_t else 0.02
        self._last_meter_t = now

        # Only the analogue input needs watching. On digital, MPD tells us
        # when a track changes and there is nothing to guess.
        if self.state.amp.input == proto.IN_ANALOG and self.state.amp.on:
            self.fingerprint.feed_bands(
                self.state.meter.band_l, self.state.meter.band_r, dt=dt)

    def _on_audio(self, frame: proto.Frame) -> None:
        try:
            self.ring.feed(proto.AudioChunk.parse(frame.payload))
        except ValueError:
            pass

    def _on_event(self, frame: proto.Frame) -> None:
        """Kind 1 is a transport key from the IR remote or the knob.

        The Pico forwards these without acting on them - it has no idea
        what is playing and should not pretend to.
        """
        p = frame.payload
        if len(p) < 3 or p[0] != 1:
            return
        fn = p[1]
        asyncio.create_task(self._transport(fn))

    async def _transport(self, fn: int) -> None:
        if self.state.amp.input != proto.IN_DIGITAL:
            return                      # nothing to control on analogue
        try:
            client = await self.mpd._connect()
            if client is None:
                return
            loop = asyncio.get_running_loop()
            if fn == proto.Fn.PREV:
                await loop.run_in_executor(None, client.previous)
            elif fn == proto.Fn.NEXT:
                await loop.run_in_executor(None, client.next)
            elif fn == proto.Fn.PLAY_PAUSE:
                await loop.run_in_executor(None, client.pause)
        except Exception as exc:                        # noqa: BLE001
            log.warning("transport %s failed: %s", fn, exc)

    # ------------------------------------------------------ audio policy

    def _wave_mode_active(self) -> bool:
        """Is a visualiser on screen that needs time-domain samples?"""
        return any(Needs.WAVE in v.needs for v in self.registry.enabled())

    def _want_stream(self) -> tuple[bool, bool]:
        """(on, stereo) — the whole audio-stream policy in one place."""
        amp = self.state.amp
        if not amp.on or amp.input != proto.IN_ANALOG:
            # Digital already gives us the audio, and a sleeping amp has
            # nothing to send.
            return False, False

        if time.monotonic() < self._capture_until:
            # A fingerprint capture is running. Mono is all it needs and
            # it halves the load, unless a wave mode wants stereo anyway.
            return True, self._wave_mode_active()

        return (self._wave_mode_active(), True)

    async def _apply_stream_policy(self) -> None:
        on, stereo = self._want_stream()
        if on == self.state.audio_streaming:
            return
        self.state.audio_streaming = on
        await self.link.audio_stream(on, stereo=stereo)
        log.info("analogue audio stream %s%s",
                 "on" if on else "off", " (stereo)" if on and stereo else "")

    async def _capture(self, seconds: float) -> bytes:
        """Ask the Pico for a timed burst and hand back 16 kHz mono PCM."""
        seconds = max(4.0, min(20.0, seconds))
        self._capture_until = time.monotonic() + seconds + 1.0
        await self._apply_stream_policy()
        # Timed on the Pico as well, belt and braces: if this process dies
        # mid-capture the stream still stops on its own.
        await self.link.audio_stream(True, stereo=self._wave_mode_active(),
                                     seconds=seconds + 1.0)
        await asyncio.sleep(seconds)
        pcm = to_16k_mono_pcm(self.ring.mono_seconds(seconds))
        self._capture_until = 0.0
        await self._apply_stream_policy()
        return pcm

    # ----------------------------------------------------------- loops

    async def _metadata_loop(self) -> None:
        """Ask whoever knows, at a sensible rate, and only when it matters."""
        while True:
            try:
                await asyncio.sleep(1.0)
                if not self.state.amp.on:
                    continue

                # Each source rate-limits its own network traffic, so
                # this can ask on every pass without turning into a poll
                # storm at the WiiM.
                now = await self.resolver.resolve(self.state.amp.input)
                if now and self._changed(now):
                    self.state.now = now
                    self.state.art_id = (self.state.art_id + 1) & 0xFF or 1
                    await self._push_nowplaying()
                    self._publish()
            except asyncio.CancelledError:
                raise
            except Exception:                            # noqa: BLE001
                log.exception("metadata loop")

    def _changed(self, now: NowPlaying) -> bool:
        old = self.state.now
        return (now.title, now.artist) != (old.title, old.artist)

    async def _fingerprint_loop(self) -> None:
        """Only ever runs at the start of music on the analogue input."""
        while True:
            try:
                await asyncio.sleep(0.5)
                amp = self.state.amp
                if not amp.on or amp.input != proto.IN_ANALOG:
                    continue
                # A streamer that knows beats listening, every time.
                if self.linkplay and await self.linkplay.poll():
                    continue
                await self.fingerprint.maybe_identify(self._capture)
            except asyncio.CancelledError:
                raise
            except Exception:                            # noqa: BLE001
                log.exception("fingerprint loop")

    async def _policy_loop(self) -> None:
        while True:
            try:
                await asyncio.sleep(1.0)
                await self._apply_stream_policy()
            except asyncio.CancelledError:
                raise
            except Exception:                            # noqa: BLE001
                log.exception("policy loop")

    async def _push_nowplaying(self) -> None:
        n = self.state.now
        await self.link.nowplaying(n.playing, self.state.art_id,
                                   n.artist, n.title, n.album)

    # ------------------------------------------------------- subscribers

    def subscribe(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(maxsize=8)
        self._subscribers.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self._subscribers.discard(q)

    def _publish(self) -> None:
        msg = self.state.to_json()
        for q in list(self._subscribers):
            try:
                q.put_nowait(msg)
            except asyncio.QueueFull:
                # A panel that cannot keep up gets the next one instead of
                # holding up the service.
                pass

    # ------------------------------------------------------------- run

    async def start(self) -> None:
        await self.link.start()
        for coro in (self._metadata_loop, self._fingerprint_loop,
                     self._policy_loop):
            self._tasks.append(asyncio.create_task(coro()))
        log.info("service up: provider=%s linkplay=%s",
                 self.provider.name, self.cfg.linkplay_host or "none")

    async def stop(self) -> None:
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            with contextlib.suppress(asyncio.CancelledError):
                await t
        self._tasks.clear()
        with contextlib.suppress(Exception):
            await self.link.audio_stream(False)
        await self.link.stop()
