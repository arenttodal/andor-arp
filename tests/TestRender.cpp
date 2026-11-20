#include "TestRender.h"

#include <algorithm>
#include <sstream>

namespace andor_arp_test
{

std::vector<Note> renderCoordinator (ThreeLaneCoordinator& c, const std::vector<TimedInput>& inputs, long long total, const Transport& transport,
                                     const std::vector<int>& sizes, const std::function<void (long long)>& beforeBlock)
{
    std::vector<Note> out;
    std::vector<ThreeLaneCoordinator::Input> block;
    std::size_t next = 0, sizeIndex = 0;
    for (long long start = 0; start < total;)
    {
        const int n = static_cast<int> (std::min<long long> (sizes[sizeIndex++ % sizes.size()], total - start));
        if (beforeBlock)
            beforeBlock (start);
        block.clear();
        while (next < inputs.size() && inputs[next].sample < start + n)
        {
            auto in = inputs[next].input;
            in.offset = static_cast<int> (std::max (0LL, inputs[next].sample - start));
            block.push_back (in);
            ++next;
        }
        c.process (transport (start), n, block.data(), static_cast<int> (block.size()));
        for (int i = 0; i < c.numOutputs(); ++i)
        {
            const auto& o = c.output (i);
            out.push_back ({ start + o.offset, laneIndex (o.lane), o.on, o.note, o.velocity, o.channel });
        }
        start += n;
    }
    return out;
}

std::vector<Note> renderReference (const LaneSettings& settings, const osp::arp::CustomPattern* pattern, const std::vector<TimedInput>& inputs,
                                   long long total, const Transport& transport, int blockSize, double sampleRate)
{
    // OSP's own way of driving the arpeggiator (PluginProcessor::runArpeggiator), restart on the
    // transport's start included.
    osp::Arpeggiator a;
    a.prepare (sampleRate);
    if (pattern != nullptr)
        a.setCustomPattern (*pattern);
    auto s = settings.arp;
    s.enabled = true;
    std::vector<Note> out;
    std::size_t next = 0;
    bool wasPlaying = false;
    for (long long start = 0; start < total; start += blockSize)
    {
        const int n = static_cast<int> (std::min<long long> (blockSize, total - start));
        const auto t = transport (start);
        if (t.valid && t.playing && ! wasPlaying)
            a.restartPattern();
        wasPlaying = t.valid && t.playing;
        a.setSeed (settings.seed);
        a.beginBlock (s, t, n);
        while (next < inputs.size() && inputs[next].sample < start + n)
        {
            const auto& in = inputs[next].input;
            const int offset = static_cast<int> (std::max (0LL, inputs[next].sample - start));
            a.advanceTo (offset);
            using K = ThreeLaneCoordinator::Input::Kind;
            if (in.kind == K::noteOn && in.velocity > 0)
                a.noteOn (offset, in.note, in.velocity, in.channel);
            else if (in.kind == K::noteOn || in.kind == K::noteOff)
                a.noteOff (offset, in.note, in.channel);
            else if (in.kind == K::sustain)
                a.sustainPedal (offset, in.down);
            else
                a.allNotesOff (offset);
            ++next;
        }
        a.endBlock();
        for (int i = 0; i < a.numEvents(); ++i)
        {
            const auto& e = a.event (i);
            if (e.kind == osp::Arpeggiator::Event::Kind::noteOn || e.kind == osp::Arpeggiator::Event::Kind::noteOff)
                out.push_back ({ start + e.offset, 0, e.kind == osp::Arpeggiator::Event::Kind::noteOn, e.note,
                                 e.kind == osp::Arpeggiator::Event::Kind::noteOn ? e.velocity : 0, 1 });
        }
    }
    return out;
}

std::vector<Note> laneOnly (const std::vector<Note>& notes, int lane)
{
    std::vector<Note> out;
    for (auto n : notes)
        if (n.lane == lane)
        {
            n.lane = 0;
            out.push_back (n);
        }
    return out;
}

std::string describe (const std::vector<Note>& notes, std::size_t max)
{
    std::ostringstream s;
    for (std::size_t i = 0; i < notes.size() && i < max; ++i)
        s << notes[i].sample << (notes[i].on ? "+" : "-") << notes[i].note << ' ';
    if (notes.size() > max)
        s << "... (" << notes.size() << ")";
    return s.str();
}

} // namespace andor_arp_test
