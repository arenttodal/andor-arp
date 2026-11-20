# ANDOR ARP — architecture

R0.1, 20 November 2026. The spec is "ANDOR ARP — product spinoff specification R0.1".

## 1. Layers

```
┌──────────────────────── ANDOR OSP (osp/, pinned commit) ────────────────────────┐
│ andor_arp_core  (pure C++20, arp-core/CMakeLists.txt)                            │
│   Arpeggiator (18 styles, CUSTOM, STRUM, swing, gate, rate, octaves, sync)       │
│   ArpCustomPattern (CONTOUR / DEGREES mapping, compiled pattern, exchange)       │
│   PatternCapture, ArpPatternModel (interpret, edit, undo), ArpFactoryPatterns    │
│   ArpPatternCodec (.andorpattern JSON, no JUCE), HostTiming, Prng                │
└──────────────────────────────────────────────────────────────────────────────────┘
                 │ linked by osp_dsp (ANDOR OSP, ANDOR ANALOG) and by ↓
┌──────────────────────── ANDOR ARP (this repository) ────────────────────────────┐
│ shared/  (pure C++20, andor_arp_shared)                                          │
│   HostAdapterContract.h   what a host gives the core and gets back               │
│   ThreeLaneCoordinator    lanes A/B/C over one input: mute, register, ownership   │
│   PerformanceModel        lane names, settings, embedded patterns, JSON          │
│   LinkProtocol            Master/Receiver messages, reconciliation, health       │
│   StateBlob               saved state as integers (Live Set parameter storage)   │
│   TripleBuffer, MidiBytes main-thread → real-time hand-over; raw MIDI parsing    │
│ max/external/                                                                    │
│   MaxSchedulingAdapter    Max scheduler + ITM → gap-free blocks (pure, tested)   │
│   andor.arp.engine.cpp    the Max SDK adapter (only glue)                        │
│ juce/ (deferred)          an instrument host with three hosted instruments       │
└──────────────────────────────────────────────────────────────────────────────────┘
```

Rules that keep this honest:

- **One arpeggiator.** Every note any ANDOR product plays from a chord comes from OSP's
  `osp::Arpeggiator`. The coordinator adds lane mechanics (mute, transposition, register,
  ownership, pass-through), never musical logic. A test proves each lane equals a bare OSP
  arpeggiator event for event (`[lanes]` "each exactly OSP's arpeggiator").
- **No copies.** `osp/` is a git submodule at a pinned commit; `andor_arp_core` is built from it
  with `add_subdirectory(osp/arp-core)`. The core's own tests (the 18-style golden included) run in
  this repository's ctest, so a core update is checked here as well as in OSP.
- **Headless first.** Everything except the 700-line Max glue is pure C++ and tested here.

## 2. The shared core (spec §2) — extracted in OSP

See OSP `docs/architecture.md` §"andor_arp_core". Decisions:

- The ARP sources were already pure C++ inside `osp_dsp`; only the `.andorpattern` codec depended
  on JUCE. The extraction therefore moved no files: `arp-core/CMakeLists.txt` owns six sources and
  `osp_dsp` links the target. No namespace, parameter index, plugin ID or stored format changed.
- The codec's meaning moved into `model/ArpPatternCodec` (pure). `io/ArpPatternJson` translates
  `juce::var` only. OSP's text output was checked byte-identical against the unmodified codec for
  the whole corpus (factory patterns plus edge documents).
- An include check fails the build's tests if a core file includes anything outside the core.

## 3. The host contract (spec §3)

`shared/HostAdapterContract.h`. The host supplies, per block: `HostTiming` (valid, playing, bpm,
ppq, signature), the block length on a sample clock, input note/pedal/All Notes Off events with
sample offsets, and settings/patterns copied in between blocks. The core supplies note events per
lane with sample offsets, channels and velocities, per-lane status, and an overflow count. The
contract's one subtle requirement: **ppq must be continuous** between consecutive blocks (within 64
samples) unless the host really jumped, because the core re-aligns to its grid on any
discontinuity. Plugin hosts satisfy this naturally; Max for Live does not, hence
`MaxSchedulingAdapter` (§6).

## 4. Three lanes (spec §4)

`ThreeLaneCoordinator` owns three `osp::Arpeggiator`s. All hear the same input events and the same
transport. Per lane: `LaneSettings` (OSP's `Arpeggiator::Settings` plus enabled/mute, register
low/high, transpose, output channel, pass-through, seed) and an optional compiled custom pattern.

- **Identity:** `Lane::A/B/C` (stored as 0/1/2). MIDI channels are never used for routing; the
  output channel is only what the lane's instrument receives (1 by default).
- **Mute keeps the phase:** a lane's arpeggiator always runs (`Settings::enabled` is forced on so
  OSP's hand-over to the keys never happens); a muted lane ends its notes and emits nothing; unmuted,
  it rejoins on the next step of the same grid.
- **Pattern and setting changes** follow the arpeggiator's own rules: a new custom pattern takes over
  at the next step with the phase kept; a style or rate continues from the current step. A change of
  transposition, register or channel ends the lane's sounding notes first (they were started under
  the old mapping).
- **No stuck notes:** every output pitch counts its owners; a pitch reached twice (folding,
  transposition, pass-through) retriggers and is released only when its last owner ends.
  `releaseAll()` ends everything (stop, bypass, unload). A 6000-block random performance (styles,
  rates, patterns, mutes, register, pass-through, transport jumps, tempo, pedal, panic) ends with
  every pitch balanced and no allocation in `process()`.
- **Seeds:** stored per lane (64-bit, as text in JSON). Defaults follow OSP's cards (lane A the
  seed, B and C `Prng::deriveSeed (seed, lane, 'Lane')`), so a setup made in ANDOR OSP replays the
  same random choices. The transport's start restarts every lane's pattern and random phrase, as in
  OSP, so bounces repeat.
- **Max Receiver vs JUCE host:** a Receiver runs one lane (`setActiveLanes (1 << lane)`); the JUCE
  variant would run all three in one processor. A test proves a single-lane instance plays exactly
  that lane of the three-lane engine, whatever the block sizes.

## 5. Pattern documents (spec §5)

`.andorpattern` stays the one library format (kind `andor.arp-pattern`, schemaVersion 1, unchanged).
ANDOR OSP and ANDOR ANALOG write it through JUCE; ANDOR ARP reads and writes it with the same codec
(`ArpPatternCodec`), without JUCE. Compatibility is proven both ways in OSP (`[arp-codec-compat]`)
and against the byte-exact fixtures in `osp/arp-core/tests/fixtures` (read by the standalone core
tests, rendered through the arpeggiator to show the musical interpretation is the same). Newer
schemas are refused. Notes for implementers: OSP writes CRLF line ends on every platform (JUCE's
`replaceWithText`) and six-decimal numbers; every reader accepts LF/CRLF and a UTF-8 BOM.

A performance (`.andorperformance`, kind `andor.arp-performance`, schemaVersion 1) holds three lanes
with their patterns embedded, so a Live Set or preset never depends on a library file.

## 6. Max for Live (spec part B)

See `docs/M4L_IMPLEMENTATION.md`. Summary:

- One MIDI track: ANDOR ARP Master (MIDI effect, passes MIDI on untouched) → Instrument Rack with
  three chains, each `ANDOR ARP Receiver (lane)` → instrument.
- Each Receiver runs `andor.arp.engine` (C++ external) for its lane from the MIDI its chain
  receives; nothing note-related travels between devices.
- **Timing:** `MaxSchedulingAdapter` renders "just in time" on Live's timeline with a fixed output
  latency (4 ms default); positions stay continuous; real loops and locates re-anchor; small drift
  is removed by an inaudible tempo correction. In a model of Max's scheduler (stale ITM readings,
  jitter, stalls, drift, loops, tempo changes) no note is lost, doubled or left hanging, and notes
  land within two scheduler vectors of a plugin-accurate reference (`docs/results/timing-simulation.md`).
  **Live itself has not been measured yet** (M4L-T1 in the test matrix).
- **Link:** versioned messages on a channel named from the Rack's Live identity (track id + rack id,
  found through the Live API), acknowledged and retried; revisions decide which state wins; a
  Receiver keeps its saved lane without a Master. Tested in simulation, including duplicated Racks
  and lost messages.
- **Persistence:** each device's state as a checksummed integer blob through the pattr
  `getvalueof/setvalueof` interface, to be stored by a parameter-enabled `[pattr]` (blob, "Stored
  Only"). **To be confirmed in Live.**

## 7. JUCE instrument host (spec part C)

Deferred behind its feasibility gate; see `docs/JUCE_IMPLEMENTATION.md`. The same coordinator would
drive three hosted instruments in one processor.

## 8. Versions

- Product: `VERSION` (0.1.0-dev), compiled into `BuildInfo.h` with this repository's commit.
- Core: `osp::arp::coreVersion` (1.0.0) and the exact OSP commit (`build::ospCoreCommit`, with
  "-modified" when the submodule has local changes), reported by the external's `dump`.
- Stored formats: `.andorpattern` 1, `.andorperformance` 1, receiver/master state 1, state blob 1,
  link protocol 1.
