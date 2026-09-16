"""Where "now playing" comes from.

Fingerprinting is the *last* resort, not the first.  It costs a network
round trip, it guesses, and it can be wrong.  Everything else here is
exact and free.

The digital input is easy: MPD knows precisely what it is playing.  The
analogue input is only hard when whatever is plugged into it is dumb — a
turntable, a tuner, a tape deck.  If it is a WiiM, a Sonos, a Chromecast
or a phone over Bluetooth, that device already knows the title and will
tell you over the network, and asking it beats listening to it every
single time.

So sources are tried in order of how much they know, and the fingerprinter
only runs when nothing else can answer.
"""

from __future__ import annotations

import asyncio
import logging
import re
import time
from dataclasses import dataclass, field

log = logging.getLogger("ampd.sources")


@dataclass
class NowPlaying:
    artist: str = ""
    title: str = ""
    album: str = ""
    art_url: str = ""
    playing: bool = False
    #: Seconds in, and total, when the source knows them.
    elapsed: float = 0.0
    duration: float = 0.0
    #: Which source answered, for the web UI and the log.
    origin: str = ""
    #: True when the source knows rather than guesses.  The panel shows a
    #: guessed title differently — a wrong artist stated confidently is
    #: worse than an honest question mark.
    exact: bool = True
    at: float = field(default_factory=time.monotonic)

    @property
    def named(self) -> bool:
        return bool(self.title or self.artist)


class MetadataSource:
    """Something that might know what is playing.

    ``poll`` must never raise and never block for long: a streamer that
    has been unplugged should mean no title, not a stalled panel.
    """

    name = "none"
    #: Higher wins when more than one source has an answer.
    priority = 0
    #: Does this source apply to the analogue input, the digital one, or
    #: both?  ``None`` means both.
    for_input: int | None = None

    async def poll(self) -> NowPlaying | None:
        return None


# ------------------------------------------------------------- linkplay


_HEX_RE = re.compile(r"^(?:[0-9a-fA-F]{2})+$")


def unhex(value: str) -> str:
    """LinkPlay hex-encodes Title, Artist and Album.  Sometimes.

    Firmware versions disagree, so this has to detect rather than assume,
    and detection has traps.  "Cafe", "abba", "BEEF" and "decade" are all
    valid hex, and ABBA is a band.  Most of those decode to invalid UTF-8
    and fall out on their own, but "dead" decodes to a perfectly valid
    Arabic letter, so a UTF-8 check alone is not enough.

    The rule that works: accept a decode that is plain printable ASCII at
    any length, and accept a decode containing anything else only when the
    input is long enough that it cannot plausibly be a word.  That keeps
    "736865" -> "she", keeps a Japanese title, and leaves "dead" alone.
    """
    if not value or not _HEX_RE.match(value):
        return value
    try:
        text = bytes.fromhex(value).decode("utf-8")
    except (ValueError, UnicodeDecodeError):
        return value
    if not text or any(ord(c) < 0x20 or ord(c) == 0x7F for c in text):
        return value                       # control characters: not a title

    if all(0x20 <= ord(c) < 0x7F for c in text):
        return text                        # plain ASCII, unambiguous enough

    # Non-Latin output is only believable from an input too long to be a
    # word that happens to be hex.  Twelve characters is six bytes.
    return text if len(value) >= 12 else value


class LinkPlaySource(MetadataSource):
    """A WiiM or other LinkPlay streamer feeding the analogue input.

    Exact, free, instant, and it works for material no fingerprinter would
    ever get — internet radio idents, podcasts, a friend's playlist.  If
    you have one of these on the analogue socket there is no reason to
    fingerprint anything.

    Newer firmware serves the API over HTTPS with a self-signed
    certificate, older over plain HTTP, so both are tried and the one
    that answers is remembered.
    """

    name = "linkplay"
    priority = 50
    for_input = 1                      # IN_ANALOG

    #: LinkPlay 'mode' values that mean it is passing something else
    #: through rather than playing.  Its own metadata is stale then.
    PASSTHROUGH_MODES = {40, 41, 43, 99}

    def __init__(self, host: str, timeout: float = 2.0,
                 poll_interval: float = 2.0) -> None:
        self.host = host
        self.timeout = timeout
        self.poll_interval = poll_interval
        self._scheme: str | None = None
        self._fail_until = 0.0
        self._next = 0.0
        self._cached: NowPlaying | None = None
        self.polls = 0

    async def poll(self) -> NowPlaying | None:
        if not self.host or time.monotonic() < self._fail_until:
            return None
        # Rate limited here rather than by the caller, so anything may ask
        # as often as it likes without turning into network traffic.  A
        # WiiM's metadata does not change faster than this.
        if time.monotonic() < self._next:
            return self._cached
        self._next = time.monotonic() + self.poll_interval
        self.polls += 1
        try:
            status = await self._get("getPlayerStatus")
            if not status:
                return None

            if str(status.get("status", "")).lower() not in ("play", "load"):
                self._cached = None
                return None
            try:
                if int(status.get("mode", 0)) in self.PASSTHROUGH_MODES:
                    # It is being used as a switch, not a player.  Let the
                    # fingerprinter have it.
                    self._cached = None
                    return None
            except (TypeError, ValueError):
                pass

            np = NowPlaying(
                artist=unhex(status.get("Artist", "")),
                title=unhex(status.get("Title", "")),
                album=unhex(status.get("Album", "")),
                playing=True,
                elapsed=_ms(status.get("curpos")),
                duration=_ms(status.get("totlen")),
                origin=self.name,
                exact=True,
            )
            meta = await self._get("getMetaInfo")
            if meta:
                m = meta.get("metaData") or {}
                np.art_url = m.get("albumArtURI") or m.get("albumArtUri") or ""
                np.artist = np.artist or m.get("artist", "")
                np.title = np.title or m.get("title", "")
                np.album = np.album or m.get("album", "")
            self._cached = np if np.named else None
            return self._cached

        except Exception as exc:                        # noqa: BLE001
            log.debug("linkplay poll failed: %s", exc)
            # Back off rather than hammer a device that is not there.
            self._fail_until = time.monotonic() + 30.0
            self._cached = None
            return None

    async def _get(self, command: str) -> dict | None:
        import aiohttp                                  # noqa: PLC0415
        schemes = [self._scheme] if self._scheme else ["https", "http"]
        timeout = aiohttp.ClientTimeout(total=self.timeout)
        for scheme in schemes:
            url = f"{scheme}://{self.host}/httpapi.asp?command={command}"
            try:
                connector = aiohttp.TCPConnector(ssl=False)   # self-signed
                async with aiohttp.ClientSession(timeout=timeout,
                                                 connector=connector) as s:
                    async with s.get(url) as r:
                        if r.status != 200:
                            continue
                        # It answers with the wrong content type.
                        data = await r.json(content_type=None)
                        self._scheme = scheme
                        return data
            except Exception:                            # noqa: BLE001
                continue
        return None


def _ms(value) -> float:
    try:
        return float(value) / 1000.0
    except (TypeError, ValueError):
        return 0.0


# ------------------------------------------------------------------ mpd


class MpdSource(MetadataSource):
    """moOde's MPD.  The digital input, and there is nothing to improve on."""

    name = "mpd"
    priority = 100
    for_input = 0                      # IN_DIGITAL

    def __init__(self, host: str = "127.0.0.1", port: int = 6600,
                 poll_interval: float = 0.5) -> None:
        self.host = host
        self.port = port
        self.poll_interval = poll_interval
        self._client = None
        self._next = 0.0
        self._cached: NowPlaying | None = None
        self.polls = 0

    async def poll(self) -> NowPlaying | None:
        if time.monotonic() < self._next:
            return self._cached
        self._next = time.monotonic() + self.poll_interval
        self.polls += 1
        try:
            client = await self._connect()
            if client is None:
                return None
            loop = asyncio.get_running_loop()
            status, song = await loop.run_in_executor(None, self._read, client)
        except Exception as exc:                        # noqa: BLE001
            log.debug("mpd poll failed: %s", exc)
            self._client = None
            return None

        if not song:
            self._cached = None
            return None
        self._cached = NowPlaying(
            artist=song.get("artist", ""),
            title=song.get("title", "") or song.get("name", ""),
            album=song.get("album", ""),
            playing=status.get("state") == "play",
            elapsed=float(status.get("elapsed", 0) or 0),
            duration=float(status.get("duration", 0) or 0),
            origin=self.name,
            exact=True,
        )
        return self._cached

    @staticmethod
    def _read(client):
        return client.status(), client.currentsong()

    async def _connect(self):
        if self._client is not None:
            return self._client
        from mpd import MPDClient                        # noqa: PLC0415
        client = MPDClient()
        client.timeout = 3
        await asyncio.get_running_loop().run_in_executor(
            None, client.connect, self.host, self.port)
        self._client = client
        return client


# ---------------------------------------------------------- fingerprint


class FingerprintSource(MetadataSource):
    """Listening to the audio, because nothing else can say.

    Lowest priority on purpose.  This is for a turntable, a tuner, a tape
    deck — anything with no idea what it is playing and no way to be
    asked.
    """

    name = "fingerprint"
    priority = 10
    for_input = 1                      # IN_ANALOG

    def __init__(self, provider, watcher, cache) -> None:
        self.provider = provider
        self.watcher = watcher
        self.cache = cache
        self._current: NowPlaying | None = None
        self.lookups = 0

    def feed_bands(self, band_l, band_r, dt=None) -> None:
        self.watcher.feed(band_l, band_r, dt=dt)
        self._last_bands = (band_l, band_r)

    async def maybe_identify(self, pcm_getter) -> NowPlaying | None:
        """Called from the service loop.  ``pcm_getter`` is an awaitable
        returning 16 kHz mono PCM of the requested length."""
        if not self.watcher.should_identify():
            return None

        bands = getattr(self, "_last_bands", None)
        if bands:
            key = self.cache.key_for(*bands)
            hit = self.cache.get(key)
            if hit:
                self._current = NowPlaying(
                    artist=hit.artist, title=hit.title, album=hit.album,
                    art_url=hit.art_url, playing=True,
                    origin=f"{self.name}/cache", exact=False)
                return self._current
        else:
            key = None

        pcm = await pcm_getter(self.provider.want_seconds)
        if not pcm:
            return None

        self.lookups += 1
        match = await self.provider.identify(pcm)
        if match is None:
            return None
        if key:
            self.cache.put(key, match)
        self._current = NowPlaying(
            artist=match.artist, title=match.title, album=match.album,
            art_url=match.art_url, playing=True,
            origin=f"{self.name}/{match.provider}", exact=False)
        return self._current

    async def poll(self) -> NowPlaying | None:
        return self._current


# -------------------------------------------------------------- resolve


class Resolver:
    """Picks the best answer available for the current input.

    Order is by priority, not by who answered last: an exact title from a
    streamer must never be displaced by a fingerprint guess that happens
    to arrive afterwards.
    """

    def __init__(self, sources: list[MetadataSource], stale_after: float = 25.0):
        self.sources = sorted(sources, key=lambda s: -s.priority)
        self.stale_after = stale_after

    def applicable(self, input_sel: int) -> list[MetadataSource]:
        return [s for s in self.sources
                if s.for_input is None or s.for_input == input_sel]

    async def resolve(self, input_sel: int) -> NowPlaying | None:
        for src in self.applicable(input_sel):
            try:
                np = await src.poll()
            except Exception:                            # noqa: BLE001
                log.exception("source %s failed", src.name)
                continue
            if np and np.named:
                if time.monotonic() - np.at > self.stale_after and not np.playing:
                    continue
                return np
        return None

    @property
    def needs_audio_stream(self) -> bool:
        """Only true when a fingerprint source is the best we have.

        This is what decides whether the analogue audio stream — 52% of
        the UART — is worth turning on for metadata.  With a WiiM on the
        analogue input it never is.
        """
        fp = [s for s in self.sources if isinstance(s, FingerprintSource)]
        if not fp:
            return False
        better = [s for s in self.sources
                  if s.for_input == 1 and s.priority > fp[0].priority]
        return not any(getattr(s, "_scheme", None) for s in better)
