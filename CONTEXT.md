# amp_class_d — project context

Handoff notes for picking this up on another machine. Read this first;
each directory then has its own README with the detail.

**Nothing here has ever run on hardware.** No board has been flashed, no
firmware linked. What exists is verified as far as it can be without the
hardware: the three C trees compile clean under `gcc -Wall -Wextra`
against stub SDK headers, and the Python has five test suites that
measure rather than assert.

## What it is

A DIY audio preamplifier controller. Four processors:

```
              IR                encoder + power button
               |                        |
               v                        v
 Raspberry Pi 4 <--UART0--> Raspberry Pi Pico 2 <--UART1--> ESP32-C3
 (8.8" panel,   921600      (authoritative:      115200     (ESP-NOW
  moOde)        isolated     volume, EQ, power)              endpoint)
                                   |                            |
                            PGA2320 / BD37033              ESP-NOW
                            relays, meter ADC                   |
                                                                v
                                                        ESP32-S3 knob
                                                        (480x480 round,
                                                         battery)
```

| | |
| --- | --- |
| Board | `amp_ctrl/` — KiCad 8, 100×100 mm, 4 layer, split AGND/DGND |
| Volume | PGA2320, SPI, via a 74AHCT125 level shifter |
| Tone | BD37033FV-M, I²C, **three** parametric bands |
| Output | DRV135UA balanced drivers |
| Host | Raspberry Pi 4, 2 GB, under moOde |
| Panel | Waveshare 33641 / 8.8-DSI-TOUCH-A, **1920×480**, 10-point touch |
| Knob | VIEWE UEDX48480021-MD80ESP32, ESP32-S3, 480×480 round, battery |

## Layout

```
amp_class_d/
├── CONTEXT.md          ← this file
├── amp_ctrl/           ← KiCad project
└── firmware/
    ├── README.md       ← system overview, wire format, the Pi's contract
    ├── common/         ← proto.c/.h, shared verbatim by all three MCUs
    ├── pico/           ← RP2350 controller        (C, Pico SDK 2.0+)
    ├── esp32c3/        ← on-board ESP-NOW radio   (C, ESP-IDF 5.x)
    ├── knob/           ← battery knob display     (C, ESP-IDF 5.2+, LVGL 8.4)
    └── pi/             ← the Pi service `ampd`    (Python 3.10+)
```

## The rule everything follows

**The Pico is authoritative.** Volume, balance, input, EQ, presets and
power state live there, in flash-backed settings. The Pi is a display and
a touch surface. The C3 is a wire with an antenna. The knob is a remote.

Pull the Pi cable and the encoder, the IR remote and the front-panel
button all still work. That is the acceptance test, and it is why nothing
else caches authoritative state.

## Status

| Tree | State |
| --- | --- |
| `pico/` | **Complete.** All 18 sources + a host DSP test. |
| `esp32c3/` | **Complete.** |
| `knob/` | **Complete.** |
| `pi/` | Core done: proto, link, audio, eq, fingerprint, sources, config, service, visualizers. |

**Not written yet, in order:**

1. `pi/ampd/api.py` — REST + WebSocket (FastAPI)
2. `pi/web/panel/` — the 1920×480 kiosk page and the **15 visualiser
   modules** (`vis/<id>.js`, one per mode in `visualizers.py`)
3. `pi/web/ui/` — settings web UI: 7-band EQ with target-vs-achieved
   curves, visualiser enable/reorder, config
4. Cover art enrichment — Shazam does not always return a URL and the
   knob needs exactly 480×480. iTunes Search and Cover Art Archive are
   both free and keyless; Last.fm's art is unreliable now.
5. `systemd/ampd.service` and an install script

## Resuming on another machine

```bash
git clone git@github.com:banczok/amp_class_d.git
cd amp_class_d
claude
```

`CLAUDE.md` is loaded automatically and imports this file, so a fresh
session starts with the project context already in front of it. Nothing
else needs installing to run the Python tests.

Working across two machines, the only real hazard is the KiCad files:
they are text, but a three-way merge of a `.kicad_pcb` produces a board
that opens and is silently wrong. `.gitattributes` marks them
`merge=binary` so git refuses rather than guesses. In practice: **pull
before you open KiCad, commit before you stop**, and never edit the board
on two machines without pushing in between.

## Building and testing

Nothing here needs hardware.

```bash
cd firmware/pi && for t in proto audio eq fingerprint service; do python tests/test_$t.py; done
```

Needs numpy only. Each script prints what it measured rather than just
passing.

```bash
cd firmware/pico && cmake -B build -S . -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_BOARD=pico2 && cmake --build build -j
```

```bash
cd firmware/knob && idf.py set-target esp32s3 && idf.py menuconfig && idf.py build
```

The C trees were verified with stub SDK headers rather than a real SDK —
that catches typos and signature errors, not API misuse. Regenerating the
stubs is in the session history; simplest is to install the real SDKs.

## Deliberate decisions — ask before changing

These cost real effort to arrive at. Several were bugs found by
measurement, so reverting them silently reintroduces the bug.

**Relay pin wiring is correct as drawn.** Do not re-derive it from the
KiCad files. It was got wrong twice by matching contact polylines to pins
by file order rather than geometry. Work from the logical sense —
energised versus de-energised, which path is selected — and let the
polarity macros in `pico/include/board.h` carry it.

**The PGA never exceeds 0 dB** (code 192), enforced in
`audio_set_volume()` and again inside `pga_write()`.

**EQ boost is clamped against volume:** `max_boost = min(15, −3 − pga_dB)`.
At PGA 0 dB the chain delivers 2.12 V differential into an amplifier that
clips at 3.05 V, so boost there clips the amp. Turning the volume up
pulls the EQ down with it.

**Seven sliders onto three hardware bands is a fit, not a mapping.**
`eq.py` searches the chip's 61 M combinations for the closest response.
The UI must draw target *and* achieved, or the sliders lie. The 160 Hz
and 400 Hz sliders have only ~60% authority — they fall in the gap
between the bass band's 120 Hz ceiling and the mid band's 500 Hz floor.

**The analogue audio stream is 24 kHz, decimated from 48 kHz through a
39-tap half-band FIR.** Sampling the ADC slower instead would fold
12–19.9 kHz back into the audible band, because the board's anti-alias
filter sits at 19.9 kHz for the 48 kHz rate. 23 taps gave only 20 dB of
rejection at 14 kHz; 39 gives 44 dB. `pico/test/dsptest.c` checks this
and the μ-law codec.

**Band bytes are logarithmic** — 2.657 units per dB, 0 dBFS at 208. A
volume change *subtracts* a constant from every band. Normalising a
spectrum by dividing by its sum removes nothing and makes a volume change
look exactly like a new track. Both the track watcher and the match cache
key work on shape relative to the loudest band.

**Everything is stereo end to end.** `MSG_METER` carries `band_l[32]` and
`band_r[32]` — the Pico runs a transform per channel, because a summed
spectrum cannot be un-summed at the far end.

**`encoder.c` owns the single GPIO callback the SDK allows per core** on
the Pico. `power.c` asks it whether an edge arrived rather than
installing a second one, which would silently unhook the encoder.

**The knob wakes on the encoder press only.** The board leaves the touch
interrupt unconnected, so touch can never be a wake source. Rotation wake
exists behind `WAKE_ON_ROTATION` but is off: the first detent's direction
is unrecoverable, and if the encoder rests with a contact closed the
internal pull-up burns ~73 µA continuously.

**The knob's keepalive is load-bearing.** The amplifier pushes state when
it *changes*, so without a hello every 1.5 s a quiet minute looks like an
unplugged amplifier and the link indicator goes red. The C3 only forwards
those to the Pico when its cache is >5 s old, or it would wake a sleeping
Pico five times a session.

**Fingerprinting happens at the start of music, not on a timer.** Two
lookups a track, measured. Sources are tried in priority order — MPD
(digital, exact) → WiiM/LinkPlay (analogue, exact) → fingerprint
(analogue, a guess). `shazamio` is free and unofficial; AcoustID is free
but built for whole files, not excerpts; Last.fm cannot identify audio at
all.

**The audio stream is off unless something needs it.** Never on digital.
On analogue only for a timed capture or while a waveform visualiser is on
screen. It is 52% of the UART.

## Board — open items, all known

Not regressions; the user is aware of each.

- Mains creepage 2.22 mm to +5VA, wants ~6 mm
- ~11 via-to-track clearance errors
- ~12 AGND/DGND domain mismatches (U3 and the driver cluster over AGND)
- Silkscreen text below the fab minimum
- Stale value fields: K2 says `PR30-12V` while `PR30-5V` is ordered;
  K3 / U12 / A1 / U9 likewise

`amp_ctrl/amp_ctrl.kicad_pcb` is the board, and the only one. An earlier
scratch copy was edited as text and ended up internally inconsistent -
footprint rotations changed without the pad angles that must follow them,
because KiCad stores pad angles absolutely - which showed up as about 93
phantom shorts in DRC. It has been deleted. Edit KiCad files in KiCad.

## Hardware setup notes

**Pi 4 serial.** GPIO14/15 carry the mini-UART by default, whose baud is
derived from the VPU core clock and drifts when it clocks down — at
921600 that is intermittent framing errors. Put the PL011 on the header:
`dtoverlay=miniuart-bt` in `/boot/firmware/config.txt`, remove
`console=serial0,115200` from `cmdline.txt`, and
`systemctl disable --now serial-getty@ttyAMA0`.

**Pi 4 current.** A 3A+ idled ~0.25 A; a Pi 4 idles nearer 0.6 A and
peaks over 1 A before the panel. Check what feeds its 5 V rail.

**Knob battery sense.** GPIO4 is ADC1_CH3 — ADC1 matters because ADC2 is
unusable while WiFi is up. Use **equal resistors** (100 k / 100 k
recommended) plus 100 nF at the pin. A 1.4:1 divider puts a full cell in
the compressed part of the S3's attenuation curve. 10 k / 10 k works but
draws 210 µA continuously, which is 14× the deep-sleep current.

## Conventions

- Comments explain *why*, especially where something looks wrong but is
  deliberate. Match the surrounding density.
- `firmware/common/proto.h` is the single source of truth for the wire
  format. `pi/tests/test_proto.py` reads that header and checks the
  Python against it, so drift fails a test rather than a control.
- British spelling in prose; American in identifiers where an SDK uses it.
