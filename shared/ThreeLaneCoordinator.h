#pragma once

#include "HostAdapterContract.h"
#include "PerformanceModel.h"

#include "engine/ArpCustomPattern.h"
#include "engine/Arpeggiator.h"

#include <array>
#include <cstdint>

namespace andor::arp
{

/**
    Up to three independent performances (lanes A, B, C) of one musical input: every lane's
    arpeggiator (OSP's, unchanged) hears the same keys and the same transport and plays its own
    settings and pattern. See HostAdapterContract.h for what a host must provide.

    Lanes are identified explicitly (Lane::A/B/C); MIDI channels are never used to route between
    them. The Max for Live Receiver runs one lane (setActiveLanes); the JUCE instrument host runs
    all three in one processor and sends each lane's events to its own hosted instrument.

    What it adds around the arpeggiator, per lane:
    - mute without losing the phase (the arpeggiator always runs; a muted lane ends its notes and
      emits nothing until it is unmuted, then rejoins on the next step);
    - transposition and an output register (notes outside it fold in by octaves);
    - an output pitch owned by two generated notes at once (folding or transposing can make two
      pitches meet) is retriggered, and released only when its last owner ends: no stuck notes;
    - optional pass-through of the held keys;
    - releaseAll(): every note a lane started ends now (stop, bypass, unload, reset).

    Real-time safe: fixed capacity, no allocation, no locks. Settings and patterns are copied in
    between blocks by the thread that calls process().
*/
class ThreeLaneCoordinator
{
public:
    struct Input
    {
        enum class Kind : std::uint8_t
        {
            noteOn,
            noteOff,
            sustain,        ///< value: true when down
            allNotesOff
        };
        int offset = 0;
        Kind kind = Kind::noteOn;
        std::uint8_t note = 0, velocity = 0, channel = 1;
        bool down = false;

        static Input on (int offset, int note, int velocity, int channel = 1) noexcept;
        static Input off (int offset, int note, int channel = 1) noexcept;
        static Input pedal (int offset, bool down) noexcept;
        static Input panic (int offset) noexcept;
    };

    struct Output
    {
        int offset = 0;
        Lane lane = Lane::A;
        bool on = true;   ///< false: note off
        std::uint8_t note = 0, velocity = 0, channel = 1;
    };

    struct LaneStatus
    {
        bool active = false;      ///< this instance runs the lane
        bool enabled = false;     ///< not muted
        bool running = false;     ///< notes held, the pattern plays
        bool synced = false;      ///< on the host's grid
        bool customPlaying = false;
        int held = 0, sounding = 0;
        int lastNote = -1;        ///< the last note-on sent (-1: none yet)
    };

    static constexpr int maxOutputs = 3 * osp::Arpeggiator::maxEvents + 2 * 128 * 3;

    void prepare (double sampleRate) noexcept;
    /** Forgets everything without emitting events (call releaseAll first when notes sound). */
    void reset() noexcept;

    /** Which lanes this instance runs (bit 0 A, 1 B, 2 C; default all). A lane switched out
        releases its notes in the next block. */
    void setActiveLanes (unsigned mask) noexcept;
    unsigned activeLanes() const noexcept { return laneMask; }

    /** Takes effect in the next process() call; a changed style or rate follows the
        arpeggiator's own rules (the pattern continues from its step; a new custom pattern takes
        over at the next step with the phase kept). */
    void setLaneSettings (Lane lane, const LaneSettings& settings) noexcept;
    const LaneSettings& laneSettings (Lane lane) const noexcept { return state (lane).settings; }
    /** nullptr or an invalid pattern clears it (CUSTOM then plays the style, never silence). */
    void setCustomPattern (Lane lane, const osp::arp::CustomPattern* pattern) noexcept;

    /** One block: `inputs` ordered by offset, all within [0, numSamples). */
    void process (const HostTiming& timing, int numSamples, const Input* inputs, int numInputs) noexcept;
    /** Ends every note any lane started, at `offset` of the block just processed (appended to
        its outputs), and forgets the held keys (a stop, bypass or unload). */
    void releaseAll (int offset) noexcept;

    int numOutputs() const noexcept { return outputCount; }
    const Output& output (int index) const noexcept { return outputs[static_cast<std::size_t> (index)]; }
    /** Events the block could not hold (never written out of bounds); 0 normally. */
    int droppedOutputs() const noexcept { return dropped; }

    LaneStatus status (Lane lane) const noexcept;
    const osp::Arpeggiator& arpeggiator (Lane lane) const noexcept { return state (lane).arp; }

private:
    struct LaneData
    {
        osp::Arpeggiator arp;
        LaneSettings settings {}, pending {};
        bool settingsPending = false;
        osp::arp::CustomPattern patternNext {};
        bool patternPending = false, patternClear = false;
        bool active = true, wasEnabled = true;
        std::array<std::uint8_t, 128> owners {};     ///< generated notes sounding on each output pitch
        std::array<std::uint8_t, 128> heldVelocity {};   ///< pass-through: keys held (velocity, 0: up)
        int lastNote = -1;
    };

    LaneData& state (Lane lane) noexcept { return lanes[static_cast<std::size_t> (laneIndex (lane))]; }
    const LaneData& state (Lane lane) const noexcept { return lanes[static_cast<std::size_t> (laneIndex (lane))]; }

    int mapPitch (const LaneSettings& s, int note) const noexcept;
    void emit (int offset, Lane lane, bool on, int note, int velocity, int channel) noexcept;
    void startNote (LaneData& d, Lane lane, int offset, int pitch, int velocity) noexcept;
    void endNote (LaneData& d, Lane lane, int offset, int pitch) noexcept;
    void releaseLane (LaneData& d, Lane lane, int offset) noexcept;
    void sortOutputs() noexcept;

    double sampleRate = 48000.0;
    unsigned laneMask = 7, pendingMask = 7;
    bool hostWasPlaying = false;
    std::array<LaneData, laneCount> lanes {};
    std::array<Output, maxOutputs> outputs {};
    int outputCount = 0, dropped = 0;
};

} // namespace andor::arp
