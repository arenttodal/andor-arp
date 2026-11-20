#include "PerformanceModel.h"

#include "core/Prng.h"

#include <algorithm>
#include <cmath>

namespace andor::arp
{

using osp::arppattern::JsonValue;
namespace oa = osp::arp;

namespace
{
    bool sameArp (const osp::Arpeggiator::Settings& a, const osp::Arpeggiator::Settings& b) noexcept
    {
        return a.pattern == b.pattern && a.rate == b.rate && a.gate == b.gate && a.swing == b.swing && a.octaves == b.octaves
            && a.rateMultiplier == b.rateMultiplier && a.rateStepped == b.rateStepped && a.velocityScale == b.velocityScale
            && a.octavesAtCycle == b.octavesAtCycle && a.strum == b.strum && a.strumDirection == b.strumDirection
            && a.strumSync == b.strumSync && a.strumMs == b.strumMs && a.strumQuarters == b.strumQuarters && a.custom == b.custom;
    }

    double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }
}

LaneSettings LaneSettings::sanitised() const noexcept
{
    LaneSettings s = *this;
    auto& a = s.arp;
    a.enabled = true;
    a.pattern = static_cast<osp::ArpPattern> (std::clamp (static_cast<int> (a.pattern), 0, oa::patternCount - 1));
    a.rate = static_cast<osp::ArpRate> (std::clamp (static_cast<int> (a.rate), 0, oa::rateCount - 1));
    a.gate = std::clamp (finiteOr (a.gate, 0.75), oa::minGate, oa::maxGate);
    a.swing = std::clamp (finiteOr (a.swing, 0.0), 0.0, oa::maxSwing);
    a.octaves = std::clamp (a.octaves, oa::minOctaves, oa::maxOctaves);
    a.rateMultiplier = std::clamp (finiteOr (a.rateMultiplier, 1.0), 0.25, 4.0);
    a.velocityScale = std::clamp (finiteOr (a.velocityScale, 1.0), 0.0, 2.0);
    a.strumDirection = static_cast<osp::StrumDirection> (std::clamp (static_cast<int> (a.strumDirection), 0, oa::strumDirectionCount - 1));
    a.strumMs = std::clamp (finiteOr (a.strumMs, 20.0), 0.0, oa::maxStrumMs);
    a.strumQuarters = std::clamp (finiteOr (a.strumQuarters, 0.125), 0.0, 4.0);
    s.transpose = std::clamp (s.transpose, -48, 48);
    s.registerLow = std::clamp (s.registerLow, 0, 127);
    s.registerHigh = std::clamp (s.registerHigh, 0, 127);
    if (s.registerHigh - s.registerLow < 11)
    {
        // At least an octave, so folding by octaves always lands inside.
        s.registerHigh = std::min (127, s.registerLow + 11);
        s.registerLow = s.registerHigh - 11;
    }
    s.outputChannel = std::clamp (s.outputChannel, 1, 16);
    if (s.seed == 0)
        s.seed = 1;
    return s;
}

bool LaneSettings::operator== (const LaneSettings& o) const noexcept
{
    return enabled == o.enabled && sameArp (arp, o.arp) && registerLow == o.registerLow && registerHigh == o.registerHigh
        && transpose == o.transpose && outputChannel == o.outputChannel && passThrough == o.passThrough && seed == o.seed;
}

std::uint64_t defaultLaneSeed (std::uint64_t seed, Lane lane) noexcept
{
    // shared/andor-ui/Source/CardArpLanes.cpp: lane A plays the seed, B and C derive theirs.
    return lane == Lane::A ? seed : osp::Prng::deriveSeed (seed, static_cast<std::uint64_t> (laneIndex (lane)), 0x4c616e65);
}

Performance defaultPerformance()
{
    Performance p;
    p.name = "Init";
    const char* names[] = { "A", "B", "C" };
    for (int i = 0; i < laneCount; ++i)
    {
        auto& lane = p.lanes[static_cast<std::size_t> (i)];
        lane.name = names[i];
        lane.settings.seed = defaultLaneSeed (1, laneAt (i));
        lane.settings.arp.enabled = true;
    }
    auto& a = p.lane (Lane::A).settings;
    a.arp.pattern = osp::ArpPattern::up;
    a.arp.rate = osp::ArpRate::sixteenth;
    a.arp.octaves = 2;
    a.arp.gate = 0.6;
    auto& b = p.lane (Lane::B).settings;
    b.arp.pattern = osp::ArpPattern::chord;
    b.arp.rate = osp::ArpRate::quarter;
    b.arp.gate = 0.9;
    b.arp.strum = true;
    b.arp.strumDirection = osp::StrumDirection::alternate;
    b.arp.strumSync = true;
    b.arp.strumQuarters = oa::strumDivisionQuarters (oa::defaultStrumDivision);
    auto& c = p.lane (Lane::C).settings;
    c.arp.pattern = osp::ArpPattern::pinkyUp;
    c.arp.rate = osp::ArpRate::eighth;
    c.transpose = -12;
    c.arp.gate = 0.5;
    return p;
}

// ---------------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------------

namespace
{
    int getInt (const JsonValue& o, const char* key, int fallback)
    {
        const auto& v = o[key];
        return v.isNumber() ? v.toInt() : fallback;
    }
    double getDouble (const JsonValue& o, const char* key, double fallback)
    {
        const auto& v = o[key];
        return v.isNumber() ? v.number : fallback;
    }
    bool getBool (const JsonValue& o, const char* key, bool fallback)
    {
        const auto& v = o[key];
        return v.isBool() ? v.boolean : fallback;
    }
    std::string getString (const JsonValue& o, const char* key, const std::string& fallback = {})
    {
        const auto& v = o[key];
        return v.isString() ? v.text : fallback;
    }
    JsonValue num (double v)
    {
        return JsonValue::makeDouble (std::isfinite (v) ? std::round (v * 1.0e6) / 1.0e6 + 0.0 : 0.0);
    }
    /** 64-bit seeds as decimal text: JSON numbers are doubles, which keep only 53 bits. */
    JsonValue seedText (std::uint64_t seed) { return JsonValue::makeString (std::to_string (seed)); }
    std::uint64_t seedFrom (const JsonValue& v, std::uint64_t fallback)
    {
        if (v.isString())
        {
            std::uint64_t s = 0;
            bool any = false;
            for (const char c : v.text)
            {
                if (c < '0' || c > '9')
                    return fallback;
                const auto d = static_cast<std::uint64_t> (c - '0');
                if (s > (UINT64_MAX - d) / 10)
                    return fallback;
                s = s * 10 + d;
                any = true;
            }
            return any && s != 0 ? s : fallback;
        }
        if (v.isNumber() && v.number >= 1.0 && v.number < 9.0e15)
            return static_cast<std::uint64_t> (v.number);
        return fallback;
    }
}

JsonValue laneToJson (const LaneState& lane)
{
    const auto& s = lane.settings;
    const auto& a = s.arp;
    auto o = JsonValue::makeObject();
    o.set ("name", JsonValue::makeString (lane.name));
    o.set ("enabled", JsonValue::makeBool (s.enabled));
    o.set ("revision", JsonValue::makeString (std::to_string (lane.revision)));
    // OSP's stored indices (append-only enums), so a value means the same in every product.
    o.set ("pattern", JsonValue::makeInt (static_cast<int> (a.pattern)));
    o.set ("rate", JsonValue::makeInt (static_cast<int> (a.rate)));
    o.set ("gate", num (a.gate));
    o.set ("swing", num (a.swing));
    o.set ("octaves", JsonValue::makeInt (a.octaves));
    o.set ("velocity", num (a.velocityScale));
    o.set ("custom", JsonValue::makeBool (a.custom));
    auto strum = JsonValue::makeObject();
    strum.set ("on", JsonValue::makeBool (a.strum));
    strum.set ("direction", JsonValue::makeInt (static_cast<int> (a.strumDirection)));
    strum.set ("sync", JsonValue::makeBool (a.strumSync));
    strum.set ("ms", num (a.strumMs));
    strum.set ("quarters", num (a.strumQuarters));
    o.set ("strum", std::move (strum));
    o.set ("registerLow", JsonValue::makeInt (s.registerLow));
    o.set ("registerHigh", JsonValue::makeInt (s.registerHigh));
    o.set ("transpose", JsonValue::makeInt (s.transpose));
    o.set ("channel", JsonValue::makeInt (s.outputChannel));
    o.set ("passThrough", JsonValue::makeBool (s.passThrough));
    o.set ("seed", seedText (s.seed));
    if (lane.pattern)
        o.set ("customPattern", osp::arppattern::documentToJson (*lane.pattern));
    return o;
}

std::optional<LaneState> laneFromJson (const JsonValue& o, std::string& error)
{
    if (! o.isObject())
    {
        error = "a lane is not a JSON object";
        return std::nullopt;
    }
    LaneState lane;
    auto& s = lane.settings;
    auto& a = s.arp;
    lane.name = getString (o, "name");
    lane.revision = seedFrom (o["revision"], 0);
    s.enabled = getBool (o, "enabled", true);
    a.pattern = static_cast<osp::ArpPattern> (getInt (o, "pattern", 0));
    a.rate = static_cast<osp::ArpRate> (getInt (o, "rate", static_cast<int> (osp::ArpRate::eighth)));
    a.gate = getDouble (o, "gate", a.gate);
    a.swing = getDouble (o, "swing", a.swing);
    a.octaves = getInt (o, "octaves", a.octaves);
    a.velocityScale = getDouble (o, "velocity", a.velocityScale);
    a.custom = getBool (o, "custom", false);
    const auto& strum = o["strum"];
    a.strum = getBool (strum, "on", false);
    a.strumDirection = static_cast<osp::StrumDirection> (getInt (strum, "direction", 0));
    a.strumSync = getBool (strum, "sync", false);
    a.strumMs = getDouble (strum, "ms", a.strumMs);
    a.strumQuarters = getDouble (strum, "quarters", a.strumQuarters);
    s.registerLow = getInt (o, "registerLow", 0);
    s.registerHigh = getInt (o, "registerHigh", 127);
    s.transpose = getInt (o, "transpose", 0);
    s.outputChannel = getInt (o, "channel", 1);
    s.passThrough = getBool (o, "passThrough", false);
    s.seed = seedFrom (o["seed"], 1);
    if (const auto& doc = o["customPattern"]; doc.isObject())
    {
        // A pattern this build cannot read (a newer schema) is reported; the lane keeps its other
        // settings and plays its style, never a misread pattern.
        std::string patternError;
        if (auto d = osp::arppattern::documentFromJson (doc, patternError))
            lane.pattern = std::move (*d);
        else
            error = "lane " + lane.name + ": custom pattern skipped (" + patternError + ")";
    }
    s = s.sanitised();
    return lane;
}

std::string performanceToText (const Performance& p)
{
    auto root = JsonValue::makeObject();
    root.set ("schemaVersion", JsonValue::makeInt (Performance::schemaVersion));
    root.set ("kind", JsonValue::makeString (performanceKind));
    root.set ("name", JsonValue::makeString (p.name));
    auto lanes = JsonValue::makeArray();
    for (const auto& lane : p.lanes)
        lanes.append (laneToJson (lane));
    root.set ("lanes", std::move (lanes));
    return osp::arppattern::writeJson (root);
}

std::optional<Performance> performanceFromText (std::string_view text, std::string& error)
{
    const auto root = osp::arppattern::parseJson (text, error);
    if (! root)
        return std::nullopt;
    if (! root->isObject())
    {
        error = "not a performance (no JSON object)";
        return std::nullopt;
    }
    const int version = getInt (*root, "schemaVersion", 0);
    if (version < 1 || version > Performance::schemaVersion)
    {
        error = version > Performance::schemaVersion ? "made by a newer version (performance schema " + std::to_string (version) + ")"
                                                     : "not a performance (no schemaVersion)";
        return std::nullopt;
    }
    if (getString (*root, "kind") != performanceKind)
    {
        error = "not an ANDOR ARP performance";
        return std::nullopt;
    }
    Performance p;
    p.name = getString (*root, "name");
    const auto& lanes = (*root)["lanes"];
    if (! lanes.isArray() || lanes.items.size() != laneCount)
    {
        error = "a performance has exactly three lanes";
        return std::nullopt;
    }
    for (int i = 0; i < laneCount; ++i)
    {
        std::string laneError;
        auto lane = laneFromJson (lanes.items[static_cast<std::size_t> (i)], laneError);
        if (! lane)
        {
            error = laneError;
            return std::nullopt;
        }
        if (! laneError.empty())
            error = laneError;   // reported; the performance still loads
        p.lanes[static_cast<std::size_t> (i)] = std::move (*lane);
    }
    return p;
}

} // namespace andor::arp
