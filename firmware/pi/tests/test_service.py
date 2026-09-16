"""Does the service actually leave things switched off?

Every claim in this project about the analogue path being affordable rests
on three policies, and all three are easy to get quietly wrong:

  * the audio stream is off unless something needs it
  * fingerprinting happens at the start of music, not on a timer
  * a streamer that knows the title stops anything from listening

None of those announce themselves when broken.  A stream left running is
52% of the UART gone; a fingerprinter on a timer is hundreds of lookups a
day.  So they are tested against a fake link that records every command.
"""

import asyncio
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from ampd import proto                                      # noqa: E402
from ampd.config import Config                              # noqa: E402
from ampd.service import Service                            # noqa: E402
from ampd.sources import NowPlaying                         # noqa: E402


class FakeLink:
    """Records what the service asks the Pico to do."""

    def __init__(self):
        self.cmds = []
        self.nowplaying = []
        self.started = False

    async def start(self):
        self.started = True

    async def stop(self):
        pass

    def on(self, *_):
        pass

    async def send(self, raw):
        pass

    async def cmd(self, cmd, value=0):
        self.cmds.append((int(cmd), int(value)))

    async def nowplaying_(self, *a):
        pass

    async def audio_stream(self, on, stereo=True, seconds=0.0):
        self.cmds.append(("stream", bool(on), bool(stereo), seconds))

    async def menu_key(self, k):
        pass

    def stats(self):
        return {}


async def _nowplaying(self, playing, art_id, artist, title, album):
    self.nowplaying.append((playing, art_id, artist, title, album))


FakeLink.nowplaying = _nowplaying


def state_payload(power=2, input_sel=proto.IN_ANALOG, vol=150):
    return bytes([power, vol, 0, 0, input_sel, 0,
                  0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 3, 192])


def meter_payload(level=200):
    return bytes([level, level, level - 20, level - 20]) + \
        bytes([level] * 32) + bytes([level] * 32)


def streams(link):
    return [c for c in link.cmds if c and c[0] == "stream"]


async def main() -> int:
    fails = []
    cfg = Config()
    cfg.dir = os.path.join(os.path.dirname(__file__), "_svc")
    cfg.fingerprint_provider = "off"
    cfg.linkplay_host = ""

    svc = Service(cfg)
    svc.link = FakeLink()

    # ---- 1. digital input: the stream must never come on --------------
    svc.state.amp = proto.AmpState.parse(state_payload(
        input_sel=proto.IN_DIGITAL))
    for v in svc.registry.all():          # even with every wave mode on
        v.enabled = True
    await svc._apply_stream_policy()
    on, _ = svc._want_stream()
    print(f"digital, all modes enabled -> stream {'ON' if on else 'off'}")
    if on:
        fails.append("the audio stream came on for the digital input")

    # ---- 2. analogue, no wave mode: still off -------------------------
    svc.state.amp = proto.AmpState.parse(state_payload(
        input_sel=proto.IN_ANALOG))
    for v in svc.registry.all():
        v.enabled = v.id in ("vu", "bars", "led")     # level and bands only
    await svc._apply_stream_policy()
    on, _ = svc._want_stream()
    print(f"analogue, no wave mode    -> stream {'ON' if on else 'off'}")
    if on:
        fails.append("the stream came on with nothing needing samples")

    # ---- 3. analogue with a wave mode: on, and stereo -----------------
    svc.registry.get("scope").enabled = True
    await svc._apply_stream_policy()
    on, stereo = svc._want_stream()
    print(f"analogue, scope enabled   -> stream "
          f"{'ON' if on else 'off'}{' stereo' if stereo else ''}")
    if not on or not stereo:
        fails.append("a wave visualiser did not get a stereo stream")

    # ---- 4. amp off: nothing streams ---------------------------------
    svc.state.amp = proto.AmpState.parse(state_payload(power=0))
    await svc._apply_stream_policy()
    on, _ = svc._want_stream()
    print(f"amp in standby            -> stream {'ON' if on else 'off'}")
    if on:
        fails.append("the stream ran with the amplifier off")

    # ---- 5. fingerprinting never runs on the digital input ------------
    svc2 = Service(cfg)
    svc2.link = FakeLink()
    svc2.state.amp = proto.AmpState.parse(state_payload(
        input_sel=proto.IN_DIGITAL))
    for _ in range(400):
        svc2._on_meter(proto.Frame(proto.Msg.METER, meter_payload()))
    # A fresh watcher is armed on purpose - it identifies the first
    # thing it hears - so "armed" proves nothing.  What matters is that it
    # was never fed: its averages stay unset and its clock never advances.
    fed = svc2.watcher._fast is not None or svc2.watcher._t > 0
    print(f"digital, 400 meter frames -> watcher fed: {fed} "
          f"(it must not be)")
    if fed:
        fails.append("the watcher was fed from the digital input")

    # ---- 6. on analogue it is fed, and arms once ----------------------
    svc3 = Service(cfg)
    svc3.link = FakeLink()
    svc3.state.amp = proto.AmpState.parse(state_payload(
        input_sel=proto.IN_ANALOG))
    for _ in range(400):
        svc3._on_meter(proto.Frame(proto.Msg.METER, meter_payload()))
    fed3 = svc3.watcher._fast is not None and svc3.watcher._t > 0
    print(f"analogue, 400 frames      -> watcher fed: {fed3}")
    if not fed3:
        fails.append("the watcher was not fed by the analogue input")

    # ---- 7. a WiiM that answers stops the fingerprinter ---------------
    cfg2 = Config()
    cfg2.dir = cfg.dir
    cfg2.linkplay_host = "10.0.0.9"
    cfg2.fingerprint_provider = "off"
    svc4 = Service(cfg2)
    svc4.link = FakeLink()
    svc4.state.amp = proto.AmpState.parse(state_payload(
        input_sel=proto.IN_ANALOG))

    calls = {"identify": 0}

    async def fake_poll():
        return NowPlaying(artist="Elbow", title="Weather to Fly",
                          playing=True, origin="linkplay", exact=True)

    async def fake_identify(_getter):
        calls["identify"] += 1
        return None

    svc4.linkplay.poll = fake_poll
    svc4.fingerprint.maybe_identify = fake_identify
    svc4.watcher.force()

    task = asyncio.create_task(svc4._fingerprint_loop())
    await asyncio.sleep(1.6)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    print(f"WiiM answering            -> fingerprint attempts: "
          f"{calls['identify']}")
    if calls["identify"]:
        fails.append("the fingerprinter ran while the WiiM had the answer")

    # ---- 8. with no WiiM it does get a turn --------------------------
    svc5 = Service(cfg)
    svc5.link = FakeLink()
    svc5.state.amp = proto.AmpState.parse(state_payload(
        input_sel=proto.IN_ANALOG))
    calls2 = {"n": 0}

    async def fake_identify2(_getter):
        calls2["n"] += 1
        return None

    svc5.fingerprint.maybe_identify = fake_identify2
    task = asyncio.create_task(svc5._fingerprint_loop())
    await asyncio.sleep(1.6)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    print(f"no WiiM                   -> fingerprint attempts: {calls2['n']}")
    if not calls2["n"]:
        fails.append("the fingerprinter never ran with no other source")

    # ---- 9. the source poll rate is bounded --------------------------
    lp = svc4.linkplay
    import ampd.sources as S
    real = S.LinkPlaySource.poll
    lp2 = S.LinkPlaySource("10.0.0.9", poll_interval=2.0)
    hits = {"n": 0}

    async def counting_get(_cmd):
        hits["n"] += 1
        return None

    lp2._get = counting_get
    for _ in range(50):
        await real(lp2)
    print(f"50 polls in a burst       -> {hits['n']} network requests "
          f"(2 s interval)")
    if hits["n"] > 2:
        fails.append(f"{hits['n']} requests from 50 polls; not rate limited")

    for f in fails:
        print("FAIL:", f)
    print("\nall checks passed" if not fails else f"\n{len(fails)} FAILURES")

    import shutil
    shutil.rmtree(cfg.dir, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
