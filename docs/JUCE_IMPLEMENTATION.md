# ANDOR ARP as a plugin (JUCE) — feasibility plan

R0.1, 20 November 2026. **Status: deferred, nothing built or tested.** The spec puts the Ableton
prototype first; this document is the audit and the gate plan, so the work can start without
re-research. No DAW or third-party instrument has been tried. Nothing here claims that nested
hosting works.

## 1. Why a host, not a MIDI effect

A plugin MIDI effect has one MIDI output. Three instruments on one track therefore need either the
DAW's own container (Ableton's racks, Bitwig's Instrument Layer, Reaper's FX container with MIDI
routing, Logic's Track Stacks — each to be tested before any claim), or ANDOR ARP hosting the three
instruments itself (option B of the spec), which is what this plan covers.

## 2. Architecture (option B)

```
ANDOR ARP (VST3 instrument, later AU) — one processor
├── ThreeLaneCoordinator (the same shared code as the Max Receivers)
├── Slot A: AudioPluginInstance (hosted VST3/AU) ← lane A's MidiBuffer
├── Slot B: AudioPluginInstance                  ← lane B's MidiBuffer
├── Slot C: AudioPluginInstance                  ← lane C's MidiBuffer
└── stereo sum (per-slot gain/pan) → output
```

- A purpose-built three-slot graph, not `AudioProcessorGraph` (fixed topology, no routing editor,
  simpler threading and latency handling).
- Host thread model: plugin scanning in a child process (`juce::PluginDirectoryScanner` with a
  dead-man's file, or an out-of-process scanner app) and never on the audio thread; instantiation on
  the message thread (`AudioPluginFormatManager::createPluginInstanceAsync`); swap of a slot through
  a lock-free pointer exchange with deferred deletion on the message thread.
- State: ANDOR's performance (JSON) + per slot `PluginDescription` (XML) + the hosted plugin's
  `getStateInformation` blob, in one versioned chunk. A missing plugin keeps its description and
  blob ("SERUM missing — click to locate"), so the session is not damaged.
- Latency: report the maximum of the slots' latencies and delay the others to it.

## 3. Audit items (spec §14) — what must be checked, with what is known

| Item | Known risk / approach | Status |
|---|---|---|
| JUCE version | OSP pins JUCE 8.0.9 (`OSP_JUCE_TAG`). Hosting a VST3 inside a JUCE VST3 needs `JUCE_PLUGINHOST_VST3=1` in the plugin target; JUCE compiles one copy of the VST3 SDK for client and host. | not built |
| Duplicate VST3 SDK symbols | a single JUCE build includes the SDK once; problems arise only if a second SDK copy is linked. Check with `nm` on the bundle. | not checked |
| Plugin scan | in-process scanning inside a DAW risks crashing the DAW: scan out of process, cache `KnownPluginList` XML. | not built |
| Editors | hosted editor in a child `DocumentWindow` or embedded `AudioProcessorEditor`; ownership must end before the instance; HiDPI on Windows. | not built |
| Nested state | the hosted plugin's state inside ours; size limits in some hosts (large Kontakt states). | not built |
| Sample rate / buffer changes | forward `prepareToPlay`/`releaseResources`; re-prepare off the audio thread. | not built |
| MIDI timing | sample offsets preserved per lane buffer (the coordinator already gives them). | covered by shared tests |
| Multi-output instruments | sum the main bus only for the MVP; expose nothing else. | not built |
| Bypass | our bypass sends `releaseAll` note-offs to every slot. | designed |
| Crashing hosted plugins | cannot be contained in-process; failures at load must show an error and leave the slot empty. | not built |
| Latency reporting | max of slots, others delayed. | designed |
| Licensing | Kontakt (Native Access), Serum, SWAM (iLok/own) authorise inside other hosts generally; must be tested on the owner's machine with the owner's licences. | not tested |
| AU | an instrument AU hosting AUs/VST3s: Logic's AU validation and sandbox (AUv3 out-of-process) differ; separate gate. | not started |

## 4. Gate (spec §14) — the minimal experiment, in order

1. `juce/hosting-poc`: a JUCE VST3 instrument with ONE slot: scan (out of process) → pick a VST3 →
   send it the coordinator's lane A → sum its audio → open its editor → save/restore its state.
2. Run it in the owner's primary DAW on macOS with one known instrument (Surge XT, free, as a
   baseline; then Kontakt, Serum, SWAM as installed). Save the project, quit the DAW, reopen.
3. Extend to three slots; run the spec §17 list.
4. Record every DAW × instrument result in `docs/TEST_MATRIX.md`.

A standalone `AudioPluginHost` run does not count (spec). **Decision rule:** if this proves
disproportionately complex, the JUCE product stays deferred; the one-track requirement is not
compromised.

## 5. Identity (spec §16)

New identifiers, never OSP's (`com.osp.instrument`, AU `aumu Osp1 Ospx`): proposed
bundle id `com.andor.arp`, VST3 class id from a new UUID, AU type `aumu`, subtype `AnAr`,
manufacturer per the ANDOR scheme (owner to confirm: OSP uses `Ospx`). To be reserved by the owner.
