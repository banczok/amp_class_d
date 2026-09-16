# amp_class_d

Claude Code loads this file automatically at the start of every session in
this repository. The project context lives in `CONTEXT.md` and is imported
below, so it is in front of you before you touch anything.

@CONTEXT.md

## House rules

**Read `CONTEXT.md` before changing anything.** Its "Deliberate decisions"
section lists choices that were arrived at by measurement. Several were
bugs found the hard way, so reverting one silently reintroduces the bug.
If a decision there looks wrong, say so — do not quietly change it.

**`amp_ctrl/amp_ctrl.kicad_pcb` is the board.** Do not create alternate
copies of it to experiment on. An earlier scratch copy was edited as text
and ended up internally inconsistent — footprint rotations changed without
the pad angles that must follow them — which produced about 93 phantom
shorts in DRC. KiCad files are edited in KiCad.

**The relay pin wiring in the schematic is correct.** Do not re-derive it
from the KiCad files; it was got wrong twice that way. Work from the
logical sense — energised versus de-energised, which path is selected —
and let the polarity macros in `firmware/pico/include/board.h` carry it.

**`firmware/common/proto.h` is the single source of truth** for the wire
format shared by all four processors. `firmware/pi/tests/test_proto.py`
reads that header and checks the Python port against it, so drift fails a
test rather than a control that quietly stops working.

## Verifying a change

Nothing here needs hardware.

```bash
cd firmware/pi && for t in proto audio eq fingerprint service; do python tests/test_$t.py; done
```

Needs numpy only. Each script prints what it measured rather than just
passing, so read the numbers — several of them are the evidence behind
decisions in `CONTEXT.md`.

The three C trees build with their real SDKs (Pico SDK 2.0+, ESP-IDF
5.2+). Without them, they were checked with stub headers under
`-Wall -Wextra`, which catches typos and signature errors but not API
misuse.

## Style

Comments explain *why*, especially where something looks wrong but is
deliberate. Match the density of the surrounding file. British spelling in
prose, American in identifiers where an SDK already uses it.
