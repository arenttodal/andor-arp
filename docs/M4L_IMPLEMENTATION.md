# ANDOR ARP for Max for Live — implementation

R0.1, 20 November 2026. Status per item: **built** (code exists and is tested headlessly), **compiled**
(builds against the Max SDK, not run in Max), **designed** (specified here, not built), **to verify
in Live** (an assumption Live must confirm before shipping).

## 1. The one-track structure (spec §6) — designed

```
ONE MIDI TRACK
├── ANDOR ARP Master (.amxd, MIDI effect)
│     [midiin] → [andor.arp.engine @role master] → [midiout]   MIDI passes on untouched
│     UI: header, three performance strips, CAPTURE, contextual popovers
└── Instrument Rack ("ANDOR ARP - 3 Instruments.adg")
      ├── Chain A: ANDOR ARP Receiver (lane A) → instrument
      ├── Chain B: ANDOR ARP Receiver (lane B) → instrument
      └── Chain C: ANDOR ARP Receiver (lane C) → instrument
```

- The Master is a MIDI effect that consumes nothing: every MIDI byte goes on (Capture listens).
- Every chain of an Instrument Rack receives the track's MIDI. Each Receiver
  (`[midiin] → [andor.arp.engine @role receiver @lane N] → [midiout]`) replaces the note stream
  with its lane's arpeggio: note-on/off, CC64 and CC123/120 are taken; all other MIDI (pitch bend,
  pressure, mod wheel, other CCs) passes on untouched to the instrument. Pass-through of the held
  keys is a per-lane option (off by default), so the original chord never leaks by accident.
- Receivers are small (a status light and the lane letter); the musician works in the Master.
- The Rack's chain selector, volume and pan stay Live's own.

**To verify in Live (M4L-R1):** that every chain receives the track's MIDI including notes from
the keyboard and from clips, and that a MIDI-effect device at the start of a chain can replace
the notes before the chain's instrument. Both are standard Live behaviour, but the spec requires the
complete workflow to be demonstrated.

## 2. The external `andor.arp.engine` (spec §7) — compiled, not yet run in Max

`max/external/andor.arp.engine.cpp`: a Max SDK adapter over the shared, tested classes. Inlet: raw
MIDI bytes from `[midiin]`, messages. Outlets: MIDI bytes for `[midiout]`, link messages for the
Rack's `[send]`, status. Attributes: `@role master|receiver`, `@lane 0..2`, `@latency` (ms),
`@stopreleases`.

Threads (Live runs Max with Overdrive and Scheduler in Audio Interrupt):

| Work | Thread | Notes |
|---|---|---|
| MIDI bytes in, parse, queue | scheduler (audio) | fixed arrays; foreign MIDI passed on at once |
| `engineService` (every scheduler tick: clock re-armed at 1 ms) | scheduler | ITM read, `MaxSchedulingAdapter`, `ThreeLaneCoordinator`, output queue |
| Output (`clock_fdelay` to each note's time) | scheduler | 8192-slot queue, dropped notes counted |
| Link messages, edits, files, Capture interpretation, saved state | main (`defer_low`, `qelem`) | JSON, symbols, dictionaries live here only |
| main → scheduler | lock-free triple buffer per lane (settings + compiled pattern) | |

Timing-critical work never runs in JavaScript. JavaScript is used only for pairing (§4) and the UI.

Clock: Max's logical time (`clock_getftime`). In Live it advances with the audio, so "now" carries
no wall-clock jitter; the simulation's jitter is a pessimistic bound. The core's clock is a virtual
48 kHz (it only needs consistent sample counting); it is independent of Live's sample rate.

### Timing adapter (`MaxSchedulingAdapter`) — built, tested in a model

The core expects plugin-style blocks. The adapter:

1. renders, at each service, the stretch of Live's timeline since the last one ("just in time")
   and schedules each note at its exact time plus a fixed latency (4 ms) — scheduler jitter moves
   rendering, not notes;
2. applies MIDI that arrived since the last service at the block's start: never after the grid
   line it came with (a clip's chord change on a bar line must play on that line; an earlier
   render-ahead design played one step of the old chord — found by the simulation);
3. keeps the core's position continuous; compares the ITM readings (which can only lag) with it
   and trusts the most advanced of the last 30 ms; re-anchors on a real jump (more than 1/32 note
   and 40 ms); otherwise removes the error with a tempo correction ≤ 0.5 % (≤ 3 % for half a second
   after a re-anchor).

Results of the model (`docs/results/timing-simulation.md`, reproducible with
`andor-arp-timing-report`): no missing, doubled or hanging note in any scenario (stale readings,
2–5 ms service jitter, 20 ms stalls, 100 ppm drift, tempo change, 2-bar loop with triplets and
swing, 1/32 at 48 kHz); timing within two scheduler vectors (~3 ms) of a plugin-accurate reference
after the 4 ms latency, except during stalls longer than the latency.

**To verify in Live (M4L-T1..T4):** the real behaviour of ITM readings in Live (staleness, update
granularity), of `clock_fdelay` resolution, of Live's handling of MIDI from a device in a chain
(does Live time-stamp it at the scheduler tick, or quantise it to the audio buffer?), and the
resulting timing against a reference render. Method: §8.

## 3. Master/Receiver link (spec §8) — built, tested in simulation

`shared/LinkProtocol.{h,cpp}`; messages carried by Max `[send]`/`[receive]` on a channel name made by
`link::channelName (rackKey)` = `andor.arp.v1.<track id>-<rack id>`.

- **What travels:** link identity, lane, revision, lane state (settings + embedded pattern, as JSON),
  acknowledgements, ping/pong. Never notes.
- **Payloads** (a lane's JSON, a few KB) travel in a registered Max dictionary
  (`andor.arp.payload.<instance>`); the message carries only its name. Max symbols are interned for
  the life of the process, so sending JSON as a symbol would grow memory with every edit.
- **Handshake:** a Receiver says `hello` (lane, stored link id, revision, has-state) every 500 ms
  until it hears the Master. The Master decides (`reconcile`): in sync (nothing travels), push its
  state (acknowledged, retried 5× at 400 ms), or pull the Receiver's (the Receiver's is newer, or the
  Master has none: a new Master never erases saved patterns). An older state of the same link never
  replaces a newer one.
- **Health:** the Master pings every 2 s; a Receiver silent for 6.5 s is gone; two Receivers on one
  lane show as "duplicate". A Receiver that loses its Master keeps playing its lane and looks for it
  again.
- **Tested (`[link]`):** a new Rack pairs and syncs; edits reach the right lane only; reopening a Set
  with mixed revisions (equal / newer Receiver / older Receiver) keeps the newest; Receivers without
  a Master keep their patterns and a fresh Master adopts them; a duplicated Rack is independent; 30 %
  message loss converges; a removed Receiver and a duplicated chain are reported.

## 4. Pairing and duplicate Racks (spec §8) — designed (script written), to verify in Live

`max/shared/andor-arp-pairing.js` (Max JS, low priority, after `[live.thisdevice]`):

- a Receiver walks up the Live Object Model from `this_device` to its Instrument Rack
  (`InstrumentGroupDevice`) and that rack's track;
- the Master finds the first Instrument Rack after itself in its track's device list;
- both send `track <track id>-<rack id>` to the external, which sets the channel.

Duplicating a track or a Rack creates new runtime ids, so the copy pairs with itself: no cross-talk
(the shared saved link id is harmless because channels differ). Two Racks on one track are kept
apart by the rack id. Nothing here is saved: ids are found again at each load. No UUID or address is
ever shown; the Master shows "Receiver missing / duplicate / not responding" per lane.

**Fallback (if the LOM walk proves unreliable, M4L-P3):** one action — the Master's "Pair" button
broadcasts a fresh rack key on a discovery channel; the Receivers that are in the same Rack (the
only fact the fallback needs from the LOM) adopt it.

**To verify in Live (M4L-P1..P4):** `LiveAPI.type` names ("Track", "RackDevice"), `class_name`
"InstrumentGroupDevice", `canonical_parent` from a device inside a chain, and whether
`[live.thisdevice]` re-fires when a device is moved to another track (if not, re-pair on the
Master's pings: a Receiver that hears no Master for 6.5 s re-runs pairing).

## 5. Persistence (spec §9) — built (format), to verify in Live

Each device's state is JSON (`andor.arp-receiver-state` / `andor.arp-master-state`, schemaVersion 1)
encoded by `StateBlob` into integers (3 bytes each, exact even through 32-bit floats; magic, version,
length and CRC-32; damaged, truncated or newer data refused). The external implements pattr's
`getvalueof` / `setvalueof`.

Device wiring to test: `[pattr ---state @bindto <engine's scripting name> @parameter_enable 1
@parameter_type blob @parameter_visibility stored-only]` (names per Max 8/9's inspector:
Parameter Mode Enable, Type = Blob, Visibility = Stored Only). The spec's warning stands: do not
assume pattr persists without correct parameter configuration — **M4L-S1..S4** test exactly this.

Load order (spec §9): the Receivers restore their lanes and play them (no Master needed) → the
Master restores its performance → pairing → `hello` / `reconcile` by revision → in sync. Active
performance resumes immediately on the Receivers (they hold their own state), which satisfies
"a missing Master must not cause Receivers to lose their saved patterns".

## 6. The Master's interface (spec §10) — designed

Compact (a MIDI-effect device height, ~170 px), ANDOR's warm ceramic / dark display language:

- Header: `ANDOR ARP`, master enable, performance preset menu, settings/help.
- Three strips (A/B/C): letter, editable name, enable, pattern (the 18 styles + CUSTOM: <name>),
  rate, octaves, gate, a 16-step picture (the lane's real output, as OSP's display), Receiver status
  light. One strip expands at a time for: CONTOUR/DEGREES, swing, STRUM, velocity, register,
  transpose, pass-through, seed, pattern library.
- CAPTURE: arm → play a phrase → STOP (RHYTHM or STEPS) → preview on the lane → APPLY / SAVE. The
  engine side exists (`capture start|stop <lane> [rhythm|steps]|cancel`, OSP's `PatternCapture` and
  `interpret`). PREVIEW and the editor (OSP's `CustomPatternEditor` is JUCE UI, not reusable in Max)
  are **not built**.
- Lists (styles, rates, mappings) come from the core's own names (`osp::arp::patternName` etc.), not
  duplicated in the patcher.

Gate M4L: the interface is not polished before the one-track Rack works reliably in Live.

## 7. Deliverables (spec §11) — status

| Deliverable | Status |
|---|---|
| `andor.arp.engine` (.mxo universal) | CI builds it unsigned on macOS (artifact); never loaded in Max yet |
| Windows `.mxe64` | CMake path written; no Windows build run |
| `ANDOR ARP Master.amxd`, `ANDOR ARP Receiver.amxd` | not built (need Max on a Mac); specs in `max/master`, `max/receiver` |
| `ANDOR ARP - 3 Instruments.adg`, example Set | not built (need Live); stock instruments only (e.g. Drift, Electric, Wavetable) |
| Three example performances | `presets/*.andorperformance` |
| Installation guide, compatibility report | drafts in `docs/RELEASE_RUNBOOK.md`, `docs/TEST_MATRIX.md` |
| Freezing with the external | to verify (M4L-D1): a frozen .amxd embeds dependencies; the .mxo must be signed and notarised for Gatekeeper |

## 8. Measuring timing in Live (M4L-T1) — method

1. One MIDI track: Master → Rack, three chains with Receivers → each chain's instrument replaced by
   a `[midiout]`-recording setup: route each chain's MIDI to a separate recording MIDI track (only
   for the measurement; the product never needs it) or record with Max `[midiparse]` +
   `[cpuclock]` logging.
2. Play a fixed clip (`max/tests/fixtures`, to be made: a 4-bar chord progression with changes on
   bar lines and a held chord) at 120 BPM, 44.1 and 48 kHz, buffer 64/256/1024, with loops on and a
   tempo change.
3. Export the recorded MIDI; compare with `andor-arp-timing-report`'s reference (the same core, ideal
   blocks) note by note: missing / extra / deviation statistics.
4. Record the figures in `docs/TEST_MATRIX.md`. Ship only within the tolerance agreed then.
