// The performance document: three lanes with their patterns, saved and reloaded exactly.

#include "PerformanceModel.h"
#include "StateBlob.h"

#include "core/Prng.h"
#include "model/ArpFactoryPatterns.h"

#include <catch2/catch_test_macros.hpp>

using namespace andor::arp;

TEST_CASE ("Performance: saved and reloaded exactly, patterns embedded", "[unit][performance]")
{
    auto p = defaultPerformance();
    p.name = "Strings \"&\" Keys";
    p.lane (Lane::A).name = "VIOLINS";
    p.lane (Lane::A).pattern = osp::arppattern::factoryPatterns()[3];
    p.lane (Lane::A).settings.arp.custom = true;
    p.lane (Lane::A).revision = 41;
    p.lane (Lane::B).settings.seed = 0xFEDCBA9876543210ull;   // all 64 bits survive (stored as text)
    p.lane (Lane::C).settings.enabled = false;
    p.lane (Lane::C).settings.registerLow = 40;
    p.lane (Lane::C).settings.registerHigh = 70;
    std::string error;
    const auto text = performanceToText (p);
    const auto again = performanceFromText (text, error);
    INFO (error);
    REQUIRE (again.has_value());
    CHECK (*again == p);
    CHECK (performanceToText (*again) == text);
}

TEST_CASE ("Performance: newer schemas refused; an unreadable lane pattern is skipped and reported", "[unit][performance]")
{
    auto p = defaultPerformance();
    p.lane (Lane::B).pattern = osp::arppattern::factoryPatterns()[0];
    auto text = performanceToText (p);
    std::string error;

    auto newer = text;
    newer.replace (newer.find ("\"schemaVersion\": 1"), 18, "\"schemaVersion\": 2");
    CHECK_FALSE (performanceFromText (newer, error).has_value());
    CHECK (error.find ("newer") != std::string::npos);

    // The embedded pattern from a newer pattern schema: the lane loads without it.
    auto patternNewer = text;
    const auto at = patternNewer.find ("\"schemaVersion\": 1", patternNewer.find ("customPattern"));
    patternNewer.replace (at, 18, "\"schemaVersion\": 9");
    error.clear();
    const auto loaded = performanceFromText (patternNewer, error);
    REQUIRE (loaded.has_value());
    CHECK_FALSE (loaded->lane (Lane::B).pattern.has_value());
    CHECK (error.find ("skipped") != std::string::npos);
}

TEST_CASE ("Performance: stored values out of range are clamped, never rejected", "[unit][performance]")
{
    LaneSettings s;
    s.arp.gate = 99.0;
    s.arp.octaves = 0;
    s.transpose = 1000;
    s.registerLow = 100;
    s.registerHigh = 101;
    s.outputChannel = 0;
    s.seed = 0;
    const auto t = s.sanitised();
    CHECK (t.arp.gate == osp::arp::maxGate);
    CHECK (t.arp.octaves == 1);
    CHECK (t.transpose == 48);
    CHECK (t.registerHigh - t.registerLow >= 11);
    CHECK (t.outputChannel == 1);
    CHECK (t.seed == 1);
}

TEST_CASE ("Performance: lane seeds follow ANDOR OSP's cards", "[unit][performance]")
{
    CHECK (defaultLaneSeed (5, Lane::A) == 5);
    CHECK (defaultLaneSeed (5, Lane::B) == osp::Prng::deriveSeed (5, 1, 0x4c616e65));
    CHECK (defaultLaneSeed (5, Lane::B) != defaultLaneSeed (5, Lane::C));
}
