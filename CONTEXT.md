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

### Done

All four trees below are written and verified as far as they can be
without hardware. Nothing has been flashed or linked.

**`firmware/pico/` — RP2350 controller. Complete.**

| | |
| --- | --- |
| `main.c` | init order, core 1 launch, main loop, one dispatcher for every source of user intent |
| `power.c` | on/off sequencing, low-power standby at 12 MHz in WFI |
| `audio.c` | audio state; enforces the PGA ceiling and the EQ-boost clamp |
| `pga2320.c` `bd37033.c` | volume over SPI, three-band tone over I²C |
| `relays.c` | mains, secondary, input select, EQ bypass |
| `encoder.c` | quadrature, click, long press; owns the GPIO callback |
| `irrx.c` | NEC / NEC-ext / RC5 / RC5X / SIRC 12-15-20, all learned |
| `settings.c` | wear-levelled flash config, two slots, CRC32 + sequence |
| `menu.c` | settings menu state machine, rendered on the Pi's panel |
| `meter.c` `fft.c` | stereo metering, two Q15 transforms per block on core 1, plus the decimated audio stream |
| `linkpi.c` `linkesp.c` | the two UART links |
| `test/dsptest.c` | host check of the μ-law codec and the decimation filter |

**`firmware/esp32c3/` — ESP-NOW endpoint. Complete.** Radio, UART bridge
to the Pico, cached state, the wake-preamble handshake.

**`firmware/knob/` — battery knob. Complete.** ST7701S panel bring-up,
LVGL UI with five screens, deep sleep on GPIO0, ESP-NOW link with
keepalive and a link indicator, battery gauge, cover fetch over HTTP.

**`firmware/pi/ampd/` — the Pi service. Core complete.**

| | |
| --- | --- |
| `proto.py` | the wire format, cross-checked against the C header |
| `link.py` | async UART, reconnects forever, never blocks |
| `audio.py` | μ-law ring, per-channel spectra, fingerprint captures |
| `eq.py` | seven sliders → BD37033 settings, ~2 ms a solve |
| `fingerprint.py` | providers, track watcher, match cache |
| `sources.py` | MPD, LinkPlay/WiiM, fingerprint, in priority order |
| `visualizers.py` | the mode manifest the web UI edits |
| `config.py` | settings, one JSON file we own |
| `service.py` | the loop, and the three policies |

Five test suites, all passing, each printing what it measured:

```
tests/test_proto.py        ids and framing against firmware/common/proto.h
tests/test_audio.py        ring, interleave, band mapping, resampler aliasing
tests/test_eq.py           fit quality per preset, slider authority, timing
tests/test_fingerprint.py  lookups per track, volume immunity, cache keys
tests/test_service.py      when the audio stream and fingerprinter run
```

### Next, in order

**1. `pi/ampd/api.py` — REST + WebSocket (FastAPI).**

Nothing else can be built until this exists; the panel and the web UI are
both clients of it. Suggested surface, which `service.py` already has the
state for:

```
GET  /api/state                     State.to_json()
POST /api/power        {on}
POST /api/volume       {db} | {rel}
POST /api/mute         {on|toggle}
POST /api/input        {0|1}
POST /api/eq           {sliders:[7]}  -> {solution, target[], achieved[]}
GET  /api/visualizers                  Registry.to_json()
PUT  /api/visualizers/{id}  {enabled}
PUT  /api/visualizers/order {ids:[]}
GET  /api/config  /  PUT /api/config
POST /api/menu/key     {key}           1=up 2=down 3=ok 4=back
GET  /cover.jpg?id=N                   480x480 for the knob, 400x400 panel
WS   /ws                               state + meter frames, ~47 Hz
```

Subscribe with `service.subscribe()`; it already coalesces and drops for
a slow client rather than blocking.

**2. `pi/web/panel/` — the 1920×480 kiosk page.**

Now playing on the **left 640**, visualiser on the **right 1280**. Swipe
the visualiser area to change mode, long-press for the picker. Transport
buttons only on the digital input — on analogue there is nothing to
control.

Fifteen modules in `vis/`, one per id in `visualizers.py`:
`vu vudial ppm peakbars bars mirror led dots dualarea stereoblend scope
fillwave mesh gonio particles` (plus `blank`). Every one is stereo.

A module exports one object:

```js
export default {
  id: "bars",
  needs: ["bands"],              // must match visualizers.py
  init(ctx, w, h) {},            // called on mode change and resize
  render(ctx, frame) {},         // frame = { bandL[32], bandR[32],
                                 //   peakL, peakR, rmsL, rmsR,
                                 //   waveL[], waveR[], edgesHz[] }
  destroy() {}
};
```

Band bytes are 0..255 at **2.657 units per dB with 0 dBFS at 208** — the
same scale on both feeds, so a module never has to ask which one it is
drawing. `edgesHz` travels with the data; do not assume a range.

**3. `pi/web/ui/` — the settings web UI.**

Seven-band EQ drawing **target and achieved together** — without both the
sliders lie. Mark the 160 Hz and 400 Hz sliders as low authority (~60%);
they sit in the gap between the bass band's 120 Hz ceiling and the mid
band's 500 Hz floor. Also: visualiser enable and reorder, input naming,
config fields, and the link/provider diagnostics from `link.stats()`.

**4. Cover art enrichment.** Shazam does not always return a URL, and the
knob wants exactly 480×480 or it refuses the JPEG. iTunes Search and the
Cover Art Archive are both free and keyless; Last.fm's artwork is
unreliable now (placeholder images). The Pi resizes — the knob will not.

**5. `systemd/ampd.service` and an install script.** Its own unit, its own
directory, touching nothing of moOde's so an update cannot fight it.

### Then, when hardware exists

- Flash and check the power sequencing against a scope before connecting
  the amplifier
- Confirm the encoder's resting contact state before enabling
  `WAKE_ON_ROTATION` on the knob — see its README
- Measure the knob's deep-sleep current with the battery divider fitted
- Pick the Pi 4's serial overlay and confirm 921600 is clean

### Optional, considered and deliberately not built

- **YAMNet** as a content gate — it classifies audio *events*, so it
  cannot identify a track, but it would stop a title being offered while
  a radio presenter is talking. ~100 ms an inference on a Pi 4.
- **Last.fm scrobbling**, including scrobbling vinyl the fingerprinter
  identified. Last.fm cannot identify audio, but it can receive a scrobble.
- A **local library index** so the analogue input matches your own
  collection offline, with no service at all.

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
