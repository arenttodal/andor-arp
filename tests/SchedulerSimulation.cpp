#include "SchedulerSimulation.h"

#include "ThreeLaneCoordinator.h"

#include "core/Prng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace andor_arp_test
{
using namespace andor::arp;

namespace
{
    /** Live's transport: position (quarters) at an audio-clock time, with tempo change and loop. */
    struct LiveTransport
    {
        const SimConfig& c;
        double ppqAt (double seconds) const
        {
            double ppq = 0.0;
            if (c.tempoChangeAtSeconds >= 0.0 && seconds > c.tempoChangeAtSeconds)
                ppq = c.tempoChangeAtSeconds * c.bpm / 60.0 + (seconds - c.tempoChangeAtSeconds) * c.tempoAfter / 60.0;
            else
                ppq = seconds * c.bpm / 60.0;
            if (c.loopStartPpq >= 0.0 && c.loopEndPpq > c.loopStartPpq && ppq >= c.loopEndPpq)
                ppq = c.loopStartPpq + std::fmod (ppq - c.loopEndPpq, c.loopEndPpq - c.loopStartPpq);
            return ppq;
        }
        double bpmAt (double seconds) const { return c.tempoChangeAtSeconds >= 0.0 && seconds > c.tempoChangeAtSeconds ? c.tempoAfter : c.bpm; }
    };

    struct Ev
    {
        double seconds;
        int note;
        bool on;
    };

    constexpr double pressSeconds = 0.1;
    const int chordNotes[] = { 57, 60, 64, 67 };
    const int chordNotesB[] = { 53, 57, 60, 65 };

    struct KeyEvent
    {
        double seconds;
        int note;
        bool on;
    };

    /** The keys: one chord held (pressed at 0.1 s), or a clip changing chord on every period. */
    std::vector<KeyEvent> keyEvents (const SimConfig& c)
    {
        std::vector<KeyEvent> k;
        const double release = c.seconds - 0.5;
        if (c.chordChangeSeconds <= 0.0)
        {
            for (int n : chordNotes)
                k.push_back ({ pressSeconds, n, true });
            for (int n : chordNotes)
                k.push_back ({ release, n, false });
            return k;
        }
        int index = 0;
        for (double t = 0.0; t < release; t += c.chordChangeSeconds, ++index)
        {
            const int* chord = index % 2 == 0 ? chordNotes : chordNotesB;
            const double end = std::min (t + c.chordChangeSeconds, release);
            for (int i = 0; i < 4; ++i)
                k.push_back ({ end, chord[i], false });   // (sorted below: offs at a change come first)
            for (int i = 0; i < 4; ++i)
                k.push_back ({ t, chord[i], true });
        }
        std::stable_sort (k.begin(), k.end(), [] (const KeyEvent& a, const KeyEvent& b) {
            return a.seconds < b.seconds || (a.seconds == b.seconds && ! a.on && b.on);
        });
        return k;
    }

    std::vector<Ev> reference (const SimConfig& c)
    {
        // The plugin case: 64-sample blocks with exact positions.
        LiveTransport live { c };
        ThreeLaneCoordinator coord;
        coord.prepare (c.sampleRate);
        coord.setActiveLanes (1);
        coord.setLaneSettings (Lane::A, c.lane);
        std::vector<Ev> out;
        const int block = 64;
        const auto total = static_cast<long long> (c.seconds * c.sampleRate);
        const auto keys = keyEvents (c);
        std::size_t nextKey = 0;
        for (long long s = 0; s < total; s += block)
        {
            const double t = static_cast<double> (s) / c.sampleRate;
            HostTiming timing;
            timing.valid = timing.playing = true;
            timing.bpm = live.bpmAt (t);
            timing.ppq = live.ppqAt (t);
            std::vector<ThreeLaneCoordinator::Input> in;
            while (nextKey < keys.size() && static_cast<long long> (std::llround (keys[nextKey].seconds * c.sampleRate)) < s + block)
            {
                const auto& k = keys[nextKey++];
                const int offset = static_cast<int> (std::max (0LL, std::llround (k.seconds * c.sampleRate) - s));
                in.push_back (k.on ? ThreeLaneCoordinator::Input::on (offset, k.note, 100) : ThreeLaneCoordinator::Input::off (offset, k.note));
            }
            coord.process (timing, block, in.data(), static_cast<int> (in.size()));
            for (int i = 0; i < coord.numOutputs(); ++i)
                out.push_back ({ static_cast<double> (s + coord.output (i).offset) / c.sampleRate, coord.output (i).note, coord.output (i).on });
        }
        return out;
    }

    std::vector<Ev> adapterRun (const SimConfig& c, long long& jumps)
    {
        LiveTransport live { c };
        osp::Prng rng (c.seed);
        MaxSchedulingAdapter adapter;
        auto cfg = c.adapter;
        cfg.sampleRate = c.sampleRate;
        adapter.configure (cfg);
        ThreeLaneCoordinator coord;
        coord.prepare (c.sampleRate);
        coord.setActiveLanes (1);
        coord.setLaneSettings (Lane::A, c.lane);

        struct Pending
        {
            double wallMs;
            int note;
            bool on;
        };
        std::vector<Pending> scheduled;
        std::vector<Ev> out;
        const double vectorMs = c.vector * 1000.0 / c.sampleRate;
        const double bufferSeconds = c.liveBuffer / c.sampleRate;
        const auto keys = keyEvents (c);
        std::size_t nextKey = 0;
        std::vector<ThreeLaneCoordinator::Input> in;   // keys waiting for the next rendered block
        double nextStall = c.stallEveryMs > 0.0 ? c.stallEveryMs : 1.0e18;

        // Services happen on the audio clock's vector grid, late by jitter; stalls delay a run of them.
        double stalledUntil = 0.0;
        for (double tick = 0.0; tick < c.seconds * 1000.0; tick += vectorMs)
        {
            double serviceMs = tick + (c.serviceJitterMs > 0.0 ? rng.nextDouble() * c.serviceJitterMs : 0.0);
            if (tick >= nextStall)
            {
                stalledUntil = tick + c.stallMs;
                nextStall += c.stallEveryMs;
            }
            serviceMs = std::max (serviceMs, stalledUntil);
            // Output whatever is due by now (clock callbacks fire on services).
            std::stable_sort (scheduled.begin(), scheduled.end(), [] (const Pending& a, const Pending& b) { return a.wallMs < b.wallMs; });
            std::size_t k = 0;
            for (; k < scheduled.size() && scheduled[k].wallMs <= serviceMs + 1.0e-9; ++k)
                out.push_back ({ (c.outputOnServiceTicks ? serviceMs : scheduled[k].wallMs) / 1000.0, scheduled[k].note, scheduled[k].on });
            scheduled.erase (scheduled.begin(), scheduled.begin() + static_cast<long> (k));

            // The transport as Live last published it (the start of the current audio buffer).
            const double audioSeconds = serviceMs / 1000.0;
            const double published = std::floor (audioSeconds / bufferSeconds) * bufferSeconds;
            MaxSchedulingAdapter::TransportReading r;
            r.valid = r.playing = true;
            r.bpm = live.bpmAt (published);
            r.ppq = live.ppqAt (published);
            // The external's own clock (drifting against the audio clock).
            const double nowMs = serviceMs * (1.0 + c.driftPpm * 1.0e-6);
            // (The reading is up to a buffer old; the external cannot tell by how much, so it is used as is.)
            const auto block = adapter.service (nowMs, r);
            // Keys reach the external at the first service at or after they happen (no timestamp);
            // they wait for the next rendered block, as in the external.
            while (nextKey < keys.size() && keys[nextKey].seconds <= audioSeconds)
            {
                const auto& key = keys[nextKey++];
                in.push_back (key.on ? ThreeLaneCoordinator::Input::on (0, key.note, 100) : ThreeLaneCoordinator::Input::off (0, key.note));
            }
            if (block.numSamples <= 0)
                continue;
            coord.process (block.timing, block.numSamples, in.data(), static_cast<int> (in.size()));
            in.clear();
            for (int i = 0; i < coord.numOutputs(); ++i)
            {
                // Wall times are on the external's clock; back to the audio clock for comparison.
                const double wall = adapter.eventWallMs (block, coord.output (i).offset) / (1.0 + c.driftPpm * 1.0e-6);
                scheduled.push_back ({ wall, coord.output (i).note, coord.output (i).on });
            }
        }
        for (const auto& p : scheduled)
            out.push_back ({ p.wallMs / 1000.0, p.note, p.on });
        jumps = adapter.jumps();
        return out;
    }
}

SimResult simulate (const SimConfig& c)
{
    SimResult r;
    const auto ref = reference (c);
    const auto got = adapterRun (c, r.jumps);
    std::vector<Ev> refOn, gotOn;
    for (const auto& e : ref)
        if (e.on)
            refOn.push_back (e);
    for (const auto& e : got)
        if (e.on)
            gotOn.push_back (e);
    r.referenceNotes = static_cast<int> (refOn.size());
    r.adapterNotes = static_cast<int> (gotOn.size());
    r.latencyMs = c.adapter.latencyMs;

    // Match each reference note-on to the nearest adapter note-on of the same pitch within 60 ms
    // after the constant latency is removed (greedy, in order).
    std::vector<bool> used (gotOn.size(), false);
    std::vector<double> dev;
    std::size_t start = 0;
    for (const auto& e : refOn)
    {
        int best = -1;
        double bestAbs = 0.060;
        for (std::size_t k = start; k < gotOn.size(); ++k)
        {
            const double d = gotOn[k].seconds - c.adapter.latencyMs / 1000.0 - e.seconds;
            if (d > 0.060)
                break;
            if (! used[k] && gotOn[k].note == e.note && std::abs (d) < bestAbs)
            {
                best = static_cast<int> (k);
                bestAbs = std::abs (d);
            }
        }
        if (best < 0)
        {
            ++r.missing;
            r.unmatched.push_back ("ref " + std::to_string (e.seconds) + " " + std::to_string (e.note));
            continue;
        }
        used[static_cast<std::size_t> (best)] = true;
        dev.push_back ((gotOn[static_cast<std::size_t> (best)].seconds - c.adapter.latencyMs / 1000.0 - e.seconds) * 1000.0);
        while (start < gotOn.size() && used[start])
            ++start;
    }
    r.matched = static_cast<int> (dev.size());
    for (std::size_t k = 0; k < gotOn.size(); ++k)
        if (! used[k])
            r.unmatched.push_back ("adapter " + std::to_string (gotOn[k].seconds) + " " + std::to_string (gotOn[k].note));
    r.extra = r.adapterNotes - r.matched;
    if (! dev.empty())
    {
        double sum = 0.0, sumAbs = 0.0;
        std::vector<double> abs;
        for (double d : dev)
        {
            sum += d;
            sumAbs += std::abs (d);
            abs.push_back (std::abs (d));
        }
        std::sort (abs.begin(), abs.end());
        r.meanMs = sum / static_cast<double> (dev.size());
        r.meanAbsMs = sumAbs / static_cast<double> (dev.size());
        r.p99AbsMs = abs[std::min (abs.size() - 1, static_cast<std::size_t> (0.99 * static_cast<double> (abs.size())))];
        r.maxAbsMs = abs.back();
    }
    std::map<int, int> sounding;
    for (const auto& e : got)
        sounding[e.note] += e.on ? 1 : -1;
    for (const auto& [note, n] : sounding)
        r.unbalancedPitches += n != 0 ? 1 : 0;
    return r;
}

std::vector<SimConfig> standardScenarios()
{
    std::vector<SimConfig> s;
    auto base = [] (const char* name) {
        SimConfig c;
        c.name = name;
        c.lane.arp.pattern = osp::ArpPattern::upDown;
        c.lane.arp.rate = osp::ArpRate::sixteenth;
        c.lane.arp.octaves = 2;
        c.lane.arp.gate = 0.7;
        return c;
    };
    s.push_back (base ("ideal: services every vector, stale buffer readings"));
    auto c = base ("jitter: services up to 2 ms late");
    c.serviceJitterMs = 2.0;
    s.push_back (c);
    c = base ("jitter 5 ms, buffer 1024");
    c.serviceJitterMs = 5.0;
    c.liveBuffer = 1024;
    s.push_back (c);
    c = base ("stalls: 20 ms every 2 s (longer than the latency)");
    c.stallEveryMs = 2000.0;
    c.stallMs = 20.0;
    s.push_back (c);
    c = base ("clock drift 100 ppm");
    c.driftPpm = 100.0;
    s.push_back (c);
    c = base ("tempo change 120 -> 97 BPM at 10 s");
    c.tempoChangeAtSeconds = 10.0;
    c.tempoAfter = 97.0;
    s.push_back (c);
    c = base ("loop: 2 bars, 1/8 triplets, swing 40 %");
    c.loopStartPpq = 4.0;
    c.loopEndPpq = 12.0;
    c.lane.arp.rate = osp::ArpRate::eighthTriplet;
    c.lane.arp.swing = 0.4;
    s.push_back (c);
    c = base ("48 kHz, buffer 128, 1/32");
    c.sampleRate = 48000.0;
    c.liveBuffer = 128;
    c.lane.arp.rate = osp::ArpRate::thirtySecond;
    s.push_back (c);
    c = base ("clip: chord changes on every bar line (2 s)");
    c.chordChangeSeconds = 2.0;
    s.push_back (c);
    c = base ("clip changes on bar lines + 2 ms jitter + loop");
    c.chordChangeSeconds = 2.0;
    c.serviceJitterMs = 2.0;
    c.loopStartPpq = 0.0;
    c.loopEndPpq = 16.0;
    s.push_back (c);
    c = base ("no tick quantisation (events at their exact time)");
    c.outputOnServiceTicks = false;
    c.serviceJitterMs = 2.0;
    s.push_back (c);
    return s;
}

std::string formatResult (const SimConfig& c, const SimResult& r)
{
    char line[512];
    std::snprintf (line, sizeof line, "| %s | %d | %d | %d | %d | %.2f | %.2f | %.2f | %.2f | %lld | %d |", c.name.c_str(), r.referenceNotes, r.matched,
                   r.missing, r.extra, r.meanMs, r.meanAbsMs, r.p99AbsMs, r.maxAbsMs, r.jumps, r.unbalancedPitches);
    return line;
}

} // namespace andor_arp_test
