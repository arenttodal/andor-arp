// The three example performances (presets/*.andorperformance): defined here, kept equal to the files.
//   ANDOR_ARP_UPDATE_PRESETS=1 ./build/andor_arp_tests "[presets]"   rewrites them

#include "PerformanceModel.h"
#include "TestRender.h"

#include "model/ArpFactoryPatterns.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace andor::arp;

namespace
{
    LaneState laneOf (const char* name, osp::ArpPattern p, osp::ArpRate r, int octaves, double gate, Lane lane)
    {
        LaneState l;
        l.name = name;
        l.settings.arp.pattern = p;
        l.settings.arp.rate = r;
        l.settings.arp.octaves = octaves;
        l.settings.arp.gate = gate;
        l.settings.seed = defaultLaneSeed (2026, lane);
        l.revision = 1;
        return l;
    }

    std::vector<std::pair<std::string, Performance>> presets()
    {
        std::vector<std::pair<std::string, Performance>> out;

        Performance strings;   // pizzicato under a held pad, a slow upper line
        strings.name = "String Section";
        strings.lane (Lane::A) = laneOf ("PIZZ", osp::ArpPattern::upDown, osp::ArpRate::eighth, 2, 0.35, Lane::A);
        strings.lane (Lane::A).settings.registerLow = 43;
        strings.lane (Lane::A).settings.registerHigh = 79;
        strings.lane (Lane::B) = laneOf ("PAD", osp::ArpPattern::chord, osp::ArpRate::quarter, 1, 1.0, Lane::B);
        strings.lane (Lane::B).settings.arp.strum = true;
        strings.lane (Lane::B).settings.arp.strumMs = 60.0;
        strings.lane (Lane::C) = laneOf ("LINE", osp::ArpPattern::random, osp::ArpRate::quarterDotted, 1, 0.9, Lane::C);
        strings.lane (Lane::C).settings.transpose = 12;
        out.push_back ({ "string-section", strings });

        Performance pulse;     // a sixteenth sequence, a triplet counterline, a bass pedal pattern
        pulse.name = "Pulse Trio";
        pulse.lane (Lane::A) = laneOf ("SEQ", osp::ArpPattern::up, osp::ArpRate::sixteenth, 2, 0.5, Lane::A);
        pulse.lane (Lane::A).settings.arp.swing = 0.2;
        pulse.lane (Lane::B) = laneOf ("TRIPLETS", osp::ArpPattern::convergeDiverge, osp::ArpRate::eighthTriplet, 1, 0.6, Lane::B);
        pulse.lane (Lane::C) = laneOf ("BASS", osp::ArpPattern::up, osp::ArpRate::eighth, 1, 0.8, Lane::C);
        pulse.lane (Lane::C).pattern = osp::arppattern::factoryPatterns()[1];   // Pedal Bass
        pulse.lane (Lane::C).settings.arp.custom = true;
        pulse.lane (Lane::C).settings.transpose = -24;
        pulse.lane (Lane::C).settings.registerLow = 28;
        pulse.lane (Lane::C).settings.registerHigh = 55;
        out.push_back ({ "pulse-trio", pulse });

        Performance keys;      // strummed chords, a gallop, a sparse high voice
        keys.name = "Keys and Bells";
        keys.lane (Lane::A) = laneOf ("STRUM", osp::ArpPattern::chord, osp::ArpRate::eighth, 1, 0.7, Lane::A);
        keys.lane (Lane::A).settings.arp.strum = true;
        keys.lane (Lane::A).settings.arp.strumDirection = osp::StrumDirection::alternate;
        keys.lane (Lane::A).settings.arp.strumSync = true;
        keys.lane (Lane::A).settings.arp.strumQuarters = osp::arp::strumDivisionQuarters (2);
        keys.lane (Lane::B) = laneOf ("GALLOP", osp::ArpPattern::up, osp::ArpRate::sixteenth, 1, 0.6, Lane::B);
        keys.lane (Lane::B).pattern = osp::arppattern::factoryPatterns()[3];     // Gallop
        keys.lane (Lane::B).settings.arp.custom = true;
        keys.lane (Lane::C) = laneOf ("BELLS", osp::ArpPattern::pinkyUp, osp::ArpRate::eighth, 2, 0.4, Lane::C);
        keys.lane (Lane::C).pattern = osp::arppattern::factoryPatterns()[5];     // Sparse
        keys.lane (Lane::C).settings.arp.custom = true;
        keys.lane (Lane::C).settings.transpose = 24;
        out.push_back ({ "keys-and-bells", keys });
        return out;
    }
}

TEST_CASE ("Presets: the example performances load, play on every lane and equal their files", "[unit][presets]")
{
    const bool update = std::getenv ("ANDOR_ARP_UPDATE_PRESETS") != nullptr;
    for (const auto& [stem, p] : presets())
    {
        const auto file = std::filesystem::path (ANDOR_ARP_SOURCE_DIR) / "presets" / (stem + ".andorperformance");
        const auto text = performanceToText (p);
        if (update)
        {
            std::ofstream (file, std::ios::binary) << text << "\n";
            continue;
        }
        std::ifstream in (file, std::ios::binary);
        std::stringstream s;
        s << in.rdbuf();
        INFO (file.string());
        CHECK (s.str() == text + "\n");
        std::string error;
        const auto loaded = performanceFromText (s.str(), error);
        REQUIRE (loaded.has_value());
        CHECK (*loaded == p);

        ThreeLaneCoordinator c;
        c.prepare (48000.0);
        for (int i = 0; i < laneCount; ++i)
        {
            const auto& l = loaded->lanes[static_cast<std::size_t> (i)];
            c.setLaneSettings (laneAt (i), l.settings);
            if (l.pattern)
            {
                const auto compiled = osp::arppattern::compile (l.pattern->pattern);
                c.setCustomPattern (laneAt (i), &compiled);
            }
        }
        std::vector<andor_arp_test::TimedInput> chord;
        for (int n : { 48, 55, 60, 64 })
            chord.push_back ({ 0, ThreeLaneCoordinator::Input::on (0, n, 100) });
        const auto notes = andor_arp_test::renderCoordinator (c, chord, 4 * 48000, [] (long long sample) {
            return andor_arp_test::playingAt (110.0, 48000.0, sample);
        }, { 256 });
        for (int i = 0; i < laneCount; ++i)
        {
            INFO (stem << " lane " << i);
            CHECK (andor_arp_test::laneOnly (notes, i).size() > 6);
        }
    }
}
