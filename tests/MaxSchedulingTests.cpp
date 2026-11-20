// MaxSchedulingAdapter: continuous positions for the core, jumps only when Live really jumped,
// and the scheduler model's timing within its documented tolerances.

#include "MaxSchedulingAdapter.h"
#include "SchedulerSimulation.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace andor::arp;
using namespace andor_arp_test;

TEST_CASE ("Max scheduling: blocks are gap-free and the position continuous under jittery readings", "[unit][max-scheduling]")
{
    MaxSchedulingAdapter a;
    MaxSchedulingAdapter::Config cfg;
    cfg.sampleRate = 44100.0;
    a.configure (cfg);
    double expectedPpq = -1.0;
    long long rendered = 0;
    for (int i = 0; i < 5000; ++i)
    {
        const double now = i * 1.451 + (i % 7) * 0.3;   // irregular services
        MaxSchedulingAdapter::TransportReading r;
        r.valid = r.playing = true;
        r.bpm = 120.0;
        r.ppq = std::floor (now / 11.61) * 11.61 / 500.0;   // published once per 512-sample buffer
        const auto b = a.service (now, r);
        if (b.numSamples == 0)
            continue;
        if (expectedPpq >= 0.0)
        {
            INFO (i);
            CHECK (! b.jumped);
            // The core's own continuity test: within 64 samples.
            CHECK (std::abs (b.timing.ppq - expectedPpq) < 64.0 / (44100.0 * 60.0 / b.timing.bpm));
        }
        expectedPpq = b.timing.ppq + b.numSamples / (44100.0 * 60.0 / b.timing.bpm);
        CHECK (std::abs (b.wallStartMs - (cfg.latencyMs + static_cast<double> (rendered) * 1000.0 / 44100.0)) < 1.0e-6);
        rendered += b.numSamples;
        // Up to the acquisition limit just after the transport's start, then the small one.
        const double limit = now < cfg.acquireSeconds * 1000.0 + 20.0 ? cfg.acquireTempoCorrection : cfg.maxTempoCorrection;
        CHECK (std::abs (b.timing.bpm / 120.0 - 1.0) <= limit + 1.0e-12);
    }
    CHECK (a.jumps() == 1);   // only the transport's start
}

TEST_CASE ("Max scheduling: a locate or loop re-anchors once", "[unit][max-scheduling]")
{
    MaxSchedulingAdapter a;
    a.configure ({});
    MaxSchedulingAdapter::TransportReading r;
    r.valid = r.playing = true;
    for (int i = 0; i < 200; ++i)
    {
        const double now = i * 1.0;
        r.ppq = now / 500.0 + (i >= 100 ? -8.0 : 0.0);   // back two bars at 100 ms
        const auto b = a.service (now, r);
        if (i == 1 || i == 100)   // (service 0 only starts the clock: nothing to render yet)
        {
            CHECK (b.jumped);
            // Re-anchored to the reading, projected back to the block's place on Live's timeline.
            CHECK (std::abs (b.timing.ppq - (r.ppq + (b.wallStartMs - a.config().latencyMs - now) / 500.0)) < 1.0e-9);
        }
        else
            CHECK_FALSE (b.jumped);
    }
    CHECK (a.jumps() == 2);
    // Stopped: the reading passes through, nothing to lock to.
    r.playing = false;
    const auto stopped = a.service (300.0, r);
    CHECK_FALSE (stopped.timing.playing);
}

TEST_CASE ("Max scheduling model: no missing, doubled or hanging notes; timing within two vectors", "[timing]")
{
    // Tolerances of the MODEL (docs/results/timing-simulation.md), after removing the configured
    // output latency: clock callbacks fire on scheduler ticks, and a block covers the time since
    // the last tick, so a note can land up to two vectors (plus the services' own lateness) after
    // its exact time. A stall longer than the latency makes notes late; it must never lose,
    // double or hang one.
    for (const auto& c : standardScenarios())
    {
        const auto r = simulate (c);
        INFO (formatResult (c, r));
        CHECK (r.referenceNotes > 50);
        CHECK (r.unbalancedPitches == 0);
        CHECK (r.missing == 0);
        CHECK (r.extra == 0);
        const double vectorMs = c.vector * 1000.0 / c.sampleRate;
        if (c.stallMs <= c.adapter.latencyMs)
            CHECK (r.maxAbsMs <= 2.0 * vectorMs + c.serviceJitterMs + 0.5);
        CHECK (r.meanMs >= -0.5);   // never early on average (the latency covers the render)
    }
}
