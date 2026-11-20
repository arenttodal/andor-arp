# ANDOR ARP — test matrix

Every claim about a host is recorded here with the host, version, date and result. **"Not run"
means no claim.** Updated 20 November 2026.

## 1. Headless (this repository's ctest; CI on Ubuntu and macOS)

| Area | Tests | Result (Linux, GCC 13, 20 Nov 2026) |
|---|---|---|
| Core: the 18 styles' frozen events | `arp-core-regression` (OSP's golden, standalone) | pass |
| Core: STRUM, capture, factory patterns, codec, fixtures written by ANDOR OSP | `arp-core-unit` | pass |
| Core: includes nothing outside itself | `arp-core-includes` | pass |
| Lanes: each lane = OSP's arpeggiator, event for event | `[lanes]` | pass |
| Lanes: no original-chord leak; pass-through opt-in | `[lanes]` | pass |
| Lanes: mute ends notes, others untouched, rejoin in phase | `[lanes]` | pass |
| Lanes: Receiver (one lane) = that lane of the 3-lane engine, any block sizes | `[lanes]` | pass |
| Lanes: register/transposition, shared pitches, releaseAll, lane removal | `[lanes]` | pass |
| Lanes: block-size independence; restart and reload replay random styles | `[lanes]` | pass |
| Lanes: 6000-block random performance: balanced notes, no allocation | `[lanes]` | pass |
| Performance JSON, presets, 64-bit seeds, newer schema refused | `[performance]`, `[presets]` | pass |
| State blob (checksum, float-exact, refusals) | `[blob]` | pass |
| Link: atoms, reconciliation, pairing, reopen, no Master, duplicate Rack, loss, removal | `[link]` | pass |
| Max scheduling adapter: continuity, jumps | `[max-scheduling]` | pass |
| Scheduler model: 11 scenarios, no missing/extra/hanging notes, ≤ 2 vectors | `[timing]` | pass (`docs/results/timing-simulation.md`) |
| `andor.arp.engine` compiles against the Max SDK | Linux object (LINUX_VERSION headers) | pass; macOS .mxo: CI |

In ANDOR OSP (branch `claude/andor-arp-spec-xy4umx`): full ctest, `[arp-codec-compat]`, OSP's ARP
suites (2.4 M assertions) — pass; ANDOR ANALOG `analog_dsp_tests` + arp-core tests against it — pass.

## 2. Ableton Live / Max for Live — NOT RUN

| ID | Test (spec §12) | Result |
|---|---|---|
| M4L-R1 | One MIDI track: all three chains receive the original chord (keyboard and clip) | not run |
| M4L-R2 | Each Receiver plays its own lane; no original-chord leak; all lanes together | not run |
| M4L-R3 | Mute/unmute; pattern change with notes held | not run |
| M4L-R4 | Start/stop, loops, jumps, tempo changes: no hanging notes, in sync | not run |
| M4L-R5 | Swing and triplets | not run |
| M4L-R6 | Capture from the keyboard | not run |
| M4L-T1 | Timing vs the reference render (method: M4L_IMPLEMENTATION §8) | not run |
| M4L-T2 | ITM staleness / update granularity in Live | not run |
| M4L-T3 | `clock_fdelay` resolution in Live (buffer 64…1024) | not run |
| M4L-T4 | How Live time-stamps MIDI from a device inside a chain | not run |
| M4L-P1 | Pairing: LOM types/ids from inside a chain | not run |
| M4L-P2 | Duplicated Rack / duplicated track independent | not run |
| M4L-P3 | Two Racks on one track | not run |
| M4L-P4 | Device moved to another track re-pairs | not run |
| M4L-S1 | pattr blob (parameter mode, Stored Only) saved with the Set | not run |
| M4L-S2 | Save, close, reopen: every lane restored, Master reconciles | not run |
| M4L-S3 | Master deleted: Receivers keep playing; new Master adopts | not run |
| M4L-S4 | Receiver removed and restored (undo) | not run |
| M4L-I1 | Third-party instruments on the chains (Kontakt, Serum, SWAM) | not run |
| M4L-D1 | Frozen devices with the signed external open on a clean Mac | not run |
| M4L-W1 | Windows external | not built |
| M4L-C1 | No timing-critical work in JavaScript (by construction: only pairing/UI in JS) | design |

## 3. JUCE instrument host — NOT STARTED

All items of spec §17: not started (`docs/JUCE_IMPLEMENTATION.md`). No DAW × instrument combination
has been tested.
