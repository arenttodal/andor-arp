#pragma once

#include "engine/Arpeggiator.h"
#include "model/ArpPatternCodec.h"
#include "model/ArpPatternModel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace andor::arp
{

/** A performance lane's identity. The order is stored: never reorder. */
enum class Lane : std::uint8_t
{
    A = 0,
    B = 1,
    C = 2
};
inline constexpr int laneCount = 3;
inline constexpr char laneLetter (Lane lane) noexcept { return static_cast<char> ('A' + static_cast<int> (lane)); }
inline constexpr int laneIndex (Lane lane) noexcept { return static_cast<int> (lane); }
inline constexpr Lane laneAt (int index) noexcept { return static_cast<Lane> (index < 0 ? 0 : index > 2 ? 2 : index); }

/**
    One lane's musical settings: a plain value, copied to the timing-critical thread as it is.
    The arpeggiator's own settings are OSP's (`osp::Arpeggiator::Settings`) with their stored
    meanings: style, rate, gate, swing, octaves, velocity, STRUM, CUSTOM. `arp.enabled` is not used
    (a lane's arpeggiator always runs, so muting and unmuting keep its phase); `enabled` mutes.
*/
struct LaneSettings
{
    bool enabled = true;                   ///< off: the lane is muted (its notes end, its clock runs on)
    osp::Arpeggiator::Settings arp {};
    int registerLow = 0, registerHigh = 127;   ///< the output's range: notes outside fold in by octaves (at least 12 apart)
    int transpose = 0;                     ///< semitones, -48 .. 48, before the range
    int outputChannel = 1;                 ///< 1 .. 16
    bool passThrough = false;              ///< the held keys also sound on this lane (off: only the arpeggio)
    std::uint64_t seed = 1;                ///< the random styles' seed (stored, so a reload replays them)

    /** Within their ranges (out-of-range stored values are clamped, never rejected). */
    LaneSettings sanitised() const noexcept;
    bool operator== (const LaneSettings& o) const noexcept;
};

/** A lane as the musician sees and saves it (message thread). */
struct LaneState
{
    std::string name;                                    ///< "VIOLINS"; empty: the letter
    LaneSettings settings {};
    std::optional<osp::arppattern::Document> pattern;   ///< the lane's custom pattern (embedded, whole)
    std::uint64_t revision = 0;                          ///< raised by the Master on every edit (Master/Receiver reconciliation)
    bool operator== (const LaneState&) const = default;
};

/** The seed OSP gives card `lane` for an instrument seed (lane A the seed itself), so a
    three-lane setup made in ANDOR OSP sounds the same in ANDOR ARP. */
std::uint64_t defaultLaneSeed (std::uint64_t seed, Lane lane) noexcept;

/**
    A whole ANDOR ARP performance: three lanes. Saved as JSON (kind "andor.arp-performance",
    schemaVersion 1) with each lane's pattern embedded as an `.andorpattern` document, so a
    performance never depends on a library file. Enum values are stored as OSP's stored indices
    (append-only), never as positions in a menu.
*/
struct Performance
{
    static constexpr int schemaVersion = 1;
    std::string name;
    std::array<LaneState, laneCount> lanes {};

    LaneState& lane (Lane l) noexcept { return lanes[static_cast<std::size_t> (laneIndex (l))]; }
    const LaneState& lane (Lane l) const noexcept { return lanes[static_cast<std::size_t> (laneIndex (l))]; }
    bool operator== (const Performance&) const = default;
};

/** Three lanes that are immediately musical (A: UP 1/16, B: CHORD 1/4 STRUM, C: PINKY UP 1/8 an octave down). */
Performance defaultPerformance();

osp::arppattern::JsonValue laneToJson (const LaneState& lane);
std::optional<LaneState> laneFromJson (const osp::arppattern::JsonValue& json, std::string& error);

std::string performanceToText (const Performance& performance);
std::optional<Performance> performanceFromText (std::string_view text, std::string& error);

inline constexpr const char* performanceKind = "andor.arp-performance";

} // namespace andor::arp
