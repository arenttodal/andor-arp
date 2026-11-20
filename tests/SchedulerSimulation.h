#pragma once

// A model of how Max for Live runs a C external, to measure MaxSchedulingAdapter before Live is
// available (docs/results/timing-simulation.md). It is a MODEL: the real figures must come from
// Live recordings (docs/TEST_MATRIX.md, M4L-T1). Assumptions, each a parameter:
//   - Live renders audio in buffers of `liveBuffer` samples and publishes the transport (ITM)
//     once per buffer: a reading taken during a buffer is that buffer's start position (stale by
//     up to one buffer);
//   - Max's scheduler serves the external every `vector` samples (Scheduler in Audio Interrupt),
//     late by up to `serviceJitterMs`, and now and then blocked for `stallMs`;
//   - an event scheduled for a wall time is output at the first service at or after it
//     (clock_fdelay resolves on the scheduler's ticks);
//   - the system clock the external reads may drift against the audio clock (`driftPpm`).
// The reference is the same core driven as a plugin is: gap-free blocks with exact positions.

#include "MaxSchedulingAdapter.h"
#include "PerformanceModel.h"

#include <string>
#include <vector>

namespace andor_arp_test
{

struct SimConfig
{
    std::string name;
    double sampleRate = 44100.0;
    int liveBuffer = 512;
    int vector = 64;
    double serviceJitterMs = 0.0;
    double stallEveryMs = 0.0, stallMs = 0.0;
    bool outputOnServiceTicks = true;
    double driftPpm = 0.0;
    double bpm = 120.0;
    double tempoChangeAtSeconds = -1.0, tempoAfter = 120.0;
    double loopStartPpq = -1.0, loopEndPpq = -1.0;   ///< a looping transport (both >= 0)
    double seconds = 30.0;
    double chordChangeSeconds = 0.0;   ///< > 0: a clip changes chord on this period (exactly on bar lines at 120 BPM)
    std::uint64_t seed = 1;
    andor::arp::MaxSchedulingAdapter::Config adapter {};
    andor::arp::LaneSettings lane {};
};

struct SimResult
{
    int referenceNotes = 0, adapterNotes = 0;
    int matched = 0, missing = 0, extra = 0;
    double latencyMs = 0.0;           ///< the constant offset removed (the adapter's latency)
    double meanMs = 0.0, meanAbsMs = 0.0, p99AbsMs = 0.0, maxAbsMs = 0.0;   ///< deviation after removing it
    long long jumps = 0;
    int unbalancedPitches = 0;        ///< notes left sounding at the end (must be 0)
    std::vector<std::string> unmatched;   ///< diagnostics: "ref|adapter seconds note"
};

SimResult simulate (const SimConfig& config);
std::vector<SimConfig> standardScenarios();
std::string formatResult (const SimConfig& config, const SimResult& result);

} // namespace andor_arp_test
