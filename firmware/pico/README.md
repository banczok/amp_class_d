# preamp_ctrl — Raspberry Pi Pico 2 firmware

Controller firmware for the `amp_ctrl` preamp board. Target is a **Raspberry Pi
Pico 2 (RP2350A)**, SMD-mounted or on headers.

The rule the whole design follows: **the Pico is authoritative.** Volume,
balance, input, EQ, presets and power state all live here, in flash-backed
settings. The Raspberry Pi 4 is a display and a touch surface; the ESP32-C3 is
a radio. Unplug either one and the encoder, the IR remote and the front-panel
power button still work.

## Build

```bash
cmake -B build -S . -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_BOARD=pico2
```

```bash
cmake --build build -j
```

Then hold BOOTSEL, plug in USB, and copy `build/preamp_ctrl.uf2` to the
`RP2350` mass-storage device — or flash over SWD:

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000" -c "program build/preamp_ctrl.elf verify reset exit"
```

Requires Pico SDK 2.0 or newer (RP2350 support).

## Layout

| File | What it owns |
| --- | --- |
| `include/board.h` | Pin map, taken from the KiCad netlist. Audio constants and the two clamps. |
| `include/proto.h` | Frame codec and message set, shared by both UART links. |
| `src/main.c` | Init order, core 1 launch, the main loop, IR/knob → action dispatch. |
| `src/power.c` | Power sequencing and the low-power standby. |
| `src/audio.c` | Audio state; enforces the PGA ceiling and the EQ-boost clamp. |
| `src/pga2320.c` | SPI volume control. |
| `src/bd37033.c` | I²C 3-band parametric EQ. |
| `src/relays.c` | Mains, secondary, input select, EQ bypass. |
| `src/encoder.c` | Quadrature decode, click, long press. Owns the GPIO callback. |
| `src/irrx.c` | NEC / NEC-ext / RC5 / RC5X / SIRC 12-15-20 decode. |
| `src/settings.c` | Wear-levelled flash config, two slots, CRC32 + sequence number. |
| `src/menu.c` | Settings menu state machine. |
| `src/meter.c` | ADC + DMA capture, level and 32-band spectrum on core 1. |
| `src/fft.c` | Q15 radix-2 FFT, N = 1024. |
| `src/linkpi.c` | UART0 to the Pi, 921600, through the isolator. |
| `src/linkesp.c` | UART1 to the on-board ESP32-C3, 115200. |

## Controls

**Encoder** — turn for volume (1 dB per detent), short press to mute, long press
(1.2 s) for the settings menu. Inside the menu: turn to move, press to select,
long press to save and leave.

**Power button** — toggles power. It works even with the menu open; it is the
escape hatch.

**IR remote** — every key is learned, nothing is hard-coded, so any household
remote works. Learning stores a normalised `{protocol, address, command}`
triple rather than raw timings, which is why holding a key repeats correctly
and why a remote that sends NEC repeat frames behaves.

## Settings menu

Long-press the encoder. Rendered on the Pi's 8.8" panel over UART, and drivable
from either the panel's touch or the encoder — the menu emits four keys
(UP/DOWN/OK/BACK) and does not care which produced them.

- **IR remote learning** — pick a function, press a key on the remote. OK while
  waiting clears the existing binding instead. 12 s timeout.
- **Input names** — rename Digital and Analogue, character by character on the
  encoder.
- **EQ presets** — four slots: load, save current, rename, clear.
- **Levels** — startup volume, max volume, balance.

Everything is written to flash on exit.

## Power sequencing

On:

```
mute → K3 mains → 400 ms → K2 secondary → 600 ms → PGA/BD init → 2.5 s → unmute
```

Off:

```
mute → 200 ms → shutdown request → Pi halts (30 s timeout) → K2 off → 300 ms → K3 off
```

Both mutes are asserted before anything moves. K3 energises the amplifier at the
same instant it energises T1, so for a few hundred milliseconds the amp is live
while the preamp rails are still coming up and C61/C62 charge toward VREF. The
BD37033 MUTE pin is the only control downstream of those caps, so it is the one
that has to be held.

## Standby

The Pico and the ESP32-C3 stay powered from +5VA; everything else is dead. After
two idle seconds in standby the firmware stops the meter, drops `clk_sys` to the
12 MHz crystal, deinits the Pi UART and sits in WFI.

Three things wake it: the power button, the encoder, and the ESP32 pulling
UART1 RX low. All three are falling-edge GPIO interrupts — every input on this
board has an external pull-up, which is also what keeps the RP2350-E9 pull-down
erratum out of the design.

`encoder.c` owns the single GPIO callback the SDK allows per core; `power.c`
asks it whether an edge arrived rather than installing a second one.

The USB console is dead while asleep (no USB PLL at 12 MHz). Touch the encoder
and it comes straight back.

**ESP32 wake handshake:** the byte that wakes the core is lost, because the UART
is unclocked when it arrives. The C3 therefore sends a short train of
`MSG_ESP_WAKE` preamble frames, waits ~20 ms, and only then sends the real
message.

## Metering

Tapped ahead of the volume control, so the reading is volume-independent —
post-volume metering gives about nine ADC counts at −40 dB.

Round-robin ADC over both channels, 96 kSPS total (48 kHz per channel), into a
ping-pong DMA buffer. Core 1 windows, transforms and bands it; core 0 never
waits for a 1024-point FFT to finish before it can change the volume. Results go
to the Pi at ~47 Hz.

## The two clamps

1. **PGA never exceeds 0 dB** (code 192). Its positive-gain range is noisy and
   the chain does not need it. Enforced in `audio_set_volume()` and again inside
   `pga_write()`, so no code path routes around it.
2. **EQ boost is clamped against volume:** `max_boost = min(15, −3 − pga_dB)`.
   At PGA 0 dB the chain already delivers 2.12 V differential into an amplifier
   that clips at 3.05 V, so boost there clips the amp, not the preamp. Turning
   the volume up pulls the EQ down with it — it re-clamps, it does not merely
   refuse the next boost.

A third ceiling, the user's **max volume** from the Levels menu, sits on top of
both.

## Protocol

```
0xA5 | LEN | TYPE | payload[LEN] | CRC8
```

CRC8 is Dallas/Maxim (poly 0x31, init 0x00) over TYPE and payload. LEN counts
payload bytes only, 0..250. Same framing on both links; only the message set
differs. See `include/proto.h`.

## Placeholders

Marked `PLACEHOLDER: Raspberry Pi 4` in the source. Transport keys
(prev / play-pause / next) are forwarded as `MSG_EVENT` kind 1 and otherwise
ignored — the Pico has no business knowing about tracks. `MSG_NOWPLAYING` from
the Pi is relayed straight through to the ESP32 for the knob display.

## Notes on the Raspberry Pi 4

Two things about the host change with the move from a 3A+ to a Pi 4 (2 GB),
and neither is in this firmware — they are on the Pi and on the supply.

**The header UART.** By default GPIO14/15 carry the mini-UART, whose baud rate
is derived from the VPU core clock, so it drifts whenever the core clocks down.
At 921600 that shows up as occasional framing errors rather than an obvious
failure. Put the real PL011 on the header instead, in `/boot/firmware/config.txt`:

```
dtoverlay=miniuart-bt
```

That moves Bluetooth to the mini-UART and gives GPIO14/15 the PL011 — Bluetooth
still works, unlike `disable-bt`. Then in `/boot/firmware/cmdline.txt` remove
`console=serial0,115200` so nothing else is talking on the link, and:

```
sudo systemctl disable --now serial-getty@ttyAMA0.service
```

The Pi 4 also has four extra PL011s (`dtoverlay=uart2`..`uart5`) if the wiring
ever moves off GPIO14/15 — that option did not exist on the 3A+.

**Current draw.** A 3A+ idles around 0.25 A at 5 V; a Pi 4 idles nearer 0.6 A
and peaks over 1 A, before the 8.8" panel. Check what feeds the Pi's 5 V rail on
this board before assuming the swap is free — the sequencing and the halt
handshake are unaffected, but the regulator sizing may not be.

Nothing about 2 GB versus 512 MB matters to the Pico side.

## Checking it without hardware

There is no host test harness, but the sources compile clean under
`-Wall -Wextra` against stub SDK headers, which catches the ordinary
typo-and-signature class of mistake before it reaches a board.
