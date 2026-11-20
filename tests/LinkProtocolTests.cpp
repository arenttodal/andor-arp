// Master/Receiver link: pairing per Rack, acknowledgement and retry, reconciliation by revision,
// Receivers that keep their patterns without a Master, duplicated Racks that stay apart.
// Simulated: a channel delivers every message to every device on it (as Max's send/receive does
// for one name), optionally dropping some.

#include "LinkProtocol.h"

#include "core/Prng.h"
#include "model/ArpFactoryPatterns.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>

using namespace andor::arp;
using namespace andor::arp::link;

namespace
{
    /** Every device of one Live Set; messages go only to devices on the same channel. */
    struct LiveSet
    {
        struct Device
        {
            std::string channel;
            MasterLink* master = nullptr;
            ReceiverLink* receiver = nullptr;
        };
        std::vector<Device> devices;
        double now = 0.0;
        osp::Prng rng { 3 };
        double dropRate = 0.0;
        int delivered = 0;

        void deliver (const std::string& channel, const std::vector<Message>& messages, std::vector<std::pair<std::string, Message>>& queue)
        {
            for (const auto& m : messages)
            {
                // Through the Max layer's encoding, as in Live.
                const auto decoded = decode (encode (m));
                REQUIRE (decoded.has_value());
                queue.push_back ({ channel, *decoded });
            }
        }

        void run (double ms, double step = 50.0)
        {
            for (double end = now + ms; now < end; now += step)
            {
                std::vector<std::pair<std::string, Message>> queue;
                for (auto& d : devices)
                    deliver (d.channel, d.master != nullptr ? d.master->tick (now) : d.receiver->tick (now), queue);
                for (int guard = 0; ! queue.empty() && guard < 50; ++guard)
                {
                    std::vector<std::pair<std::string, Message>> next;
                    for (const auto& [channel, m] : queue)
                    {
                        if (dropRate > 0.0 && rng.nextDouble() < dropRate)
                            continue;
                        for (auto& d : devices)
                            if (d.channel == channel)
                            {
                                ++delivered;
                                deliver (d.channel, d.master != nullptr ? d.master->receive (m, now) : d.receiver->receive (m, now), next);
                            }
                    }
                    queue = std::move (next);
                }
            }
        }
    };

    LaneState someLane (int which, std::uint64_t revision = 1)
    {
        LaneState l;
        l.name = "LANE" + std::to_string (which);
        l.settings.arp.pattern = static_cast<osp::ArpPattern> (which % osp::arp::patternCount);
        l.settings.seed = 100 + static_cast<std::uint64_t> (which);
        l.pattern = osp::arppattern::factoryPatterns()[static_cast<std::size_t> (which) % osp::arppattern::factoryPatterns().size()];
        l.revision = revision;
        return l;
    }

    bool samePlayable (const std::optional<LaneState>& a, const LaneState& b)
    {
        return a && a->settings == b.settings && a->pattern == b.pattern && a->name == b.name;
    }
}

TEST_CASE ("Link: messages survive Max's atoms and refuse foreign or newer ones", "[unit][link]")
{
    Message m;
    m.type = Type::state;
    m.lane = 2;
    m.linkId = "L1";
    m.senderId = "dev-9";
    m.revision = 123456789012ull;
    m.hasState = true;
    m.payload = R"({"name": "é"})";
    CHECK (decode (encode (m)) == m);
    auto atoms = encode (m);
    atoms[1] = std::int64_t { protocolVersion + 1 };
    std::string error;
    CHECK_FALSE (decode (atoms, &error).has_value());
    CHECK (error.find ("newer") != std::string::npos);
    atoms = encode (m);
    atoms[0] = std::string ("other");
    CHECK_FALSE (decode (atoms).has_value());
    atoms = encode (m);
    atoms[3] = std::int64_t { 7 };
    CHECK_FALSE (decode (atoms).has_value());
    atoms = encode (m);
    atoms[3] = 2.0;   // Max may hand integers over as floats
    CHECK (decode (atoms).has_value());
    CHECK (channelName ("12 34") == "andor.arp.v1.12_34");
}

TEST_CASE ("Link: reconciliation never replaces a newer state of the same link with an older one", "[unit][link]")
{
    CHECK (reconcile ({ "L", 5, true }, { "L", 5, true }) == Reconcile::inSync);
    CHECK (reconcile ({ "L", 5, true }, { "L", 4, true }) == Reconcile::pushToReceiver);
    CHECK (reconcile ({ "L", 5, true }, { "L", 6, true }) == Reconcile::adoptReceiver);
    CHECK (reconcile ({ "L", 5, true }, { "", 0, false }) == Reconcile::pushToReceiver);
    CHECK (reconcile ({ "L", 0, false }, { "M", 9, true }) == Reconcile::adoptReceiver);   // a new Master keeps the saved pattern
    CHECK (reconcile ({ "L", 5, true }, { "M", 9, true }) == Reconcile::pushToReceiver);   // another Rack's state: the Master decides
}

TEST_CASE ("Link: a new Rack pairs, every lane in sync, edits reach the right Receiver", "[unit][link]")
{
    MasterLink master;
    master.begin ("link-1", "m", defaultPerformance(), false);
    std::array<ReceiverLink, 3> rx;
    LiveSet set;
    set.devices.push_back ({ channelName ("track-7"), &master, nullptr });
    for (int i = 0; i < 3; ++i)
    {
        rx[static_cast<std::size_t> (i)].begin (laneAt (i), "r" + std::to_string (i), std::nullopt, "");
        set.devices.push_back ({ channelName ("track-7"), nullptr, &rx[static_cast<std::size_t> (i)] });
    }
    set.run (2000);
    for (int i = 0; i < 3; ++i)
    {
        INFO (i);
        CHECK (master.laneLink (laneAt (i)) == LaneLink::inSync);
        CHECK (samePlayable (rx[static_cast<std::size_t> (i)].state(), master.performance().lanes[static_cast<std::size_t> (i)]));
        CHECK (rx[static_cast<std::size_t> (i)].connected());
    }
    const auto before0 = rx[0].state();
    set.devices.front().master->edit (Lane::B, someLane (5), set.now);
    set.run (1000);
    CHECK (samePlayable (rx[1].state(), someLane (5)));
    CHECK (rx[1].state()->revision == master.performance().lane (Lane::B).revision);
    CHECK (rx[0].state() == before0);   // only lane B changed
    CHECK (master.laneLink (Lane::B) == LaneLink::inSync);
}

TEST_CASE ("Link: reopening a saved Set reconciles by revision without overwriting newer patterns", "[unit][link]")
{
    auto saved = defaultPerformance();
    for (int i = 0; i < 3; ++i)
        saved.lanes[static_cast<std::size_t> (i)] = someLane (i, 10);
    MasterLink master;
    master.begin ("link-1", "m", saved, true);
    std::array<ReceiverLink, 3> rx;
    rx[0].begin (Lane::A, "a", someLane (0, 10), "link-1");    // equal: nothing travels
    rx[1].begin (Lane::B, "b", someLane (8, 12), "link-1");    // newer than the Master's: adopted
    rx[2].begin (Lane::C, "c", someLane (9, 3), "link-1");     // older: replaced
    LiveSet set;
    set.devices.push_back ({ channelName ("t"), &master, nullptr });
    for (auto& r : rx)
        set.devices.push_back ({ channelName ("t"), nullptr, &r });
    set.run (3000);
    CHECK (samePlayable (rx[0].state(), someLane (0)));
    CHECK (samePlayable (rx[1].state(), someLane (8)));
    CHECK (master.performance().lane (Lane::B).settings == someLane (8).settings);
    CHECK (master.performance().lane (Lane::B).revision == 12);
    CHECK (samePlayable (rx[2].state(), someLane (2)));
    for (int i = 0; i < 3; ++i)
        CHECK (master.laneLink (laneAt (i)) == LaneLink::inSync);
}

TEST_CASE ("Link: Receivers keep playing their saved lanes without a Master, and a new Master adopts them", "[unit][link]")
{
    std::array<ReceiverLink, 3> rx;
    LiveSet set;
    for (int i = 0; i < 3; ++i)
    {
        rx[static_cast<std::size_t> (i)].begin (laneAt (i), "r" + std::to_string (i), someLane (i, 4), "old-link");
        set.devices.push_back ({ channelName ("t"), nullptr, &rx[static_cast<std::size_t> (i)] });
    }
    set.run (5000);
    for (int i = 0; i < 3; ++i)
    {
        CHECK_FALSE (rx[static_cast<std::size_t> (i)].connected());
        CHECK (samePlayable (rx[static_cast<std::size_t> (i)].state(), someLane (i)));
    }
    // The musician drops a fresh Master onto the track.
    MasterLink master;
    master.begin ("new-link", "m", defaultPerformance(), false);
    set.devices.push_back ({ channelName ("t"), &master, nullptr });
    set.run (3000);
    for (int i = 0; i < 3; ++i)
    {
        INFO (i);
        CHECK (samePlayable (rx[static_cast<std::size_t> (i)].state(), someLane (i)));
        CHECK (master.performance().lanes[static_cast<std::size_t> (i)].settings == someLane (i).settings);
        CHECK (rx[static_cast<std::size_t> (i)].linkId() == "new-link");
        CHECK (master.laneLink (laneAt (i)) == LaneLink::inSync);
    }
}

TEST_CASE ("Link: a duplicated Rack is a separate performance group", "[unit][link]")
{
    // Duplicating a track copies the devices with their saved state: the same link id, the same
    // revisions. The copies sit on another track, so on another channel.
    auto saved = defaultPerformance();
    MasterLink m1, m2;
    m1.begin ("link-1", "m1", saved, true);
    m2.begin ("link-1", "m2", saved, true);
    std::array<ReceiverLink, 3> r1, r2;
    LiveSet set;
    set.devices.push_back ({ channelName ("track-1"), &m1, nullptr });
    set.devices.push_back ({ channelName ("track-2"), &m2, nullptr });
    for (int i = 0; i < 3; ++i)
    {
        r1[static_cast<std::size_t> (i)].begin (laneAt (i), "a" + std::to_string (i), std::nullopt, "");
        r2[static_cast<std::size_t> (i)].begin (laneAt (i), "b" + std::to_string (i), std::nullopt, "");
        set.devices.push_back ({ channelName ("track-1"), nullptr, &r1[static_cast<std::size_t> (i)] });
        set.devices.push_back ({ channelName ("track-2"), nullptr, &r2[static_cast<std::size_t> (i)] });
    }
    set.run (2000);
    set.devices[0].master->edit (Lane::A, someLane (3), set.now);   // edit the original only
    set.run (1500);
    CHECK (samePlayable (r1[0].state(), someLane (3)));
    CHECK_FALSE (samePlayable (r2[0].state(), someLane (3)));
    CHECK (m1.laneLink (Lane::A) == LaneLink::inSync);
    CHECK (m2.laneLink (Lane::A) == LaneLink::inSync);
}

TEST_CASE ("Link: lost messages are retried; a missing, removed or duplicated Receiver shows", "[unit][link]")
{
    MasterLink master;
    master.begin ("L", "m", defaultPerformance(), true);
    std::array<ReceiverLink, 3> rx;
    LiveSet set;
    set.dropRate = 0.3;
    set.devices.push_back ({ channelName ("t"), &master, nullptr });
    for (int i = 0; i < 2; ++i)   // no Receiver for lane C
    {
        rx[static_cast<std::size_t> (i)].begin (laneAt (i), "r" + std::to_string (i), std::nullopt, "");
        set.devices.push_back ({ channelName ("t"), nullptr, &rx[static_cast<std::size_t> (i)] });
    }
    set.run (8000);
    CHECK (master.laneLink (Lane::A) == LaneLink::inSync);
    CHECK (master.laneLink (Lane::B) == LaneLink::inSync);
    CHECK (master.laneLink (Lane::C) == LaneLink::noReceiver);
    for (int k = 0; k < 10; ++k)
        set.devices.front().master->edit (Lane::A, someLane (k), set.now + k);
    set.run (6000);
    CHECK (samePlayable (rx[0].state(), someLane (9)));   // the last edit wins, despite the losses

    // Lane B's Receiver is deleted: the Master notices it has gone.
    set.dropRate = 0.0;
    set.devices.erase (set.devices.begin() + 2);
    set.run (10000);
    CHECK (master.laneLink (Lane::B) == LaneLink::noReceiver);

    // Two Receivers claim lane A (a chain duplicated inside the Rack).
    ReceiverLink extra;
    extra.begin (Lane::A, "dup", std::nullopt, "");
    set.devices.push_back ({ channelName ("t"), nullptr, &extra });
    set.run (2000);
    CHECK (master.laneLink (Lane::A) == LaneLink::duplicate);
    CHECK (samePlayable (extra.state(), someLane (9)));   // both still play the lane's state
}
