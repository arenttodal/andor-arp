#include "LinkProtocol.h"

#include <algorithm>

namespace andor::arp::link
{

std::string channelName (const std::string& trackKey)
{
    std::string key;
    for (const char c : trackKey)
        key += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ? c : '_';
    return std::string (protocolName) + ".v" + std::to_string (protocolVersion) + "." + (key.empty() ? "unpaired" : key);
}

namespace
{
    const char* typeName (Type t)
    {
        switch (t)
        {
            case Type::hello:  return "hello";
            case Type::state:  return "state";
            case Type::ack:    return "ack";
            case Type::pull:   return "pull";
            case Type::upload: return "upload";
            case Type::ping:   return "ping";
            case Type::pong:   return "pong";
        }
        return "?";
    }

    std::optional<Type> typeFrom (const std::string& s)
    {
        for (auto t : { Type::hello, Type::state, Type::ack, Type::pull, Type::upload, Type::ping, Type::pong })
            if (s == typeName (t))
                return t;
        return std::nullopt;
    }

    const std::string* str (const Atom& a) { return std::get_if<std::string> (&a); }
    std::optional<std::int64_t> integer (const Atom& a)
    {
        if (const auto* i = std::get_if<std::int64_t> (&a))
            return *i;
        if (const auto* d = std::get_if<double> (&a); d != nullptr && *d == static_cast<double> (static_cast<std::int64_t> (*d)))
            return static_cast<std::int64_t> (*d);
        return std::nullopt;
    }
}

std::vector<Atom> encode (const Message& m)
{
    return { std::string (protocolName), std::int64_t { protocolVersion }, std::string (typeName (m.type)), std::int64_t { m.lane },
             m.linkId, m.senderId, static_cast<std::int64_t> (m.revision), std::int64_t { m.hasState ? 1 : 0 }, m.payload };
}

std::optional<Message> decode (const std::vector<Atom>& a, std::string* error)
{
    auto fail = [error] (const char* what) -> std::optional<Message> {
        if (error != nullptr)
            *error = what;
        return std::nullopt;
    };
    if (a.size() < 9 || str (a[0]) == nullptr || *str (a[0]) != protocolName)
        return fail ("not an ANDOR ARP link message");
    const auto version = integer (a[1]);
    if (! version || *version < 1)
        return fail ("no protocol version");
    if (*version > protocolVersion)
        return fail ("a newer link protocol (update this device)");
    Message m;
    const auto* type = str (a[2]);
    const auto t = type != nullptr ? typeFrom (*type) : std::nullopt;
    const auto lane = integer (a[3]);
    const auto revision = integer (a[6]);
    const auto hasState = integer (a[7]);
    if (! t || ! lane || *lane < -1 || *lane >= laneCount || str (a[4]) == nullptr || str (a[5]) == nullptr || ! revision || *revision < 0 || ! hasState
        || str (a[8]) == nullptr)
        return fail ("a malformed link message");
    m.type = *t;
    m.lane = static_cast<int> (*lane);
    m.linkId = *str (a[4]);
    m.senderId = *str (a[5]);
    m.revision = static_cast<std::uint64_t> (*revision);
    m.hasState = *hasState != 0;
    m.payload = *str (a[8]);
    return m;
}

Reconcile reconcile (const LaneVersion& master, const LaneVersion& receiver) noexcept
{
    if (! receiver.hasState)
        return Reconcile::pushToReceiver;
    if (! master.hasState)
        return Reconcile::adoptReceiver;   // a new or emptied Master: the Receiver's saved pattern is kept
    if (receiver.linkId != master.linkId)
        return Reconcile::pushToReceiver;  // another Rack's state (or none of this link's): the Master decides
    if (receiver.revision == master.revision)
        return Reconcile::inSync;
    return receiver.revision > master.revision ? Reconcile::adoptReceiver : Reconcile::pushToReceiver;
}

// ---------------------------------------------------------------------------------------------
// Master
// ---------------------------------------------------------------------------------------------

void MasterLink::begin (std::string linkId, std::string instanceId, const Performance& restored, bool restoredFromSession)
{
    link = std::move (linkId);
    self = std::move (instanceId);
    perf = restored;
    hasState.fill (restoredFromSession);
    lanes = {};
    lastPing = -1.0e18;
}

Message MasterLink::stateMessage (int lane) const
{
    Message m;
    m.type = Type::state;
    m.lane = lane;
    m.linkId = link;
    m.senderId = self;
    const auto& l = perf.lanes[static_cast<std::size_t> (lane)];
    m.revision = l.revision;
    m.hasState = true;
    m.payload = osp::arppattern::writeJson (laneToJson (l));
    return m;
}

std::vector<Message> MasterLink::receive (const Message& msg, double nowMs)
{
    std::vector<Message> out;
    if (msg.lane < 0 || msg.lane >= laneCount || msg.senderId == self)
        return out;
    const auto li = static_cast<std::size_t> (msg.lane);
    auto& ls = lanes[li];
    auto& lane = perf.lanes[li];

    auto peer = std::find_if (ls.peers.begin(), ls.peers.end(), [&] (const Peer& p) { return p.id == msg.senderId; });
    if (peer == ls.peers.end())
    {
        ls.peers.push_back ({ msg.senderId, nowMs });
        peer = ls.peers.end() - 1;
    }
    peer->lastHeard = nowMs;

    Message reply;
    reply.lane = msg.lane;
    reply.linkId = link;
    reply.senderId = self;
    switch (msg.type)
    {
        case Type::hello:
            switch (reconcile ({ link, lane.revision, hasState[li] }, { msg.linkId, msg.revision, msg.hasState }))
            {
                case Reconcile::inSync:
                    ls.ackedRevision = lane.revision;
                    ls.awaitingAck = ls.gaveUp = false;
                    reply.type = Type::ping;   // "I am here, and you are in sync"
                    reply.revision = lane.revision;
                    out.push_back (reply);
                    break;
                case Reconcile::pushToReceiver:
                    if (! hasState[li])
                    {
                        hasState[li] = true;   // the Master's default becomes this link's first revision
                        lane.revision = std::max<std::uint64_t> (lane.revision, 1);
                    }
                    out.push_back (stateMessage (msg.lane));
                    ls.awaitingAck = true;
                    ls.gaveUp = false;
                    ls.retries = 0;
                    ls.sentAt = nowMs;
                    break;
                case Reconcile::adoptReceiver:
                    reply.type = Type::pull;
                    out.push_back (reply);
                    break;
            }
            break;
        case Type::upload:
        {
            std::string error;
            if (const auto parsed = osp::arppattern::parseJson (msg.payload, error))
                if (auto state = laneFromJson (*parsed, error))
                {
                    lane = std::move (*state);
                    lane.revision = msg.revision;
                    hasState[li] = true;
                    ls.ackedRevision = msg.revision;
                    ls.awaitingAck = ls.gaveUp = false;
                    reply.type = Type::ping;
                    reply.revision = msg.revision;
                    out.push_back (reply);
                }
            break;
        }
        case Type::ack:
            if (msg.revision == lane.revision && msg.linkId == link)
            {
                ls.ackedRevision = msg.revision;
                ls.awaitingAck = ls.gaveUp = false;
            }
            break;
        case Type::pong:
        case Type::state:
        case Type::pull:
        case Type::ping:
            break;
    }
    return out;
}

void MasterLink::forgetSilent (double nowMs)
{
    for (auto& ls : lanes)
        ls.peers.erase (std::remove_if (ls.peers.begin(), ls.peers.end(), [nowMs] (const Peer& p) { return nowMs - p.lastHeard > silenceMs; }),
                        ls.peers.end());
}

std::vector<Message> MasterLink::tick (double nowMs)
{
    std::vector<Message> out;
    forgetSilent (nowMs);
    for (int i = 0; i < laneCount; ++i)
    {
        auto& ls = lanes[static_cast<std::size_t> (i)];
        if (! ls.awaitingAck || ls.gaveUp || nowMs - ls.sentAt < retryMs)
            continue;
        if (ls.retries >= maxRetries)
        {
            ls.gaveUp = true;
            continue;
        }
        ++ls.retries;
        ls.sentAt = nowMs;
        out.push_back (stateMessage (i));
    }
    if (nowMs - lastPing >= pingMs)
    {
        lastPing = nowMs;
        Message ping;
        ping.type = Type::ping;
        ping.lane = -1;
        ping.linkId = link;
        ping.senderId = self;
        out.push_back (ping);
    }
    return out;
}

std::vector<Message> MasterLink::edit (Lane lane, const LaneState& state, double nowMs)
{
    const auto li = static_cast<std::size_t> (laneIndex (lane));
    auto& l = perf.lanes[li];
    const auto revision = l.revision + 1;
    l = state;
    l.revision = revision;
    hasState[li] = true;
    auto& ls = lanes[li];
    ls.awaitingAck = true;
    ls.gaveUp = false;
    ls.retries = 0;
    ls.sentAt = nowMs;
    return { stateMessage (laneIndex (lane)) };
}

LaneLink MasterLink::laneLink (Lane lane) const noexcept
{
    const auto& ls = lanes[static_cast<std::size_t> (laneIndex (lane))];
    if (ls.peers.empty())
        return LaneLink::noReceiver;
    if (ls.peers.size() > 1)
        return LaneLink::duplicate;
    if (ls.gaveUp)
        return LaneLink::notResponding;
    if (ls.awaitingAck)
        return LaneLink::syncing;
    return LaneLink::inSync;
}

// ---------------------------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------------------------

void ReceiverLink::begin (Lane lane, std::string instanceId, std::optional<LaneState> restored, std::string storedLinkId)
{
    laneId = lane;
    self = std::move (instanceId);
    current = std::move (restored);
    link = std::move (storedLinkId);
    masterHeard = false;
    lastHello = -1.0e18;
    ++changeCount;
}

Message ReceiverLink::hello() const
{
    Message m;
    m.type = Type::hello;
    m.lane = laneIndex (laneId);
    m.linkId = link;
    m.senderId = self;
    m.revision = current ? current->revision : 0;
    m.hasState = current.has_value();
    return m;
}

std::vector<Message> ReceiverLink::receive (const Message& msg, double nowMs)
{
    std::vector<Message> out;
    const int mine = laneIndex (laneId);
    if (msg.senderId == self || (msg.lane != mine && msg.lane != -1))
        return out;
    if (msg.type == Type::state || msg.type == Type::pull || msg.type == Type::ping)
        lastMaster = nowMs;

    Message reply;
    reply.lane = mine;
    reply.senderId = self;
    switch (msg.type)
    {
        case Type::state:
        {
            if (msg.lane != mine)
                break;
            std::string error;
            const auto parsed = osp::arppattern::parseJson (msg.payload, error);
            auto state = parsed ? laneFromJson (*parsed, error) : std::nullopt;
            if (! state)
                break;   // unreadable (a newer format): keep playing what we have; no ack, so the Master shows it
            state->revision = msg.revision;
            current = std::move (*state);
            link = msg.linkId;
            masterHeard = true;
            ++changeCount;
            reply.type = Type::ack;
            reply.linkId = link;
            reply.revision = msg.revision;
            out.push_back (reply);
            break;
        }
        case Type::pull:
            if (msg.lane != mine || ! current)
                break;
            masterHeard = true;
            link = msg.linkId;   // the Master adopts this state under its link
            reply.type = Type::upload;
            reply.linkId = link;
            reply.revision = current->revision;
            reply.hasState = true;
            reply.payload = osp::arppattern::writeJson (laneToJson (*current));
            out.push_back (reply);
            break;
        case Type::ping:
            if (! masterHeard && msg.lane == -1)
            {
                // A Master appeared (or came back) without having heard us: introduce ourselves.
                out.push_back (hello());
                break;
            }
            masterHeard = true;
            if (msg.lane == mine)
                link = msg.linkId;   // in sync (or adopted) under this link
            reply.type = Type::pong;
            reply.linkId = link;
            reply.revision = current ? current->revision : 0;
            out.push_back (reply);
            break;
        case Type::hello:
        case Type::ack:
        case Type::upload:
        case Type::pong:
            break;
    }
    return out;
}

std::vector<Message> ReceiverLink::tick (double nowMs)
{
    if (masterHeard && nowMs - lastMaster > masterSilenceMs)
        masterHeard = false;   // the Master is gone (deleted, or its device off): keep playing, look for it
    if (masterHeard || nowMs - lastHello < helloMs)
        return {};
    lastHello = nowMs;
    return { hello() };
}

} // namespace andor::arp::link
