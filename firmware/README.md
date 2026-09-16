# amp_class_d firmware

Four processors, one amplifier. Three of them are in this tree.

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

| Directory | Target | What it is |
| --- | --- | --- |
| `pico/` | RP2350 (Pico 2) | The controller. Owns all state. |
| `esp32c3/` | ESP32-C3 | On-board radio, always powered from +5VA. |
| `knob/` | ESP32-S3 | Battery remote with a round touch display. |
| `pi/` | Raspberry Pi 4 | Panel UI, web UI, MPD, fingerprinting, EQ solver. |
| `common/` | — | The frame codec, shared verbatim by all three. |

Each has its own README.

## The rule

**The Pico is authoritative.** Volume, balance, input, EQ, presets and power
state live there, in flash-backed settings. The Pi is a display and a touch
surface. The C3 is a wire with an antenna. The knob is a remote.

Pull the Pi cable and the encoder, the IR remote and the front-panel button all
still work. That is the acceptance test, and it is why nothing else caches
authoritative state.

## Wire format

One codec everywhere — UART0 to the Pi, UART1 to the C3, and inside the ESP-NOW
payloads:

```
0xA5 | LEN | TYPE | payload[LEN] | CRC8
```

CRC8 is Dallas/Maxim (poly 0x31, init 0x00) over TYPE and payload. LEN counts
payload only, 0..250. Message ids are in `common/proto.h` and that file is the
single source of truth for all three builds.

## What the Raspberry Pi 4 has to provide

Not written yet — everything on the Pico and knob sides that touches it is
marked `PLACEHOLDER: Raspberry Pi 4` in the source. The contract is:

**Over UART0, 921600 8N1, through the isolator**

| Direction | Message | |
| --- | --- | --- |
| Pi → Pico | `MSG_HELLO` | I am up. Pico replies with full state. |
| Pi → Pico | `MSG_CMD` | Set one field: `[id][int16 LE]`, ids in `proto.h`. |
| Pi → Pico | `MSG_MENU_KEY` | `[key]` — UP/DOWN/OK/BACK from the panel's touch. |
| Pi → Pico | `MSG_NOWPLAYING` | Track metadata; relayed straight to the knob. |
| Pico → Pi | `MSG_STATE` | Power, volume, mute, input, EQ path, tone, headroom. |
| Pico → Pi | `MSG_METER` | Peak, RMS and 32 bands, ~47 Hz. |
| Pico → Pi | `MSG_MENU` | The settings screen to draw: `[screen][cursor][count]` then length-prefixed rows. |
| Pico → Pi | `MSG_EVENT` | Kind 1 = a transport key was pressed. |

`MSG_NOWPLAYING` / `MSG_ESP_NOW` payload:

```
[0]   flags, bit0 = playing
[1]   art_id, bumped whenever the cover changes, 0 = none
[2..] "artist\0title\0album\0", UTF-8
```

**Over UART, Pico to Pi, on demand**

`MSG_AUDIO` carries the analogue input as 24 kHz mono G.711 mu-law — 24 kB/s,
26% of the link — so the Pi can fingerprint what is playing and draw real
waveforms. Enabled with `CMD_AUDIO_STREAM`. See `pi/README.md`.

**Over HTTP, for the knob**

```
GET <base>/cover.jpg?id=<art_id>     ->  480x480 baseline JPEG
```

The knob refuses anything that is not exactly 480×480. Scaling belongs on the
Pi.

**Discrete lines, through the isolator**

| | |
| --- | --- |
| `SHUTDOWN_REQ` (Pico GP18 → Pi) | Asserted to ask the Pi to halt. |
| `HALTED` (Pi → Pico GP19) | Pi asserts when it has halted. 30 s timeout, then the mains relay opens anyway — a wedged Pi must not keep the amplifier powered. |

**Serial setup on a Pi 4**

GPIO14/15 carry the mini-UART by default, whose baud is derived from the VPU
core clock and drifts when it clocks down. Move the PL011 to the header —
`dtoverlay=miniuart-bt` in `config.txt` — and free it from the console. Details
in `pico/README.md`.

## Building without hardware

There is no Pico SDK or ESP-IDF in this tree, and no host test harness. What
there is: all three projects compile clean under `gcc -Wall -Wextra` against
stub SDK headers, which catches the ordinary typo-and-signature class of mistake
before it reaches a board. It is not a link and it is not a test.
