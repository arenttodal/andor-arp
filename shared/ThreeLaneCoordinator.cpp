#include "ThreeLaneCoordinator.h"

#include <algorithm>

namespace andor::arp
{

ThreeLaneCoordinator::Input ThreeLaneCoordinator::Input::on (int offset, int note, int velocity, int channel) noexcept
{
    Input i;
    i.offset = offset;
    i.kind = velocity > 0 ? Kind::noteOn : Kind::noteOff;
    i.note = static_cast<std::uint8_t> (std::clamp (note, 0, 127));
    i.velocity = static_cast<std::uint8_t> (std::clamp (velocity, 0, 127));
    i.channel = static_cast<std::uint8_t> (std::clamp (channel, 1, 16));
    return i;
}

ThreeLaneCoordinator::Input ThreeLaneCoordinator::Input::off (int offset, int note, int channel) noexcept
{
    auto i = on (offset, note, 0, channel);
    i.kind = Kind::noteOff;
    return i;
}

ThreeLaneCoordinator::Input ThreeLaneCoordinator::Input::pedal (int offset, bool down) noexcept
{
    Input i;
    i.offset = offset;
    i.kind = Kind::sustain;
    i.down = down;
    return i;
}

ThreeLaneCoordinator::Input ThreeLaneCoordinator::Input::panic (int offset) noexcept
{
    Input i;
    i.offset = offset;
    i.kind = Kind::allNotesOff;
    return i;
}

void ThreeLaneCoordinator::prepare (double rate) noexcept
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    for (auto& d : lanes)
        d.arp.prepare (sampleRate);
    reset();
}

void ThreeLaneCoordinator::reset() noexcept
{
    for (auto& d : lanes)
    {
        d.arp.reset();
        d.owners.fill (0);
        d.heldVelocity.fill (0);
        d.lastNote = -1;
        d.wasEnabled = d.settings.enabled;
    }
    hostWasPlaying = false;
    outputCount = dropped = 0;
}

void ThreeLaneCoordinator::setActiveLanes (unsigned mask) noexcept
{
    pendingMask = mask & 7u;
}

void ThreeLaneCoordinator::setLaneSettings (Lane lane, const LaneSettings& settings) noexcept
{
    auto& d = state (lane);
    d.pending = settings.sanitised();
    d.settingsPending = true;
}

void ThreeLaneCoordinator::setCustomPattern (Lane lane, const osp::arp::CustomPattern* pattern) noexcept
{
    auto& d = state (lane);
    if (pattern != nullptr && pattern->valid())
    {
        d.patternNext = *pattern;
        d.patternClear = false;
    }
    else
        d.patternClear = true;
    d.patternPending = true;
}

int ThreeLaneCoordinator::mapPitch (const LaneSettings& s, int note) const noexcept
{
    int p = note + s.transpose;
    // The range is at least 12 wide (sanitised), so folding by octaves always lands inside it.
    while (p < s.registerLow)
        p += 12;
    while (p > s.registerHigh)
        p -= 12;
    return std::clamp (p, 0, 127);
}

void ThreeLaneCoordinator::emit (int offset, Lane lane, bool on, int note, int velocity, int channel) noexcept
{
    if (outputCount >= maxOutputs)
    {
        ++dropped;
        return;
    }
    auto& o = outputs[static_cast<std::size_t> (outputCount++)];
    o.offset = offset;
    o.lane = lane;
    o.on = on;
    o.note = static_cast<std::uint8_t> (note);
    o.velocity = static_cast<std::uint8_t> (std::clamp (velocity, on ? 1 : 0, 127));
    o.channel = static_cast<std::uint8_t> (channel);
}

void ThreeLaneCoordinator::startNote (LaneData& d, Lane lane, int offset, int pitch, int velocity) noexcept
{
    if (! d.settings.enabled)
        return;   // muted: nothing starts (and so nothing needs ending)
    auto& owners = d.owners[static_cast<std::size_t> (pitch)];
    if (owners > 0)
        emit (offset, lane, false, pitch, 0, d.settings.outputChannel);   // the pitch again: it retriggers
    if (owners < 255)
        ++owners;
    emit (offset, lane, true, pitch, velocity, d.settings.outputChannel);
    d.lastNote = pitch;
}

void ThreeLaneCoordinator::endNote (LaneData& d, Lane lane, int offset, int pitch) noexcept
{
    auto& owners = d.owners[static_cast<std::size_t> (pitch)];
    if (owners == 0)
        return;   // never started here (muted, or already released by releaseAll)
    if (--owners == 0)
        emit (offset, lane, false, pitch, 0, d.settings.outputChannel);
}

void ThreeLaneCoordinator::releaseLane (LaneData& d, Lane lane, int offset) noexcept
{
    for (int p = 0; p < 128; ++p)
        if (d.owners[static_cast<std::size_t> (p)] > 0)
        {
            d.owners[static_cast<std::size_t> (p)] = 0;
            emit (offset, lane, false, p, 0, d.settings.outputChannel);
        }
    d.heldVelocity.fill (0);
}

void ThreeLaneCoordinator::process (const HostTiming& timing, int numSamples, const Input* inputs, int numInputs) noexcept
{
    outputCount = dropped = 0;
    numSamples = std::max (1, numSamples);

    // The host's transport started: every lane's pattern and random choices start over, so a
    // bounce is the same every time (as ANDOR OSP's processor does).
    const bool playing = timing.valid && timing.playing;
    const bool restart = playing && ! hostWasPlaying;
    hostWasPlaying = playing;

    const unsigned previousMask = laneMask;
    laneMask = pendingMask;

    for (int li = 0; li < laneCount; ++li)
    {
        const auto lane = laneAt (li);
        auto& d = lanes[static_cast<std::size_t> (li)];
        const bool active = (laneMask >> li) & 1u;
        const bool wasActive = (previousMask >> li) & 1u;
        if (! active)
        {
            if (wasActive)
            {
                releaseLane (d, lane, 0);
                d.arp.reset();
            }
            d.active = false;
            continue;
        }
        d.active = true;

        if (d.settingsPending)
        {
            const bool passWasOn = d.settings.passThrough;
            const bool mapChanged = d.pending.transpose != d.settings.transpose || d.pending.registerLow != d.settings.registerLow
                                 || d.pending.registerHigh != d.settings.registerHigh || d.pending.outputChannel != d.settings.outputChannel;
            if (mapChanged)
                releaseLane (d, lane, 0);   // notes started under the old mapping end now
            d.settings = d.pending;
            d.settingsPending = false;
            if (passWasOn && ! d.settings.passThrough)
                for (int n = 0; n < 128; ++n)
                    if (d.heldVelocity[static_cast<std::size_t> (n)] > 0)
                    {
                        d.heldVelocity[static_cast<std::size_t> (n)] = 0;
                        endNote (d, lane, 0, n);
                    }
        }
        if (d.patternPending)
        {
            if (d.patternClear)
                d.arp.clearCustomPattern();
            else
                d.arp.setCustomPattern (d.patternNext);
            d.patternPending = false;
        }
        if (d.wasEnabled && ! d.settings.enabled)
            releaseLane (d, lane, 0);   // muted: its notes end; its clock runs on
        d.wasEnabled = d.settings.enabled;

        if (restart)
            d.arp.restartPattern();
        auto arpSettings = d.settings.arp;
        arpSettings.enabled = true;   // always: muting must not hand the keys back to the instrument
        d.arp.setSeed (d.settings.seed);
        d.arp.beginBlock (arpSettings, timing, numSamples);

        // The keys, at their samples. The arpeggiator's events and the pass-through notes are
        // written in offset order (the arpeggiator's own list is ordered).
        int next = 0;   // the arpeggiator's next unread event
        auto flushArp = [&] (int upTo, bool all) {
            for (; next < d.arp.numEvents(); ++next)
            {
                const auto& e = d.arp.event (next);
                if (! all && e.offset > upTo)
                    break;
                using K = osp::Arpeggiator::Event::Kind;
                if (e.kind == K::noteOn)
                    startNote (d, lane, e.offset, mapPitch (d.settings, e.note), e.velocity);
                else if (e.kind == K::noteOff)
                    endNote (d, lane, e.offset, mapPitch (d.settings, e.note));
                // (the arpeggiator never emits pedal events while it owns the notes)
            }
        };
        for (int i = 0; i < numInputs; ++i)
        {
            const auto& in = inputs[i];
            const int offset = std::clamp (in.offset, 0, numSamples - 1);
            d.arp.advanceTo (offset);
            flushArp (offset, false);
            auto keyUp = [&] {
                d.arp.noteOff (offset, in.note, in.channel);
                if (d.heldVelocity[in.note] > 0)
                {
                    d.heldVelocity[in.note] = 0;
                    endNote (d, lane, offset, in.note);
                }
            };
            switch (in.kind)
            {
                case Input::Kind::noteOn:
                    if (in.velocity == 0)
                    {
                        keyUp();
                        break;
                    }
                    d.arp.noteOn (offset, in.note, in.velocity, in.channel);
                    if (d.settings.passThrough)
                    {
                        if (d.heldVelocity[in.note] > 0)
                            endNote (d, lane, offset, in.note);
                        d.heldVelocity[in.note] = in.velocity;
                        startNote (d, lane, offset, in.note, in.velocity);
                    }
                    break;
                case Input::Kind::noteOff:
                    keyUp();
                    break;
                case Input::Kind::sustain:
                    d.arp.sustainPedal (offset, in.down);
                    break;
                case Input::Kind::allNotesOff:
                    d.arp.allNotesOff (offset);
                    flushArp (offset, true);
                    for (int n = 0; n < 128; ++n)
                        if (d.heldVelocity[static_cast<std::size_t> (n)] > 0)
                        {
                            d.heldVelocity[static_cast<std::size_t> (n)] = 0;
                            endNote (d, lane, offset, n);
                        }
                    break;
            }
            flushArp (offset, false);
        }
        d.arp.endBlock();
        flushArp (0, true);
    }
}

void ThreeLaneCoordinator::releaseAll (int offset) noexcept
{
    for (int li = 0; li < laneCount; ++li)
    {
        auto& d = lanes[static_cast<std::size_t> (li)];
        releaseLane (d, laneAt (li), std::max (0, offset));
        d.arp.reset();
    }
}

ThreeLaneCoordinator::LaneStatus ThreeLaneCoordinator::status (Lane lane) const noexcept
{
    const auto& d = state (lane);
    LaneStatus s;
    s.active = d.active && ((laneMask >> laneIndex (lane)) & 1u);
    s.enabled = d.settings.enabled;
    s.running = d.arp.isRunning();
    s.synced = d.arp.isSynced();
    s.customPlaying = d.arp.customActive();
    s.held = d.arp.heldCount();
    int sounding = 0;
    for (auto o : d.owners)
        sounding += o > 0 ? 1 : 0;
    s.sounding = sounding;
    s.lastNote = d.lastNote;
    return s;
}

} // namespace andor::arp
