#pragma once

// Helpers shared by ANDOR ARP's tests: render the coordinator (or one bare OSP arpeggiator, the
// reference) over a timeline of input events and transport, with absolute sample times.

#include "ThreeLaneCoordinator.h"

#include <functional>
#include <string>
#include <vector>

namespace andor_arp_test
{
using namespace andor::arp;

struct TimedInput
{
    long long sample = 0;
    ThreeLaneCoordinator::Input input;
};

struct Note
{
    long long sample = 0;
    int lane = 0;
    bool on = true;
    int note = 0, velocity = 0, channel = 1;
    bool operator== (const Note&) const = default;
};

/** The host's transport at an absolute sample (blocks take it at their first sample). */
using Transport = std::function<HostTiming (long long sample)>;

inline HostTiming playingAt (double bpm, double sampleRate, long long sample, double startPpq = 0.0)
{
    HostTiming t;
    t.valid = t.playing = true;
    t.bpm = bpm;
    t.ppq = startPpq + static_cast<double> (sample) / (sampleRate * 60.0 / bpm);
    return t;
}

/** Runs `total` samples in blocks of `block` (or of sizes from `sizes`, cycling). */
std::vector<Note> renderCoordinator (ThreeLaneCoordinator& c, const std::vector<TimedInput>& inputs, long long total, const Transport& transport,
                                     const std::vector<int>& sizes, const std::function<void (long long blockStart)>& beforeBlock = {});

/** A bare osp::Arpeggiator with lane settings `s` (the reference a lane must equal). */
std::vector<Note> renderReference (const LaneSettings& s, const osp::arp::CustomPattern* pattern, const std::vector<TimedInput>& inputs, long long total,
                                   const Transport& transport, int block, double sampleRate);

std::vector<Note> laneOnly (const std::vector<Note>& notes, int lane);
std::string describe (const std::vector<Note>& notes, std::size_t max = 12);

} // namespace andor_arp_test
