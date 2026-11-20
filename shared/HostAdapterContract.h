#pragma once

// The contract between ANDOR ARP's musical core and each host adapter (the Max for Live external,
// the JUCE plugin, the headless test renderer). Every musical decision is made in andor_arp_core
// (OSP's arpeggiator) and in the ThreeLaneCoordinator; an adapter only translates its host's
// time and MIDI into this model and the generated events back.
//
// HOST -> CORE, once per block, on the host's timing-critical thread:
//   - HostTiming at the block's first sample: valid, playing, bpm, ppq (quarters since the song's
//     start), time signature. It must be CONTINUOUS while the transport plays: consecutive blocks'
//     ppq differ by exactly numSamples / samplesPerQuarter, within 64 samples, unless the host
//     really looped or jumped. (The core treats anything else as a jump and re-aligns to the grid,
//     which can drop or repeat a step near a boundary. A host whose clock jitters, such as Max's
//     scheduler, therefore needs a smoothing adapter: max/external/MaxSchedulingAdapter.)
//   - numSamples: the block's length on a sample clock at the prepared rate. Blocks follow each
//     other without gaps; their size may vary (1 .. any).
//   - Input events, ordered by sample offset inside the block: note on (velocity 1-127), note off
//     (a note-on with velocity 0 is a note off), sustain pedal down/up (CC64 >= 64), All Notes Off
//     (CC123, and CC120 All Sound Off). Nothing else: pitch bend, pressure and other controllers
//     pass by the core and are forwarded by the adapter unchanged.
//   - Settings (LaneSettings) and compiled custom patterns, copied in between blocks. Never a
//     pointer the message thread still writes.
//
// CORE -> HOST, after the block:
//   - Note events per lane (A, B, C), each with a sample offset inside the block, its MIDI
//     channel and velocity. At the same offset a note-off precedes a note-on. Every note-on has
//     its note-off later, unless the session ends first: then the adapter calls releaseAll().
//   - Status per lane (running, held notes, sounding notes, step) and an overflow flag when a
//     block produced more events than the fixed capacity (they are dropped and counted, never
//     written out of bounds; at most 2048 per lane per block).
//
// THE ADAPTER MUST:
//   - never forward the original note events or CC64 to an instrument a lane plays (the lane
//     replaces them), unless that lane's pass-through is on (then the coordinator emits them);
//   - call releaseAll() on transport stop when its host requires it, on bypass, deactivation,
//     device deletion, unload and before reset(), and send the resulting note-offs;
//   - run process() on a thread where no allocation, lock, file or network access happens (the
//     core and the coordinator never do any of these);
//   - keep the seed in its saved state, so the random styles replay identically after reload.
//
// Message-thread side (allocates, never on the timing-critical thread): PerformanceModel
// (lane names, settings, pattern documents, JSON) and the `.andorpattern` codec.

#include "engine/HostTiming.h"

namespace andor::arp
{
using HostTiming = osp::HostTiming;
}
