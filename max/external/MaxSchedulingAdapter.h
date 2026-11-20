#pragma once

#include "engine/HostTiming.h"

#include <cstdint>

namespace andor::arp
{

/**
    Turns Max's scheduler into the blocks the ARP core expects (HostAdapterContract.h), without
    any Max header: the external (andor.arp.engine) calls it, the tests drive it with simulated
    scheduler and transport behaviour.

    The problem. The core is written for a plugin's audio callback: gap-free blocks on a sample
    clock, each with the transport's exact position. In Max for Live a C external runs in Max's
    scheduler: it is woken at moments that jitter, reads the transport (ITM) as Live last
    published it (possibly up to an audio buffer stale), and outputs MIDI from a clock callback.
    Feeding those raw readings to the core would look like a jump every few blocks (its tolerance
    is 64 samples) and drop or repeat steps near grid lines.

    The adapter's model (just in time, fixed output latency):
    - A virtual sample clock starts at the first service and runs on Live's timeline: virtual
      sample v is the moment origin + v / rate. Every service renders the block from where the
      last one ended up to `now`, so blocks never overlap or leave gaps, and outputs each event at
      origin + latency + v / rate. Scheduler jitter changes when a block is rendered, never where
      its notes land (as long as a service comes within `latency`).
    - MIDI that arrived since the last service is applied at the block's first sample, i.e. no
      later than it really happened. This matters: a clip's chord change sits exactly on a grid
      line, and applying it after the line (as rendering ahead would) plays one step of the old
      chord. Applied early by at most one service interval, the new chord plays on the line.
    - The block's position is the previous block's end (continuous), never the raw reading. The
      readings (ITM, as Live last published it: never ahead, up to an audio buffer behind) are
      projected to the block's start and compared; the control error is the most advanced of the
      recent ones (the least stale). An error above `jumpQuarters` and `jumpMs` (a loop, a locate,
      the transport starting) re-anchors the position (the core then re-aligns to the grid, which
      is what a jump should do); a smaller one is removed by a tempo correction of at most
      `maxTempoCorrection` (0.5 %: inaudible), larger (`acquireTempoCorrection`) for a moment after
      a re-anchor, so the core stays locked to Live's transport without seeing a discontinuity.
    - Output reaches the instrument `latency` after Live's timeline (constant: 4 ms by default).

    Not real-time sensitive in itself (plain arithmetic, no allocation); called on the scheduler
    thread.
*/
class MaxSchedulingAdapter
{
public:
    struct Config
    {
        double sampleRate = 48000.0;      ///< the virtual clock (Live's sample rate where known)
        double latencyMs = 4.0;           ///< output delay behind Live's timeline (at least the longest gap between services)
        double jumpQuarters = 0.125;      ///< a larger error than this is a jump (1/32 note)
        double jumpMs = 40.0;             ///< ... and larger than this in time (readings can be a buffer stale)
        double maxTempoCorrection = 0.005;
        double acquireTempoCorrection = 0.03;   ///< for `acquireSeconds` after a re-anchor (the anchor reading may be stale)
        double acquireSeconds = 0.5;
        double correctionSeconds = 0.25;  ///< how quickly a small error is removed
        double envelopeMs = 30.0;         ///< the window of readings whose least stale one is trusted
    };

    /** What the transport (Max's ITM, which follows Live) says at a service. */
    struct TransportReading
    {
        bool valid = false;
        bool playing = false;
        double bpm = 120.0;
        double ppq = 0.0;                 ///< ITM ticks / 480
        int numerator = 4, denominator = 4;
    };

    struct Block
    {
        osp::HostTiming timing;           ///< for the core: continuous ppq, corrected bpm
        int numSamples = 0;               ///< 0: nothing to render at this service
        double wallStartMs = 0.0;         ///< when sample 0 of the block reaches the instrument
        bool jumped = false;              ///< re-anchored to the reading (loop, locate, start)
    };

    void configure (const Config& config) noexcept;
    const Config& config() const noexcept { return cfg; }
    /** Forgets the clock (the next service starts it again). */
    void reset() noexcept;

    /** One scheduler service at wall time `nowMs` (any monotonic millisecond clock). */
    Block service (double nowMs, const TransportReading& transport) noexcept;

    /** The wall time of `offset` in a block (for scheduling the event's output). */
    double eventWallMs (const Block& block, int offset) const noexcept { return block.wallStartMs + offset * 1000.0 / cfg.sampleRate; }

    /** The current position error, in quarters (tests and the device's diagnostics). */
    double lastErrorQuarters() const noexcept { return lastError; }
    std::int64_t jumps() const noexcept { return jumpCount; }

private:
    Config cfg;
    bool started = false, wasPlaying = false;
    double originMs = 0.0;
    std::int64_t renderedSamples = 0;   ///< virtual samples rendered so far
    double ppqAtEnd = 0.0;              ///< the position where the next block starts (continuous)
    double lastBpm = 120.0;
    double lastError = 0.0, anchoredAtMs = -1.0e18;
    double correctedQuarters = 0.0;   ///< how far the tempo correction has moved the position since the anchor
    std::int64_t jumpCount = 0;
    struct Reading
    {
        double ms = 0.0, error = 0.0;
    };
    static constexpr int historySize = 64;
    Reading history[historySize] {};
    int historyCount = 0, historyNext = 0;
};

} // namespace andor::arp
