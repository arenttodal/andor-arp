// The external's small real-time helpers: raw MIDI byte parsing and the lock-free hand-overs.

#include "MidiBytes.h"
#include "TripleBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace andor::arp;

namespace
{
    std::vector<MidiByteParser::Message> parse (std::initializer_list<int> bytes)
    {
        MidiByteParser p;
        std::vector<MidiByteParser::Message> out;
        for (int b : bytes)
        {
            MidiByteParser::Message m;
            if (p.feed (b, m))
                out.push_back (m);
        }
        return out;
    }
}

TEST_CASE ("MIDI bytes: notes, running status, real-time bytes inside a message, SysEx passed on", "[unit][glue]")
{
    // note on, running-status note on, a clock byte inside a message, velocity-0 note off
    const auto m = parse ({ 0x90, 60, 100, 64, 90, 0x90, 67, 0xF8, 80, 0x80, 60, 0 });
    REQUIRE (m.size() == 5);
    CHECK ((m[0].status == 0x90 && m[0].data1 == 60 && m[0].data2 == 100 && m[0].length == 3));
    CHECK ((m[1].status == 0x90 && m[1].data1 == 64 && m[1].data2 == 90));
    CHECK ((m[2].status == 0xF8 && m[2].length == 1));
    CHECK ((m[3].status == 0x90 && m[3].data1 == 67 && m[3].data2 == 80));
    CHECK ((m[4].status == 0x80 && m[4].data1 == 60));

    const auto c = parse ({ 0xB0, 64, 127, 0xC1, 5, 0xE0, 0, 64, 0xB0, 123, 0 });
    REQUIRE (c.size() == 4);
    CHECK ((c[0].status == 0xB0 && c[0].data1 == 64 && c[0].data2 == 127));
    CHECK ((c[1].status == 0xC1 && c[1].length == 2 && c[1].data1 == 5));
    CHECK ((c[2].status == 0xE0 && c[2].length == 3));
    CHECK ((c[3].data1 == 123));

    const auto s = parse ({ 0xF0, 0x7E, 0x01, 0xF7, 0x90, 1, 2 });
    REQUIRE (s.size() == 5);
    for (int i = 0; i < 4; ++i)
        CHECK (s[static_cast<std::size_t> (i)].sysexByte);
    CHECK ((s[4].status == 0x90 && ! s[4].sysexByte));
    CHECK (parse ({ 60, 100 }).empty());   // stray data bytes without a status
}

TEST_CASE ("Triple buffer: the reader always gets the latest whole value, across threads", "[unit][glue]")
{
    struct Value
    {
        int a = 0, b = 0;   // always written equal: a torn read would differ
    };
    TripleBuffer<Value> t;
    Value v;
    CHECK_FALSE (t.read (v));
    t.write ({ 1, 1 });
    t.write ({ 2, 2 });
    REQUIRE (t.read (v));
    CHECK (v.a == 2);
    CHECK_FALSE (t.read (v));

    std::atomic<bool> done { false };
    std::thread writer ([&] {
        for (int i = 3; i < 200000; ++i)
            t.write ({ i, i });
        done = true;
    });
    int last = 2, torn = 0, backwards = 0;
    while (! done)
        if (t.read (v))
        {
            torn += v.a != v.b;
            backwards += v.a < last;
            last = v.a;
        }
    writer.join();
    if (t.read (v))
        last = v.a;
    CHECK (torn == 0);
    CHECK (backwards == 0);
    CHECK (last == 199999);
}

TEST_CASE ("SPSC queue: order kept, full refused", "[unit][glue]")
{
    SpscQueue<int, 4> q;
    CHECK (q.push (1));
    CHECK (q.push (2));
    CHECK (q.push (3));
    CHECK_FALSE (q.push (4));   // capacity - 1 items
    int x = 0;
    REQUIRE (q.pop (x));
    CHECK (x == 1);
    CHECK (q.push (4));
    for (int want : { 2, 3, 4 })
    {
        REQUIRE (q.pop (x));
        CHECK (x == want);
    }
    CHECK_FALSE (q.pop (x));
}
