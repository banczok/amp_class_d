# ampd — Raspberry Pi 4 side

Runs on the Pi 4 (2 GB) under **moOde**, on the 8.8" Waveshare panel
(33641 / 8.8-DSI-TOUCH-A, 1920×480, 10-point touch).

It lives here rather than under `software/` so all four processors in this
amplifier sit in one tree, even though only three of them run firmware.

Not a replacement for moOde — a separate service alongside it, in its own
directory with its own systemd unit, touching none of moOde's files so an
update cannot fight it. MPD is already there on port 6600; we talk to it.

## Panel layout

```
├──── now playing 640 ────┤├──────────── visualiser 1280 ────────────┤
  cover 400×400 · input
  volume · title · artist      swipe left/right to change a mode
  ◀◀  ▶Ⅱ  ▶▶  (digital only)   long-press for the picker
```

Transport appears only on the digital input. On analog there is nothing to
control — the title and cover come from fingerprinting, not from a player.

## Status

| | |
| --- | --- |
| `ampd/proto.py` | Done. The wire format, cross-checked against the C header. |
| `ampd/link.py` | Done. Async UART, reconnects forever, never blocks. |
| `ampd/audio.py` | Done and measured. Ring, spectra, fingerprint captures. |
| `ampd/eq.py` | Done and measured. Seven sliders → BD37033 settings. |
| `ampd/visualizers.py` | Done. The mode manifest the web UI edits. |
| `ampd/fingerprint.py` | Done and measured. Providers, track watcher, cache. |
| `ampd/sources.py` | Done. MPD, LinkPlay/WiiM and fingerprint, in priority order. |
| `ampd/config.py` | Done. Settings the web UI edits, one JSON file we own. |
| `ampd/service.py` | Done and tested. The loop, and the three policies. |
| REST + WebSocket API | Not written yet. |
| panel page, 15 visualiser modules, web UI | Not written yet. |

Run the three test scripts directly; each prints what it measured:

```bash
for t in proto audio eq fingerprint service; do python tests/test_$t.py; done
```

## Everything is stereo

Not just the modes that obviously need it. The chain carries both channels
end to end:

- **`MSG_METER`** carries `band_l[32]` and `band_r[32]`, not a mono sum. The
  Pico runs a transform per channel — about 2% more of core 1 — because a
  summed spectrum cannot be un-summed at this end.
- **`MSG_AUDIO`** interleaves L,R when asked to. Mono is 24 kB/s, stereo
  48 kB/s, and the mode is switchable because a fingerprint gets summed to
  mono anyway and there is no reason to pay double for one.

The band byte is the same scale on both feeds, derived rather than guessed:
the Pico emits 8 units per doubling of band power from a Q15 transform,
which is 2.657 units per dB with 0 dBFS landing on 208. `Analyser.spectrum`
normalises to the same scale, so a visualiser never has to ask which feed
it is looking at.

## The seven-band EQ on a three-band chip

The BD37033 has three parametric bands and nothing will change that:

```
bass    f0  60 / 80 / 100 / 120 Hz     Q  0.5 / 1.0 / 1.5 / 2.0
middle  f0  0.5 / 1 / 1.5 / 2.5 kHz    Q  0.75 / 1.0 / 1.25 / 1.5
treble  f0  7.5 / 10 / 12.5 / 15 kHz   Q  0.75 / 1.25
gain    0 to ±15 dB in 1 dB steps, each band
```

So the seven sliders are treated as a **target curve** — seven peaking
filters at ISO centres, Q 1.4, exactly what a real graphic EQ is — and
`eq.solve()` searches the chip's 61 million combinations for the closest
response it can actually produce. Coordinate descent over precomputed
per-band responses; about 2 ms here, call it 9 ms on a Pi 4, so it can run
on every slider drag.

**The UI must draw both curves.** Target dashed, achieved solid. Without
that the sliders lie.

### How well it fits

Worst miss over the weighted 40 Hz – 16 kHz band, from `tests/test_eq.py`:

| Preset | Max error |
| --- | --- |
| notch at 1 kHz | 0.31 dB |
| bass boost | 1.94 dB |
| smiley | 2.09 dB |
| vocal lift | 2.12 dB |
| tilt up / down | 2.39 dB |
| treble boost | 3.37 dB |
| everything +10 | 5.36 dB |
| alternating ±8 | 7.61 dB |

Under about 1.5 dB is inaudible. The two bad cases are exactly the shapes
three bells cannot make — a flat shelf across the whole spectrum, and a
comb — and the achieved curve shows that plainly.

### Slider authority

The chip has coverage gaps. Push one slider to +8 dB and measure what
comes out at that frequency:

| Slider | Delivered | Authority | |
| --- | --- | --- | --- |
| 63 Hz | 7.7 dB | 97% | bass band centres here |
| 160 Hz | 4.6 dB | **58%** | gap between 120 Hz and 500 Hz |
| 400 Hz | 4.9 dB | **62%** | gap |
| 1 kHz | 7.9 dB | 99% | mid band centres here |
| 2.5 kHz | 7.9 dB | 99% | mid band centres here |
| 6.3 kHz | 6.1 dB | 76% | gap between 2.5 kHz and 7.5 kHz |
| 16 kHz | 6.8 dB | 85% | above the top f0 of 15 kHz |

The 160 Hz and 400 Hz sliders can only be reached through the skirts of
the bass and mid bands. **The web UI should mark those sliders** rather
than let someone drag one and wonder why little happens.

Standard ISO centres were kept deliberately. Choosing centres that
flatter the solver would hide the limitation instead of showing it.

The headroom clamp the Pico enforces — `max_boost = min(15, −3 − pga_dB)`
— is applied inside the solver too, so the curve on screen is the curve
you get rather than one the firmware quietly undoes afterwards. Cuts are
never clamped; the ceiling is about boost only.

## Audio from the analog input

The Pi cannot hear the analog input — there is no converter on it and the
amplifier does not route audio its way. Without something, the analog side
has no title, no artist and no cover, and the waveform visualisers have
nothing to draw.

So the Pico streams it over the UART that is already there:

```
48 kHz ADC → 39-tap half-band FIR → ÷2 → 24 kHz → G.711 μ-law → MSG_AUDIO
```

24 kB/s, **26% of the 921600 link**, leaving control, meter and menu
traffic (under 3 kB/s combined) untouched. Off by default; the Pi turns it
on with `CMD_AUDIO_STREAM`, either continuously or as a timed capture in
hundreds of milliseconds so a crashed Pi cannot leave it running.

**Why not just sample slower on the Pico.** The board's anti-alias filter
sits at ~19.9 kHz because the ADC runs at 48 kHz. Sampling at 24 kHz
directly folds 12–19.9 kHz back into the audible band, and invented
spectral energy is precisely what stops a fingerprint from matching. The
digital filter is doing real work: measured −44 dB at 14 kHz and −90 dB at
16 kHz, flat to 10 kHz. `firmware/pico/test/dsptest.c` checks it, and
checks the μ-law codec against a reference decoder.

**Why 24 kHz and not 16.** A fingerprinter wants 16 kHz, but that leaves
the spectrum display with nothing above 8 kHz. 24 kHz is one clean halving
of what the ADC already runs at, gives 12 kHz of spectrum, and the Pi can
decimate again for the fingerprinter.

## What runs, and when it does not

Three policies live in `service.py` and they are the whole reason the
analogue path is affordable. All three are tested, because none of them
announce themselves when broken — a stream left running is 52% of the UART
gone, and a fingerprinter on a timer is hundreds of lookups a day.

**The audio stream is off unless something needs it.**

| | |
| --- | --- |
| digital input | **never on.** The Pi already has the audio. |
| analogue, no waveform mode on screen | off — the Pico's 32 stereo bands are enough |
| analogue, waveform mode on screen | on, continuous, stereo |
| analogue, fingerprint due | on for a timed burst, then off again |

The capture is timed **on the Pico as well**, not just here, so if this
process dies mid-capture the stream still stops on its own.

**Fingerprinting happens at the start of music.** The watcher is only fed
on the analogue input — on digital, MPD says when a track changes and
there is nothing to guess. A WiiM that answers stops the fingerprinter
before it starts.

**Every source rate-limits its own network traffic.** The WiiM is asked at
most every 2 s and MPD every 0.5 s, inside the source rather than by the
caller, so anything may ask as often as it likes without it becoming
traffic. Measured: 50 polls in a burst produce 1 request.

## Where "now playing" comes from

Fingerprinting is the **last** resort, not the first. It costs a round trip,
it guesses, and it can be wrong. Sources are tried in order of how much
they actually know:

| Priority | Source | Input | |
| --- | --- | --- | --- |
| 100 | **MPD** | digital | moOde already knows exactly. Nothing to improve on. |
| 50 | **LinkPlay / WiiM** | analog | Ask the streamer over the network. Exact, free, instant. |
| 10 | **Fingerprint** | analog | Only when nothing else can answer. |

The middle row is the one worth knowing about. The analogue input is only
a hard problem when what is plugged into it is *dumb* — a turntable, a
tuner, a tape deck. If it is a WiiM, that device already knows the title
and will tell you:

```
GET https://<ip>/httpapi.asp?command=getPlayerStatus
GET https://<ip>/httpapi.asp?command=getMetaInfo      # album art URL
```

Free, exact, no quota, and it gets things no fingerprinter ever will —
internet radio idents, podcasts, a track thirty seconds before the vocal
starts. With a WiiM on the analogue socket the audio stream never has to
come on for metadata at all, which is 52% of the UART saved.

Two details that bite. The fields are **hex-encoded**, but only on some
firmware, so it has to be detected — and detection has traps, because
`Cafe`, `abba`, `BEEF` and `decade` are all valid hex, and ABBA is a band.
Most decode to invalid UTF-8 and fall out on their own; `dead` decodes to a
perfectly good Arabic letter and does not. The rule that works is to accept
a plain-ASCII decode at any length and a non-ASCII one only from an input
too long to be a word. Second, newer firmware serves this over HTTPS with a
self-signed certificate and the wrong content type, so both schemes are
tried and verification is off.

`mode` values for line-in, Bluetooth and optical mean the WiiM is being
used as a switch rather than a player. Its metadata is stale then, so those
are ignored and the fingerprinter gets the job.

### Things that sound like they would help and do not

**Last.fm** cannot identify audio. Its fingerprinting service died years
ago; the API is scrobbling and lookup-by-name. It is genuinely useful here
for two other things — scrobbling what the amplifier plays, *including
vinyl the fingerprinter identified*, and filling in an album name — but it
cannot answer "what is this". Its album art is also unreliable now: many
responses return a placeholder star. iTunes Search and the Cover Art
Archive are both free, keyless and better for artwork.

**YAMNet** classifies audio *events* — 521 AudioSet labels like Music,
Speech, Guitar, Silence. It cannot tell you which song, and no amount of
fine-tuning changes that: identity is a retrieval problem against a
catalogue, not a classification problem over 521 classes. The neural
approach that *does* do identity is a learned embedding plus a database of
what you want to recognise, which is how Pixel's on-device Now Playing
works — and it needs the database, which is the hard half.

Where YAMNet would genuinely earn its keep is as a gate: not offering a
title while a radio presenter is talking, and labelling the panel Speech
rather than showing a stale track. That is a real improvement and about
100 ms an inference on a Pi 4. It is not on the critical path, so it is not
built.

## Recognising the analogue input

None of this costs money. The choice of service matters far less than how
often you ask it, which is what `TrackWatcher` is for.

| | Cost | Recall on a live signal | Catch |
| --- | --- | --- | --- |
| **shazamio** | free, no account | best by a wide margin | unofficial client; can break without notice |
| **AcoustID + MusicBrainz** | free, API key | poor on short excerpts | indexes whole-track fingerprints — built for "what file is this", not "what is playing" |
| **AudD** | paid per lookup | good | cheapest commercial option; pricing has changed more than once, check before relying on it |
| **ACRCloud** | paid, trial first | good | aimed at broadcast monitoring; more than this needs |
| **Gracenote** | enterprise | good | requires a commercial agreement. Not realistic here. |

**Use shazamio.** It builds Shazam's own signature locally and posts that,
so there is no key and no quota, and Shazam's catalogue is the reason it
identifies things the others miss. The risk is that it is reverse
engineered — which is exactly why `Provider` is an interface and switching
to AudD is one config line.

AcoustID deserves a word because it looks like the obvious free answer and
mostly is not. It matches whole-track Chromaprint fingerprints, so it is
excellent at identifying a file you already have and much weaker at a
fifteen-second excerpt from the middle of a record — especially a
different pressing, or a turntable running half a percent fast. Worth
having configured as a second opinion, not as the primary.

### What actually keeps it cheap

Asking on a timer would be hundreds of lookups a day. Asking when the
music demonstrably changes is two per track, measured:

```
12 tracks x 4 min  ->  24 lookups  (2.00 per track)
an 8 hour day      ->  about 240, before the cache takes any
```

Two signals arm a lookup, both from data already arriving at 47 Hz:

- **a gap** — quiet for more than 1.2 s, armed once per gap rather than
  once per frame of it
- **a spectral shift** — a 1.5 s average of the spectrum moving away from
  a 25 s average

The threshold is measured rather than guessed. With the shape normalised
across 32 bands, the distance between those two averages stays under
**0.7** inside a single track even with every band wandering ±5 dB, and
peaks near **8.7** at a track change. It is set at 2.0.

**The bands are logarithmic, and that changes the arithmetic.** At 2.657
byte units per dB, turning the volume down *subtracts* a constant from
every band — it does not scale them. Normalising a spectrum by dividing by
its sum, which is the reflex, removes nothing at all here and leaves a
volume change looking exactly like a new track. Both the watcher and the
cache key work on the shape relative to the loudest band instead. A 20 dB
volume sweep produces zero spurious lookups; a genuine change is caught in
under half a second.

The cache is keyed on that same shape, quantised hard, so the second time
a record comes round it is answered locally and costs nothing at all.

## Two data sources, one interface

| Input | Metadata | Spectrum / waveform |
| --- | --- | --- |
| digital | MPD, exact | Pi's own DSP on the played audio, full resolution |
| analog | fingerprint from `MSG_AUDIO` | Pi's DSP on the μ-law stream, 12 kHz |

The frequency axis travels with the data — `Analyser.edges_hz` reports the
edges the FFT bins **actually** represent, not the ones that were asked
for. At 24 kHz with a 2048-point transform the bottom band is 3 Hz wide
against an 11 Hz bin, so several low bands legitimately read the same bin;
forcing them apart shifts the whole axis and puts 5 kHz in a bar labelled
1 kHz.

Both go behind one interface so a visualiser never has to know which it is
getting. The Pico's own `MSG_METER` (32 bands at 47 Hz) stays as the cheap
always-on fallback when the audio stream is off.

## Running the EQ check

```bash
python tests/test_eq.py
```

Needs numpy only.
