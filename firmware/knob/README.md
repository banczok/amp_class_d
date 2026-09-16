# amp_knob — battery knob remote

Firmware for a **VIEWE UEDX48480021-MD80ESP32**: ESP32-S3-R8, 16 MB flash, 8 MB
octal PSRAM, 2.1" 480×480 round IPS on an ST7701S, capacitive touch, a rotary
encoder and a push. Battery powered, and asleep almost all of the time.

## Build

```bash
idf.py set-target esp32s3
```

```bash
idf.py menuconfig
```

Set your SSID, password and the Pi base URL under **Preamp knob remote**, then:

```bash
idf.py build flash monitor
```

ESP-IDF v5.2 or newer (5.2 for `ADC_ATTEN_DB_12`). Managed components (LVGL 8.4, `esp_lvgl_port`,
`esp_lcd_touch_cst816s`, `esp_new_jpeg`) are pulled by `idf_component.yml` on
the first build.

## The screens

| | |
| --- | --- |
| **Asleep** | Panel off, ESP32 in deep sleep. |
| **No link** | The amplifier did not answer. Dim, brief, keeps trying. |
| **Wake** | Power symbol and "hold 2 s". Only appears when the amp is off. |
| **No track** | Amp on, nothing playing: input name and current volume. |
| **Playing** | Cover full-bleed behind a flat scrim, title and artist near the top, prev / play-pause / next near the bottom. |
| **Volume** | Outer ring plus a large dB readout, over whatever was there. Fades after 1.5 s. |

A small battery gauge sits at the top of every screen except the volume
overlay, which wants the panel to itself, with a link indicator to its
left — quiet while the amplifier is answering, and it turns red the moment
it stops.

It is one LVGL screen with layers that show and hide, not five screens — the
cover has to survive a mode change without being re-decoded, and a 460 kB image
is not something to reload because the volume overlay appeared.

## Controls

| | |
| --- | --- |
| Short press, asleep | Wake. The only wake source, by default. |
| Short press, awake | Mute — the same thing the amplifier's own encoder does. |
| **Hold 2 s** | Power. The ring fills while you hold, so it is visible rather than a guess. Works from the wake press too: press and keep holding turns the amp on in one gesture. |
| Turn | Volume, 1 dB per detent. |
| Touch | Transport only. Nothing else on screen is touchable. |

**Touch cannot wake it.** This board leaves the touch interrupt unconnected
(`TP_INT = NC` in the vendor BSP), so the controller can only be polled over
I²C, which needs the CPU running. GPIO0, GPIO5 and GPIO6 are all RTC pads, so
the RTC controller can watch them with the digital domain powered down; the
touch chip cannot be watched by anything.

### Waking on rotation

Off by default. `WAKE_ON_ROTATION` in `board_knob.h` adds the two encoder phases
to the wake mask. Three things to know before turning it on:

**The first detent is unrecoverable.** Quadrature needs two edges to know which
way the knob went, and the CPU is not running for the first one. So a turn from
cold brings the screen up showing the *current* volume, and only the second
detent onward moves it. That is not a bug that can be fixed in firmware — the
direction information genuinely is not there.

**It may or may not cost current, depending on your encoder.** PHA and PHB have
no external pull-ups on this board — on the vendor schematic they run straight
from the module pins to the encoder contacts and then to ground, and R14/R15
(4k7) are the touch I²C pull-ups, not these. So the internal pull-ups are
load-bearing, and they have to be RTC pull-ups held through sleep. If your
encoder's detent leaves both contacts open — most EC11-style parts — that costs
nothing. If a contact rests closed, the ~45 kΩ internal pull-up burns about
**73 µA** through it continuously, which dwarfs every other number in this
document including the whole battery divider question. Measure it.

**A knob gets knocked.** Press-only means the amplifier does not come on because
something brushed against it.

The firmware arms only the lines that are actually high at the moment it sleeps,
so an encoder resting with a contact closed will not cause a wake loop — that
phase simply is not armed, and `input_prepare_sleep()` refuses to sleep at all
if nothing can be armed.

Also note C10/C11, the debounce footprints on PHA/PHB, are marked `NC` and are
not fitted. The Gray-code decoder does all the filtering. If you see spurious
detents, 10 nF in those two positions is the hardware fix.

## When the amplifier does not answer

A knob that lights up fully and then silently does nothing is worse than
one that says why. If no reply arrives within 900 ms of waking — unplugged,
out of range, or the cached channel is stale — the wake stops there:

- its own screen, not an error badge on a normal one. With no link there
  is no volume to show, no track and nothing any control would do, so
  drawing the usual furniture would be a lie
- backlight at **22%** rather than 40%, because this is a wake the user got
  nothing out of
- back to sleep in **4 s** rather than 10
- **no WiFi and no cover fetch** — a second and a half of associating buys
  nothing here
- it keeps saying hello every 250 ms, and if the amplifier turns up the
  screen switches over and comes to full brightness

Press-and-hold still sends power even with no link. The failure may be one
way, and the amplifier coming up is exactly what is being waited for.

### The keepalive that makes the indicator honest

The amplifier pushes state when it **changes**, not on a schedule. So a
quiet minute looks exactly like an unplugged amplifier, and a link
indicator built only on "when did we last hear something" would go red
four seconds into every session.

The knob therefore says hello every 1.5 s while it is awake, against a 4 s
stale window. That is five extra frames a session, which is nothing.

The catch is at the other end: the C3 forwards a hello to the Pico, and
forwarding all of those would wake a sleeping Pico five times a session
for no reason. So it only asks the Pico when its own cached state is more
than 5 s old, and answers the knob from cache otherwise.

## What a wake looks like

```
radio on, park on cached channel     ~10 ms
ESP-NOW hello, amp answers           ~5 ms
panel init, first frame              ~250 ms
backlight up
... then WiFi and the cover          ~1-2 s, in the background
```

The order is the whole design. Associating first and talking second would make
every wake feel like a second and a half, on a device whose entire job is to
answer a hand reaching for it. So control runs over ESP-NOW, which needs no
association, and WiFi is not touched until there is already something on screen.

The channel is the catch: ESP-NOW peers are registered with `channel = 0`,
meaning "whatever channel we are on", so the knob has to already be on the amp's
channel when it sends that first hello. It stores the channel it last associated
on in NVS and parks there on wake, before the radio has done anything else.
First boot uses `CONFIG_KNOB_FALLBACK_CHANNEL`, which must match
`AMP_ESPNOW_CHANNEL` on the amplifier's C3.

## Sleep

Deep sleep after **10 s** of no interaction with the amp on, or 6 s on the wake
prompt with the amp off. Backlight fades first, then the panel goes off, then
the radio, then `esp_deep_sleep_start()`.

Backlight runs at **40%**. It is by far the largest draw while awake — well
above the SoC, the radio and the panel logic combined — so this is the single
number with the most effect on how long a charge lasts.

Everything re-initialises on wake — there is no light-sleep path that keeps the
RGB panel alive, because two 460 kB framebuffers plus the cover canvas is most
of what the PSRAM is doing and none of it survives anyway.

## Album art

Text arrives over ESP-NOW because it is tiny and has to be on screen before WiFi
has even associated. The cover does not:

```
GET <CONFIG_KNOB_PI_BASE_URL>/cover.jpg?id=<art_id>
```

**The Pi must serve a 480×480 baseline JPEG.** The decoder checks the header and
refuses anything else rather than scaling it. Resampling belongs on the Pi: an
arbitrary 1400×1400 cover would need a second full-screen buffer plus a scaler
here, for a result no better than what a machine with a real CPU produces before
it ever hits the air.

`art_id` comes from the now-playing message and is just a change counter — the
knob re-fetches when it differs from what it has, and otherwise leaves the radio
alone.

## Memory

| | |
| --- | --- |
| RGB framebuffers | 2 × 460,800 B, PSRAM |
| Cover canvas | 460,800 B, PSRAM |
| JPEG staging | 192 kB, PSRAM |
| RGB bounce buffer | 480 × 30 px, internal SRAM |

The bounce buffer is not optional. Without it, decoding the cover keeps PSRAM
busy long enough to starve the RGB DMA and the whole screen tears.

## Battery

Single-cell LiPo through a divider into **GPIO4**, which is `ADC1_CH3`. ADC1 is
not a free choice: ADC2 shares its SAR block with the WiFi radio and reads fail
outright while WiFi is up — which on this device is exactly when you would want
a reading.

```
BAT+ ---[ R_top ]---+--- GPIO4
                    |
                 [ R_bot ]   ‖ 100 nF to GND
                    |
                   GND
```

**Use equal resistors, not a 1.4:1 divider.** Scaling 4.2 V up to ~3.0 V at the
pin sounds like it uses more of the ADC range, but the S3's 12 dB attenuation
curve compresses toward the top of its ~3.1 V span, so it puts a full battery in
the least accurate part of the curve — and leaves no headroom if a charger holds
the cell above 4.2 V. Equal resistors land 4.2 V at 2.1 V, in the linear region,
and still give roughly 800 counts across the usable 3.3–4.2 V range. That is far
more resolution than a percentage display can use.

**Pick the value for sleep current, not accuracy.** The divider is across the
cell permanently, and the knob is in deep sleep almost all of the time:

| Divider | Continuous drain | Per day | Verdict |
| --- | --- | --- | --- |
| 10 k / 10 k | 210 µA | 5.0 mAh | Swamps the ~15 µA deep sleep by 14×. On a 500 mAh cell that is 1%/day doing nothing. |
| **100 k / 100 k** | **21 µA** | **0.5 mAh** | Recommended. Roughly doubles sleep current, which is negligible. |
| 1 M / 1 M | 2.1 µA | 0.05 mAh | Source impedance 500 kΩ — reads low and noisy even with the cap. |

The **100 nF is not optional** at 100 k. The SAR samples onto a small internal
capacitor and needs a low source impedance to charge it; above roughly 10 kΩ of
Thévenin impedance it reads low without a local reservoir to sample against. The
firmware also throws away the first four conversions after the mux switches and
averages 32.

**Can you go above 100 k?** Yes, up to about 470 k, and then it stops working
for a different reason: the pin's own input leakage (±50 nA worst case) develops
an error across the source impedance.

| Divider | Source Z | Worst-case leakage error at the cell |
| --- | --- | --- |
| 100 k / 100 k | 50 kΩ | 5 mV — nothing |
| 220 k / 220 k | 110 kΩ | 11 mV — fine |
| 470 k / 470 k | 235 kΩ | 24 mV — borderline |
| 1 M / 1 M | 500 kΩ | 50 mV — useless; that is ~20 percentage points in the flat part of the LiPo curve |

But the gain is not worth chasing. 100 k already costs 21 µA against a deep
sleep of roughly 15–30 µA; going to 220 k saves 12 µA, on a device whose
backlight draws around 100 mA whenever it is awake. Battery life here is set by
how often you wake it and for how long, not by the divider. Pick 100 k or 220 k,
whichever is in the drawer.

Above 220 k, raise the cap to 220 nF–1 µF and consider more warm-up samples.

If you want literally zero standby drain, a small N-channel MOSFET in series
with the bottom leg, gated from a spare GPIO, does it — but at 21 µA it is not
worth the part.

**Change `VBAT_DIVIDER_PERMILLE` in `board_knob.h` and nothing else** if you pick
a different ratio. 500 means 1:1.

### Reading it

Sampled once per wake, in `app_main` before the radio, the panel or the
backlight are switched on. That is the quietest the cell will be all wake, and
therefore the only reading worth trusting — a 300 mA WiFi burst sags a small
cell by 100 mV or more through its own internal resistance, and a gauge that
drops ten points every time the cover loads is worse than no gauge.

Percentage comes from a piecewise fit to a real LiPo discharge curve, not a
linear map. A cell sits between 3.7 V and 3.9 V for most of its useful life and
then falls off a cliff; mapping 3.3–4.2 V linearly onto 0–100% reads 55% when
the cell is nearly full and 40% when it is nearly flat.

The last percentage is kept in RTC memory so it survives deep sleep, and a
three-point deadband is applied against it — otherwise the figure is recomputed
from scratch on every wake and wanders by a point or two each time. Below 10%
the gauge turns red.

Calibration uses the S3's per-chip curve-fitting data from eFuse. If that is
blank the firmware falls back to uncorrected counts and says so in the log.

## Panel bring-up

The ST7701S register sequence is bit-banged out over 9-bit SPI **before** the
RGB peripheral is created, because SPI SCK and SDO are physically shared with
RGB data bits 3 and 2. Create the RGB panel first and the init goes nowhere.

The table in `display.c` is the vendor's, verbatim, for the touch-equipped
variant of this board — including the 18 MHz pixel clock rather than the 20 MHz
used on the touchless one. Panel init sequences encode the specific glass; one
wrong byte shows up as a tint or a roll that is very hard to trace back.

## Files

| | |
| --- | --- |
| `board_knob.h` | Pins and RGB timings, from the vendor BSP. |
| `display.c` | ST7701S init, RGB bus, LVGL port, touch, backlight. |
| `input.c` | Gray-code decode, click, 2 s hold, ext0 wake arming. |
| `link.c` | ESP-NOW to the amp: keys out, state and metadata in. |
| `net.c` | WiFi station, brought up late and on purpose. |
| `art.c` | Cover fetch and JPEG decode into the PSRAM canvas. |
| `battery.c` | ADC1 sampling, calibration, LiPo curve. |
| `ui.c` | The five screens. |
| `main.c` | Wake sequence, mode selection, sleep timer. |

`../common/proto.c` is shared verbatim with the Pico and the C3.

## Pairing

The knob broadcasts `MSG_ESP_HELLO` on every wake. Whoever answers is the
amplifier — there is only one and it is the only thing that speaks this
protocol — and its MAC goes into NVS, so every wake after the first is a single
unicast frame.
