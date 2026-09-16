"""The UART link to the Pico.

921600 8N1 through the isolator, on the Pi's PL011 — see the note about
``dtoverlay=miniuart-bt`` in ``pico/README.md``; the mini-UART's baud rate
is derived from the VPU core clock and drifts when it clocks down, which
at this speed shows up as occasional framing errors rather than an
obvious failure.

The Pico is authoritative.  Nothing here caches a decision, and nothing
here is allowed to matter: pull this cable and the encoder, the IR remote
and the front-panel button still work.  So the link reconnects quietly,
forever, and never blocks anything else.
"""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Awaitable, Callable

from . import proto

log = logging.getLogger("ampd.link")

Handler = Callable[[proto.Frame], None | Awaitable[None]]


class Link:
    def __init__(self, device: str = "/dev/ttyAMA0", baud: int = 921600) -> None:
        self.device = device
        self.baud = baud
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._decoder = proto.Decoder()
        self._handlers: dict[int, list[Handler]] = {}
        self._last_rx = 0.0
        self._task: asyncio.Task | None = None

        # Diagnostics worth having when something is wrong at 921600.
        self.frames_rx = 0
        self.bytes_rx = 0
        self.reconnects = 0
        self.audio_gaps = 0
        self._audio_seq: int | None = None

    # ---------------------------------------------------------- wiring

    def on(self, msg_type: int, fn: Handler) -> None:
        self._handlers.setdefault(int(msg_type), []).append(fn)

    @property
    def alive(self) -> bool:
        """Heard anything in the last five seconds.

        The Pico sends state on change and meter frames continuously
        while it is on, but in standby it goes quiet on purpose, so this
        being False is not by itself a fault.
        """
        return (time.monotonic() - self._last_rx) < 5.0

    # ------------------------------------------------------------- run

    async def start(self) -> None:
        self._task = asyncio.create_task(self._run(), name="ampd-link")

    async def stop(self) -> None:
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        self._close()

    def _close(self) -> None:
        if self._writer:
            try:
                self._writer.close()
            except Exception:
                pass
        self._reader = self._writer = None

    async def _run(self) -> None:
        backoff = 0.5
        while True:
            try:
                # Imported here rather than at module scope: every
                # other part of this package is testable without a
                # serial port attached, and pyserial only exists on
                # the Pi.
                import serial_asyncio                # noqa: PLC0415
                self._reader, self._writer = \
                    await serial_asyncio.open_serial_connection(
                        url=self.device, baudrate=self.baud)
                log.info("link up on %s at %d", self.device, self.baud)
                backoff = 0.5
                self._decoder = proto.Decoder()
                await self.send(proto.build(proto.Msg.HELLO))
                await self._pump()
            except asyncio.CancelledError:
                raise
            except Exception as exc:                    # noqa: BLE001
                log.warning("link error: %s", exc)
            self._close()
            self.reconnects += 1
            await asyncio.sleep(backoff)
            backoff = min(5.0, backoff * 2)

    async def _pump(self) -> None:
        assert self._reader is not None
        while True:
            # A big read: at 921600 with the audio stream on, 48 kB/s is
            # arriving and per-byte work in Python would not keep up.
            data = await self._reader.read(4096)
            if not data:
                raise ConnectionError("serial closed")
            self.bytes_rx += len(data)
            self._last_rx = time.monotonic()
            for frame in self._decoder.feed(data):
                self.frames_rx += 1
                await self._dispatch(frame)

    async def _dispatch(self, frame: proto.Frame) -> None:
        if frame.type == proto.Msg.AUDIO:
            self._check_audio_seq(frame.payload)
        for fn in self._handlers.get(frame.type, ()):
            try:
                r = fn(frame)
                if asyncio.iscoroutine(r):
                    await r
            except Exception:                            # noqa: BLE001
                log.exception("handler for 0x%02X failed", frame.type)

    def _check_audio_seq(self, payload: bytes) -> None:
        """A dropped audio frame is a click, not a crash, so count it and
        carry on — but count it, because a rising number means the Pi is
        not draining the link fast enough and that is worth knowing."""
        if not payload:
            return
        seq = payload[0]
        if self._audio_seq is not None and seq != (self._audio_seq + 1) & 0xFF:
            self.audio_gaps += 1
        self._audio_seq = seq

    # ------------------------------------------------------------ send

    async def send(self, raw: bytes) -> None:
        w = self._writer
        if w is None:
            return                      # link down; the Pico is fine without us
        w.write(raw)
        await w.drain()

    async def cmd(self, cmd: int, value: int = 0) -> None:
        await self.send(proto.build_cmd(cmd, value))

    async def menu_key(self, key: int) -> None:
        await self.send(proto.build(proto.Msg.MENU_KEY, bytes([key])))

    async def nowplaying(self, playing: bool, art_id: int,
                         artist: str, title: str, album: str) -> None:
        """Relayed by the Pico straight to the ESP32 for the knob."""
        await self.send(proto.build(
            proto.Msg.NOWPLAYING,
            proto.encode_nowplaying(playing, art_id, artist, title, album)))

    async def audio_stream(self, on: bool, stereo: bool = True,
                           seconds: float = 0.0) -> None:
        """Turn the analogue audio stream on or off.

        ``seconds`` makes it a timed capture, which is what a fingerprint
        wants — and means a crash here cannot leave 52% of the link
        running forever.
        """
        await self.cmd(proto.Cmd.AUDIO_MODE, 1 if stereo else 0)
        if not on:
            await self.cmd(proto.Cmd.AUDIO_STREAM, 0)
            return
        ticks = max(2, min(32767, round(seconds * 10))) if seconds else 1
        await self.cmd(proto.Cmd.AUDIO_STREAM, ticks)

    def stats(self) -> dict:
        return {
            "device": self.device,
            "baud": self.baud,
            "alive": self.alive,
            "frames_rx": self.frames_rx,
            "bytes_rx": self.bytes_rx,
            "crc_errors": self._decoder.crc_errors,
            "audio_gaps": self.audio_gaps,
            "reconnects": self.reconnects,
        }
