# amp_c3 — on-board ESP32-C3 firmware

ESP-NOW endpoint for the battery knob remote. Runs on the ESP32-C3 SuperMini
soldered to the `amp_ctrl` board, powered from **+5VA** — the always-on rail —
so it is alive whether or not the amplifier is.

## Build

```bash
idf.py set-target esp32c3
```

```bash
idf.py menuconfig
```

Set your SSID and password under **Preamp controller (ESP32-C3)**, then:

```bash
idf.py build flash monitor
```

ESP-IDF v5.0 or newer. The console comes out over the built-in USB Serial/JTAG
on the same USB-C socket you flash from — UART0 is taken by the Pico link.

## What it does

Two jobs, and only two:

1. Be the ESP-NOW endpoint for the knob, so a device that is asleep 99% of the
   time can change the volume in a few milliseconds and turn the amplifier on
   from cold.
2. Cache the last amp state and the last now-playing blob, so a knob that has
   just woken up has something to draw immediately instead of waiting for a
   round trip through the Pico and the Pi.

It decides nothing. Every key it receives goes straight to the Pico, which owns
the state. If this chip dies, the amplifier loses its remote knob and nothing
else — encoder, IR and the front-panel button are unaffected.

## Wiring

| Pico | C3 | |
| --- | --- | --- |
| GP4 (TX) | GPIO20 / U0RXD | commands in |
| GP5 (RX) | GPIO21 / U0TXD | state out |

115200 8N1, same framing as everything else (`common/proto.h`).

## Why ESP-NOW for control and WiFi for artwork

A WiFi association costs roughly a second and a couple of hundred millijoules;
an ESP-NOW frame costs a few milliseconds. Turning the volume has to feel
instant, so control goes over ESP-NOW. Album art does not, so that goes over
WiFi — on the knob, in its own time, once it is already awake and showing
something.

The catch is that both have to happen on the same radio channel. So this side
joins the house WiFi like an ordinary station and lets ESP-NOW ride on the STA
interface. Peers are registered with `channel = 0`, meaning "whatever channel we
are on now", so an AP channel change fixes itself.

If no SSID is configured, or the AP is down, it parks on
`CONFIG_AMP_ESPNOW_CHANNEL` so a knob that also falls back can still reach it.
**The amplifier stays controllable when the network is not.**

## Why it never sleeps

It has to hear an ESP-NOW frame that could arrive at any moment, and there is no
second path to it — this radio is what wakes the amplifier. An unassociated or
dozing station simply drops those frames, so `WIFI_PS_NONE` and the receiver
stays on. CPU is pinned at 80 MHz to claw some of it back.

That is roughly 70 mA on a rail that is mains-derived. The deep-sleep cycle in
this system belongs to the battery knob, which is the thing that actually runs
out of charge.

## Waking the Pico

In standby the Pico drops `clk_sys` to its 12 MHz crystal and hands UART RX to
the GPIO block so an edge can wake it — which means the byte that does the
waking is consumed by the wake and never reaches a UART.

So: if nothing has been heard from the Pico for 2 s, assume it is asleep, send
eight `MSG_ESP_WAKE` frames, wait 40 ms for it to restore its clocks and re-init
the UART, then send the frame that matters. Sending the preamble unnecessarily
costs nothing — the Pico ignores `MSG_ESP_WAKE` when it is already awake.

The periodic state refresh is deliberately suppressed while the Pico is asleep.
Polling it every few seconds would wake it every few seconds and throw away the
entire point of the standby mode.

## Pairing

The knob broadcasts `MSG_ESP_HELLO` on every wake. The first one to arrive is
adopted, its MAC stored in NVS, and it is answered with state plus now-playing.
Call `radio_forget_knob()` to re-pair with a different one.

Optional ESP-NOW encryption (`CONFIG_AMP_ESPNOW_ENCRYPT`) uses a compile-time
PMK/LMK pair that both ends must share. Broadcast discovery is never encrypted,
so pairing works either way.

## Messages

| From knob | |
| --- | --- |
| `MSG_ESP_HELLO` | I am awake. Pairs, replies with state + now-playing, nudges the Pico. |
| `MSG_ESP_REMOTE` | `{fn_id, repeat}`. Forwarded verbatim to the Pico. |

| To knob | |
| --- | --- |
| `MSG_ESP_STATE` | `{power, vol_code, muted, input, eq_path, max_vol}` |
| `MSG_ESP_NOW` | now-playing blob, originated by the Pi |

Only pushed while the knob is believed awake (heard from within 10 s) — an
unacknowledged unicast to a sleeping receiver is just wasted airtime.
