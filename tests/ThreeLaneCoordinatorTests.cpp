// The three-lane coordinator: one input, three independent performances, never a stuck note, and
// every lane exactly OSP's arpeggiator (no second algorithm).

#include "TestRender.h"

#include "core/Prng.h"
#include "model/ArpFactoryPatterns.h"
#include "support/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>

using namespace andor_arp_test;
using namespace andor::arp;

namespace
{
    constexpr double sr = 48000.0;

    LaneSettings lane (osp::ArpPattern p, osp::ArpRate r, int octaves = 1)
    {
        LaneSettings s;
        s.arp.pattern = p;
        s.arp.rate = r;
        s.arp.octaves = octaves;
        s.arp.gate = 0.8;
        return s;
    }

    std::vector<TimedInput> chord (long long at, long long release, std::initializer_list<int> notes)
    {
        std::vector<TimedInput> in;
        for (int n : notes)
            in.push_back ({ at, ThreeLaneCoordinator::Input::on (0, n, 96) });
        for (int n : notes)
            in.push_back ({ release, ThreeLaneCoordinator::Input::off (0, n) });
        std::stable_sort (in.begin(), in.end(), [] (const TimedInput& a, const TimedInput& b) { return a.sample < b.sample; });
        return in;
    }

    Transport playing (double bpm = 120.0)
    {
        return [bpm] (long long s) { return playingAt (bpm, sr, s); };
    }

    /** Every pitch on every lane: ons and offs alternate, starting with an on, and end off. */
    void requireBalanced (const std::vector<Note>& notes)
    {
        std::map<std::pair<int, int>, int> sounding;
        for (const auto& n : notes)
        {
            auto& s = sounding[{ n.lane, n.note }];
            if (n.on)
            {
                INFO ("lane " << n.lane << " note " << n.note << " at " << n.sample << " started while sounding");
                REQUIRE (s == 0);
                s = 1;
            }
            else
            {
                INFO ("lane " << n.lane << " note " << n.note << " at " << n.sample << " ended while silent");
                REQUIRE (s == 1);
                s = 0;
            }
        }
        for (const auto& [key, s] : sounding)
        {
            INFO ("lane " << key.first << " note " << key.second << " left sounding");
            REQUIRE (s == 0);
        }
    }
}

TEST_CASE ("Coordinator: three lanes play one chord independently, each exactly OSP's arpeggiator", "[unit][lanes]")
{
    std::array<LaneSettings, 3> s { lane (osp::ArpPattern::up, osp::ArpRate::sixteenth, 2), lane (osp::ArpPattern::down, osp::ArpRate::eighthTriplet),
                                    lane (osp::ArpPattern::random, osp::ArpRate::eighth, 3) };
    s[1].arp.swing = 0.4;
    s[2].seed = 99;
    const auto pattern = osp::arppattern::compile (osp::arppattern::factoryPatterns()[1].pattern);
    s[0].arp.custom = true;   // A plays a custom pattern
    const auto in = chord (1000, 4 * 48000, { 60, 64, 67, 71 });
    const long long total = 5 * 48000;

    ThreeLaneCoordinator c;
    c.prepare (sr);
    for (int i = 0; i < 3; ++i)
        c.setLaneSettings (laneAt (i), s[static_cast<std::size_t> (i)]);
    c.setCustomPattern (Lane::A, &pattern);
    const auto out = renderCoordinator (c, in, total, playing(), { 512 });
    requireBalanced (out);

    for (int i = 0; i < 3; ++i)
    {
        const auto want = renderReference (s[static_cast<std::size_t> (i)], i == 0 ? &pattern : nullptr, in, total, playing(), 512, sr);
        const auto got = laneOnly (out, i);
        INFO ("lane " << i << "\n got  " << describe (got) << "\n want " << describe (want));
        CHECK (got.size() > 20);
        CHECK (got == want);
    }
    // The lanes really differ.
    CHECK (laneOnly (out, 0) != laneOnly (out, 1));
    CHECK (laneOnly (out, 1) != laneOnly (out, 2));
}

TEST_CASE ("Coordinator: the original chord never leaks into a lane unless pass-through is on", "[unit][lanes]")
{
    // CHORD at 1/1... a lane playing DOWN at 1/4 starts on the top note; the chord's other keys
    // must not sound at the chord's own onset.
    ThreeLaneCoordinator c;
    c.prepare (sr);
    c.setActiveLanes (1u << 1);
    c.setLaneSettings (Lane::B, lane (osp::ArpPattern::down, osp::ArpRate::quarter));
    const auto in = chord (0, 48000, { 48, 52, 55 });
    auto out = renderCoordinator (c, in, 60000, playing(), { 256 });
    int onsAtStart = 0;
    for (const auto& n : out)
        if (n.on && n.sample == 0)
            ++onsAtStart;
    CHECK (onsAtStart == 1);
    requireBalanced (out);

    // With pass-through, the keys sound too (and still end).
    ThreeLaneCoordinator p;
    p.prepare (sr);
    p.setActiveLanes (1u << 1);
    auto withKeys = lane (osp::ArpPattern::down, osp::ArpRate::quarter);
    withKeys.passThrough = true;
    withKeys.transpose = 24;   // the arpeggio two octaves up, so it never shares a pitch with the keys
    p.setLaneSettings (Lane::B, withKeys);
    out = renderCoordinator (p, in, 60000, playing(), { 256 });
    onsAtStart = 0;
    for (const auto& n : out)
        if (n.on && n.sample == 0)
            ++onsAtStart;
    CHECK (onsAtStart == 4);
    requireBalanced (out);
}

TEST_CASE ("Coordinator: muting one lane ends its notes, leaves the others untouched, and rejoins in phase", "[unit][lanes]")
{
    const auto a = lane (osp::ArpPattern::up, osp::ArpRate::sixteenth);
    auto b = lane (osp::ArpPattern::upDown, osp::ArpRate::eighth, 2);
    b.arp.gate = 1.4;   // long notes, so a note is sounding when the mute comes
    const auto in = chord (0, 3 * 48000, { 60, 63, 67 });
    const long long total = 4 * 48000, muteAt = 48000 + 1000, unmuteAt = 2 * 48000 + 1000;

    ThreeLaneCoordinator c;
    c.prepare (sr);
    c.setLaneSettings (Lane::A, a);
    c.setLaneSettings (Lane::B, b);
    c.setActiveLanes (3);
    auto bMuted = b;
    bMuted.enabled = false;
    const auto out = renderCoordinator (c, in, total, playing(), { 500 }, [&] (long long start) {
        if (start == (muteAt / 500) * 500)
            c.setLaneSettings (Lane::B, bMuted);
        if (start == (unmuteAt / 500) * 500)
            c.setLaneSettings (Lane::B, b);
    });
    requireBalanced (out);

    CHECK (laneOnly (out, 0) == renderReference (a, nullptr, in, total, playing(), 500, sr));

    const auto ref = renderReference (b, nullptr, in, total, playing(), 500, sr);
    const auto got = laneOnly (out, 1);
    const long long muteBlock = (muteAt / 500) * 500, unmuteBlock = (unmuteAt / 500) * 500;
    std::vector<Note> refOns, gotOns;
    for (const auto& n : ref)
        if (n.on && (n.sample < muteBlock || n.sample >= unmuteBlock))
            refOns.push_back (n);
    for (const auto& n : got)
        if (n.on)
            gotOns.push_back (n);
    CHECK (gotOns == refOns);   // silent while muted; afterwards exactly the steps it would have played
    bool endedAtMute = false;
    for (const auto& n : got)
        endedAtMute = endedAtMute || (! n.on && n.sample == muteBlock);
    CHECK (endedAtMute);
}

TEST_CASE ("Coordinator: a Receiver running one lane plays exactly that lane of the three-lane engine", "[unit][lanes]")
{
    std::array<LaneSettings, 3> s { lane (osp::ArpPattern::thumbUp, osp::ArpRate::sixteenth, 2), lane (osp::ArpPattern::randomOther, osp::ArpRate::eighth),
                                    lane (osp::ArpPattern::chord, osp::ArpRate::quarter) };
    s[2].arp.strum = true;
    s[2].arp.strumMs = 30.0;
    s[1].seed = defaultLaneSeed (7, Lane::B);
    const auto in = chord (300, 2 * 48000, { 55, 60, 64, 70 });
    ThreeLaneCoordinator all;
    all.prepare (sr);
    for (int i = 0; i < 3; ++i)
        all.setLaneSettings (laneAt (i), s[static_cast<std::size_t> (i)]);
    const auto everything = renderCoordinator (all, in, 3 * 48000, playing (133.0), { 128 });
    for (int i = 0; i < 3; ++i)
    {
        ThreeLaneCoordinator one;
        one.prepare (sr);
        one.setActiveLanes (1u << i);
        one.setLaneSettings (laneAt (i), s[static_cast<std::size_t> (i)]);
        // A Receiver's host blocks differ from another chain's: block size must not matter.
        const auto mine = renderCoordinator (one, in, 3 * 48000, playing (133.0), { 64, 37, 512 });
        INFO ("lane " << i);
        CHECK (laneOnly (mine, i) == laneOnly (everything, i));
        for (const auto& n : mine)
            CHECK (n.lane == i);
    }
}

TEST_CASE ("Coordinator: transposition and the register fold notes inside, and shared pitches never hang", "[unit][lanes]")
{
    auto s = lane (osp::ArpPattern::up, osp::ArpRate::sixteenth, 4);
    s.transpose = 7;
    s.registerLow = 60;
    s.registerHigh = 72;   // a narrow range: four octaves of arpeggio fold onto shared pitches
    s.arp.gate = 1.5;
    ThreeLaneCoordinator c;
    c.prepare (sr);
    c.setActiveLanes (1);
    c.setLaneSettings (Lane::A, s);
    const auto out = renderCoordinator (c, chord (0, 48000, { 36, 43, 48, 55 }), 2 * 48000, playing(), { 333 });
    requireBalanced (out);
    for (const auto& n : out)
    {
        CHECK (n.note >= 60);
        CHECK (n.note <= 72);
    }
}

TEST_CASE ("Coordinator: releaseAll and switching a lane out end every note", "[unit][lanes]")
{
    ThreeLaneCoordinator c;
    c.prepare (sr);
    for (int i = 0; i < 3; ++i)
    {
        auto s = lane (osp::ArpPattern::chord, osp::ArpRate::quarter);
        s.arp.gate = 1.5;
        c.setLaneSettings (laneAt (i), s);
    }
    std::vector<Note> out;
    auto in = chord (0, 10 * 48000, { 60, 64, 67 });
    in.resize (3);   // keys never released
    out = renderCoordinator (c, in, 20000, playing(), { 1000 });
    int sounding = 0;
    for (int i = 0; i < 3; ++i)
        sounding += c.status (laneAt (i)).sounding;
    CHECK (sounding == 9);
    c.releaseAll (5);
    for (int i = 0; i < c.numOutputs(); ++i)
        out.push_back ({ 20000 + c.output (i).offset, laneIndex (c.output (i).lane), c.output (i).on, c.output (i).note, 0, 1 });
    requireBalanced (out);
    for (int i = 0; i < 3; ++i)
        CHECK (c.status (laneAt (i)).held == 0);

    // A lane switched out (its Receiver's chain removed) releases at the next block.
    ThreeLaneCoordinator d;
    d.prepare (sr);
    d.setLaneSettings (Lane::C, lane (osp::ArpPattern::chord, osp::ArpRate::quarter));
    d.setActiveLanes (1u << 2);
    auto notes = renderCoordinator (d, in, 20000, playing(), { 1000 }, [&] (long long start) {
        if (start == 10000)
            d.setActiveLanes (0);
    });
    CHECK_FALSE (notes.empty());
    requireBalanced (notes);
}

TEST_CASE ("Coordinator: block sizes do not change what any lane plays", "[unit][lanes]")
{
    std::array<LaneSettings, 3> s { lane (osp::ArpPattern::random, osp::ArpRate::sixteenth, 2), lane (osp::ArpPattern::randomOnce, osp::ArpRate::eighth),
                                    lane (osp::ArpPattern::chord, osp::ArpRate::sixteenthTriplet, 3) };
    s[2].arp.strum = true;
    s[2].arp.strumMs = 25.0;
    for (int i = 0; i < 3; ++i)
        s[static_cast<std::size_t> (i)].seed = defaultLaneSeed (1234, laneAt (i));
    const auto in = chord (100, 2 * 48000, { 50, 57, 62, 65, 69 });
    std::array<std::vector<Note>, 3> first;
    for (const std::vector<int>& sizes : std::vector<std::vector<int>> { { 64 }, { 1000 }, { 1, 7, 333, 64 } })
    {
        ThreeLaneCoordinator c;
        c.prepare (sr);
        for (int i = 0; i < 3; ++i)
            c.setLaneSettings (laneAt (i), s[static_cast<std::size_t> (i)]);
        const auto out = renderCoordinator (c, in, 3 * 48000, playing (110.0), sizes);
        requireBalanced (out);
        for (int l = 0; l < 3; ++l)
        {
            const auto mine = laneOnly (out, l);
            auto& want = first[static_cast<std::size_t> (l)];
            if (want.empty())
            {
                want = mine;
                continue;
            }
            // Same notes; times within a sample (block boundaries move floating-point rounding).
            INFO ("lane " << l);
            REQUIRE (mine.size() == want.size());
            for (std::size_t k = 0; k < mine.size(); ++k)
            {
                CHECK (mine[k].note == want[k].note);
                CHECK (mine[k].on == want[k].on);
                CHECK (std::abs (mine[k].sample - want[k].sample) <= 1);
            }
        }
    }
}

TEST_CASE ("Coordinator: a transport restart and a reload replay the random styles identically", "[unit][lanes]")
{
    std::array<LaneSettings, 3> s { lane (osp::ArpPattern::random, osp::ArpRate::sixteenth, 2), lane (osp::ArpPattern::randomOnce, osp::ArpRate::eighth),
                                    lane (osp::ArpPattern::randomOther, osp::ArpRate::sixteenthTriplet, 3) };
    for (int i = 0; i < 3; ++i)
        s[static_cast<std::size_t> (i)].seed = defaultLaneSeed (77, laneAt (i));
    const auto in = chord (100, 4 * 48000, { 50, 57, 62, 65, 69 });
    // Played from bar 1 for 1 s, stopped, then played from bar 1 again: both passes must match.
    auto transport = [] (long long sample) {
        HostTiming t;
        t.valid = true;
        t.bpm = 110.0;
        const long long stop = 48000, restart = 96000;
        t.playing = sample < stop || sample >= restart;
        const long long from = sample < stop ? sample : sample >= restart ? sample - restart : 0;
        t.ppq = static_cast<double> (from) / (sr * 60.0 / t.bpm);
        return t;
    };
    std::vector<Note> first;
    for (int reload = 0; reload < 2; ++reload)
    {
        ThreeLaneCoordinator c;   // a fresh instance: a reload from the saved settings
        c.prepare (sr);
        for (int i = 0; i < 3; ++i)
            c.setLaneSettings (laneAt (i), s[static_cast<std::size_t> (i)]);
        const auto out = renderCoordinator (c, in, 3 * 48000, transport, { 64 });
        if (reload == 0)
            first = out;
        else
            CHECK (out == first);
        // The pass after the restart repeats the first pass's steps (same notes, same order).
        for (int l = 0; l < 3; ++l)
        {
            std::vector<int> before, after;
            for (const auto& n : laneOnly (out, l))
                if (n.on && n.sample > 2000 && n.sample < 48000)
                    before.push_back (n.note);
                else if (n.on && n.sample >= 96000 && n.sample < 96000 + 46000)
                    after.push_back (n.note);
            INFO ("lane " << l);
            // (The first pass starts a step later: its keys came 100 samples after the bar line.)
            CHECK (before.size() >= 3);
            REQUIRE (after.size() >= before.size());
            CHECK (std::equal (before.begin(), before.end(), after.begin()));
        }
    }
}

TEST_CASE ("Coordinator: a long random performance never leaves a note hanging, and never allocates", "[unit][lanes]")
{
    osp::Prng rng (2026);
    ThreeLaneCoordinator c;
    c.prepare (44100.0);
    std::array<LaneSettings, 3> s {};
    const auto patterns = osp::arppattern::factoryPatterns();
    std::vector<Note> out;
    std::array<bool, 128> down {};
    double ppq = 0.0, bpm = 120.0;
    bool play = true;
    int allocations = 0;
    for (int block = 0; block < 6000; ++block)
    {
        const int n = 1 + static_cast<int> (rng.nextBelow (700));
        // The message thread's changes, between blocks.
        if (rng.nextBelow (40) == 0)
        {
            const auto l = laneAt (static_cast<int> (rng.nextBelow (3)));
            auto& x = s[static_cast<std::size_t> (laneIndex (l))];
            x.enabled = rng.nextBelow (5) != 0;
            x.arp.pattern = static_cast<osp::ArpPattern> (rng.nextBelow (osp::arp::patternCount));
            x.arp.rate = static_cast<osp::ArpRate> (rng.nextBelow (osp::arp::rateCount));
            x.arp.octaves = 1 + static_cast<int> (rng.nextBelow (4));
            x.arp.gate = 0.1 + 1.4 * rng.nextDouble();
            x.arp.swing = rng.nextDouble();
            x.arp.strum = rng.nextBelow (2) == 0;
            x.arp.custom = rng.nextBelow (3) == 0;
            x.transpose = static_cast<int> (rng.nextBelow (49)) - 24;
            x.registerLow = static_cast<int> (rng.nextBelow (80));
            x.registerHigh = x.registerLow + 11 + static_cast<int> (rng.nextBelow (40));
            x.passThrough = rng.nextBelow (4) == 0;
            c.setLaneSettings (l, x);
            if (rng.nextBelow (2) == 0)
            {
                const auto p = osp::arppattern::compile (patterns[rng.nextBelow (patterns.size())].pattern);
                c.setCustomPattern (l, &p);
            }
        }
        if (rng.nextBelow (300) == 0)
            play = ! play;
        if (rng.nextBelow (500) == 0)
            ppq = static_cast<double> (rng.nextBelow (64));   // a locate / loop jump
        if (rng.nextBelow (200) == 0)
            bpm = 60.0 + 140.0 * rng.nextDouble();
        if (rng.nextBelow (400) == 0)
            c.setActiveLanes (static_cast<unsigned> (rng.nextBelow (8)));

        std::vector<ThreeLaneCoordinator::Input> in;
        in.reserve (16);
        int offset = 0;
        while (rng.nextBelow (3) == 0 && offset < n)
        {
            const int note = 36 + static_cast<int> (rng.nextBelow (48));
            const auto r = rng.nextBelow (20);
            if (r == 0)
                in.push_back (ThreeLaneCoordinator::Input::pedal (offset, rng.nextBelow (2) == 0));
            else if (r == 1 && rng.nextBelow (10) == 0)
            {
                in.push_back (ThreeLaneCoordinator::Input::panic (offset));
                down.fill (false);
            }
            else if (! down[static_cast<std::size_t> (note)])
            {
                in.push_back (ThreeLaneCoordinator::Input::on (offset, note, 1 + static_cast<int> (rng.nextBelow (127))));
                down[static_cast<std::size_t> (note)] = true;
            }
            else
            {
                in.push_back (ThreeLaneCoordinator::Input::off (offset, note));
                down[static_cast<std::size_t> (note)] = false;
            }
            offset += static_cast<int> (rng.nextBelow (static_cast<std::uint64_t> (n)));
        }
        HostTiming t;
        t.valid = true;
        t.playing = play;
        t.bpm = bpm;
        t.ppq = ppq;
        {
            osp::test::AllocationScope scope;
            c.process (t, n, in.data(), static_cast<int> (in.size()));
            allocations += scope.count();
        }
        if (play)
            ppq += n / (44100.0 * 60.0 / bpm);
        for (int i = 0; i < c.numOutputs(); ++i)
            out.push_back ({ block, laneIndex (c.output (i).lane), c.output (i).on, c.output (i).note, 0, 1 });
        CHECK (c.droppedOutputs() == 0);
    }
    // The session ends: stop, bypass or unload.
    c.releaseAll (0);
    for (int i = 0; i < c.numOutputs(); ++i)
        out.push_back ({ 999999, laneIndex (c.output (i).lane), c.output (i).on, c.output (i).note, 0, 1 });
    CHECK (allocations == 0);
    CHECK (out.size() > 1000);
    requireBalanced (out);
}
