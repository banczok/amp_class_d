"""The analogue audio stream: ring buffer, spectra, fingerprint captures.

The Pico sends the analogue input as 24 kHz stereo G.711 mu-law over the
UART.  This is where it lands.  Three things read from it:

  * the waveform visualisers, which want the last few hundred milliseconds
  * the spectrum visualisers, when a finer picture than the Pico's 32
    bands is wanted
  * the fingerprinter, which wants five seconds of mono

None of them should ever block the link, so writing is a memcpy into a
ring and everything else works on a copy.
"""

from __future__ import annotations

import logging
import threading
import time

import numpy as np

from . import proto

log = logging.getLogger("ampd.audio")

RATE = proto.AUDIO_RATE_HZ          # 24000
RING_SECONDS = 12.0                 # enough for a 5 s capture plus slack

#: The mu-law byte -> int16 table, as a numpy lookup.
_ULAW = np.frombuffer(proto.ULAW_TABLE, dtype="<i2").astype(np.float32) / 32768.0

# The Pico's band byte, derived rather than guessed, so the two feeds are
# interchangeable and a visualiser never has to ask which one it is on.
#
# meter.c emits (MSB index << 3) | 3 mantissa bits of the band's peak
# power, which is 8 units per doubling of power - 8/6.0206 dB in
# amplitude terms, doubled because power is amplitude squared.  Its
# transform runs on Q15 samples scaled 12->16 bits with a halve-per-stage
# FFT, which puts a full-scale sine at 8*log2((32752 * N/4) / 1024)^2
# = 208.
DB_PER_UNIT = 16.0 / 6.020599913        # 2.657 byte units per dB
FULL_SCALE_BYTE = 208.0                 # where 0 dBFS lands


class AudioRing:
    """A lock-protected stereo ring.

    A lock rather than something clever: writes are one memcpy at 100 Hz
    and reads are a handful per second, so contention is not real, and a
    ring that is obviously correct beats one that is nearly lock-free.
    """

    def __init__(self, seconds: float = RING_SECONDS) -> None:
        self.n = int(RATE * seconds)
        self._buf = np.zeros((2, self.n), dtype=np.float32)
        self._w = 0
        self._total = 0
        self._lock = threading.Lock()
        self.last_rx = 0.0
        self.stereo = True

    # ------------------------------------------------------------ write

    def feed(self, chunk: proto.AudioChunk) -> None:
        if not chunk.ulaw:
            return
        samples = _ULAW[np.frombuffer(chunk.ulaw, dtype=np.uint8)]

        if chunk.stereo:
            if samples.size & 1:
                # Should not happen - the Pico guarantees whole pairs -
                # but a truncated frame must not swap the channels for
                # everything after it.
                samples = samples[:-1]
                log.warning("odd stereo frame, trimming")
            left = samples[0::2]
            right = samples[1::2]
        else:
            left = right = samples

        self._write(left, right)
        self.stereo = chunk.stereo
        self.last_rx = time.monotonic()

    def _write(self, left: np.ndarray, right: np.ndarray) -> None:
        k = left.size
        if k > self.n:                       # absurdly large frame
            left, right, k = left[-self.n:], right[-self.n:], self.n
        with self._lock:
            w = self._w
            end = w + k
            if end <= self.n:
                self._buf[0, w:end] = left
                self._buf[1, w:end] = right
            else:
                cut = self.n - w
                self._buf[0, w:] = left[:cut]
                self._buf[1, w:] = right[:cut]
                self._buf[0, :end - self.n] = left[cut:]
                self._buf[1, :end - self.n] = right[cut:]
            self._w = end % self.n
            self._total += k

    # ------------------------------------------------------------- read

    @property
    def live(self) -> bool:
        return (time.monotonic() - self.last_rx) < 1.0

    def latest(self, samples: int) -> np.ndarray:
        """The most recent ``samples`` frames, shape (2, samples)."""
        k = min(samples, self.n)
        with self._lock:
            w = self._w
            if k <= w:
                return self._buf[:, w - k:w].copy()
            head = self._buf[:, self.n - (k - w):].copy()
            tail = self._buf[:, :w].copy()
            return np.concatenate((head, tail), axis=1)

    def mono_seconds(self, seconds: float) -> np.ndarray:
        a = self.latest(int(RATE * seconds))
        return (a[0] + a[1]) * 0.5


# ------------------------------------------------------------ analysis


class Analyser:
    """Turns the ring into what a visualiser actually draws.

    Kept separate from the ring so the digital path can feed the same
    interface from the Pi's own audio without a UART anywhere in sight.
    """

    def __init__(self, bands: int = proto.METER_BANDS,
                 fft_size: int = 2048,
                 f_lo: float = 40.0, f_hi: float = 11_000.0) -> None:
        self.bands = bands
        self.n = fft_size
        self._window = np.hanning(fft_size).astype(np.float32)

        # Log-spaced edges.  f_hi stops below the 12 kHz Nyquist of the
        # decimated stream, where the half-band filter is already rolling
        # off - a band drawn there would show the filter, not the music.
        edges = np.geomspace(f_lo, f_hi, bands + 1)
        bins = np.clip(np.round(edges * fft_size / RATE).astype(int),
                       1, fft_size // 2 - 1)

        # Adjacent low bands are allowed to land on the same bin.  Forcing
        # each band onto a distinct one instead - the obvious fix for an
        # empty band - cascades: the bottom band is 3 Hz wide against an
        # 11 Hz bin, so every band below about 500 Hz gets pushed up one,
        # and the whole frequency axis shifts.  This is what the Pico
        # does too, and it keeps the mapping honest.
        self._bins = bins

        # The edges the bins actually represent, not the ones asked for.
        # A bar labelled 40 Hz that is really reading bin 1 is a lie the
        # UI would faithfully repeat.
        self.edges_hz = bins * (RATE / fft_size)

    def spectrum(self, ring: AudioRing) -> tuple[list[int], list[int]]:
        """Per-channel band levels, 0..255, on the Pico's own scale."""
        block = ring.latest(self.n)
        out = []
        for ch in (0, 1):
            # Normalise to amplitude first.  A Hann window sums to N/2 and
            # a sine puts half its energy in each of two bins, so the peak
            # bin of a full-scale sine is A.N/4 - without dividing that
            # out, 0 dBFS reads +52 dB and everything saturates.
            amp = np.abs(np.fft.rfft(block[ch] * self._window)) * (4.0 / self.n)
            db = 20.0 * np.log10(amp + 1e-9)
            vals = []
            for b in range(self.bands):
                lo = int(self._bins[b])
                hi = max(int(self._bins[b + 1]), lo + 1)
                peak = float(db[lo:hi].max())
                vals.append(int(np.clip(peak * DB_PER_UNIT + FULL_SCALE_BYTE,
                                        0, 255)))
            out.append(vals)
        return out[0], out[1]

    @staticmethod
    def envelope(ring: AudioRing, points: int, seconds: float = 0.05
                 ) -> tuple[list[float], list[float]]:
        """Min/max reduced traces for the scope and filled-waveform modes.

        Reducing here rather than shipping raw samples matters: 50 ms of
        stereo at 24 kHz is 2400 floats, and the panel only has 1280
        pixels to put them in.
        """
        block = ring.latest(int(RATE * seconds))
        k = block.shape[1] // points or 1
        usable = k * points
        out = []
        for ch in (0, 1):
            v = block[ch, -usable:].reshape(points, k)
            # Peak of each cell, signed the way the cell leans, so the
            # trace keeps its shape instead of turning into an envelope.
            hi, lo = v.max(axis=1), v.min(axis=1)
            out.append(np.where(np.abs(hi) >= np.abs(lo), hi, lo).tolist())
        return out[0], out[1]


# -------------------------------------------------------- fingerprint


def to_16k_mono_pcm(mono: np.ndarray) -> bytes:
    """24 kHz float -> 16 kHz signed 16-bit, which is what a
    fingerprinter wants.

    24000/16000 is exactly 3:2, so this is a clean polyphase decimation
    rather than a resample: low-pass at 8 kHz, then take two of every
    three samples.  Skipping the filter would fold 8-12 kHz down over the
    top of the material and cost matches.
    """
    if mono.size < 12:
        return b""

    # 31-tap windowed sinc, cutoff 8 kHz at 24 kHz = 1/3 of the rate.
    taps = 31
    m = taps - 1
    idx = np.arange(taps) - m / 2.0
    h = np.sinc(idx * (2 * 8000.0 / RATE)) * np.hamming(taps)
    h /= h.sum()

    filtered = np.convolve(mono, h, mode="same")

    # 3:2 by interpolating to the 16 kHz grid; the anti-alias is done.
    n_out = int(mono.size * 16000 / RATE)
    src = np.arange(n_out) * (RATE / 16000.0)
    resampled = np.interp(src, np.arange(filtered.size), filtered)

    clipped = np.clip(resampled, -1.0, 1.0)
    return (clipped * 32767.0).astype("<i2").tobytes()
