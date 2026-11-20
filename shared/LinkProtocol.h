#pragma once

#include "PerformanceModel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace andor::arp::link
{

/**
    The Master/Receiver link of the Max for Live product (docs/M4L_IMPLEMENTATION.md), as pure C++:
    the Max objects only carry these messages over Max's send/receive and call these classes, so
    pairing, acknowledgement and reconciliation are tested here, without Live.

    What travels: identity, lane assignment, lane state (settings + pattern document, as JSON),
    revisions, acknowledgements and health. Never note events: every Receiver generates its lane
    locally from the MIDI its chain receives.

    Scope. All messages of one ANDOR ARP Rack use one channel name derived from where the devices
    are (channelName): the Live track that holds the Master and whose Instrument Rack holds the
    Receivers. Two Racks on two tracks, a duplicated track included, therefore never hear each
    other. (Max's "---" prefix would make a name local to one device, which is the opposite of
    what a Master and its Receivers need.)

    Authority. While connected, the Master is authoritative for edits. When a Receiver connects,
    reconcile() decides by link identity and revision; a newer state is never overwritten by an
    older one of the same link, and a Receiver keeps playing its own saved state while no Master
    is there.
*/

inline constexpr int protocolVersion = 1;
inline constexpr const char* protocolName = "andor.arp";

/** The channel for a Rack, from its Live track's identity (the LOM id of the track holding the
    Master; the Receivers find the same track by walking up from their chain). Only characters a
    Max symbol carries safely. */
std::string channelName (const std::string& trackKey);

using Atom = std::variant<std::int64_t, double, std::string>;

enum class Type
{
    hello,      ///< Receiver -> Master: I am lane X, my stored link and revision
    state,      ///< Master -> Receiver: the lane's state at revision N (payload: lane JSON)
    ack,        ///< Receiver -> Master: applied revision N
    pull,       ///< Master -> Receiver: send me your state (the Master adopts it)
    upload,     ///< Receiver -> Master: my state (payload)
    ping,       ///< Master -> all: are you there (and: I am, with my link)
    pong        ///< Receiver -> Master
};

struct Message
{
    Type type = Type::hello;
    int lane = 0;
    std::string linkId;          ///< the Master's link identity (hello/upload: the Receiver's stored one)
    std::string senderId;        ///< the sending device instance (runtime only)
    std::uint64_t revision = 0;
    bool hasState = false;       ///< hello: the Receiver holds a saved lane state
    std::string payload;         ///< state / upload: laneToJson text

    bool operator== (const Message&) const = default;
};

std::vector<Atom> encode (const Message& message);
/** nullopt for another protocol, a newer protocol version or a malformed list. */
std::optional<Message> decode (const std::vector<Atom>& atoms, std::string* error = nullptr);

/** What a Master does when a Receiver says hello. */
enum class Reconcile
{
    inSync,          ///< same link, same revision: nothing travels
    pushToReceiver,  ///< the Master's state goes to the Receiver
    adoptReceiver    ///< the Receiver's state becomes the Master's (it is newer, or the Master has none)
};

struct LaneVersion
{
    std::string linkId;
    std::uint64_t revision = 0;
    bool hasState = false;
};
Reconcile reconcile (const LaneVersion& master, const LaneVersion& receiver) noexcept;

/** A lane's connection as the Master shows it. */
enum class LaneLink
{
    noReceiver,      ///< nothing has said hello (show: "add the Receiver to chain X")
    syncing,         ///< a state was sent, not yet acknowledged
    inSync,
    notResponding,   ///< sent several times without an acknowledgement
    duplicate        ///< more than one Receiver claims this lane in this Rack
};

/** The Master's side. Call receive() for each message on the channel, tick() regularly (e.g.
    every 250 ms), edit() when the musician changes a lane; send what they return. */
class MasterLink
{
public:
    static constexpr double retryMs = 400.0;
    static constexpr int maxRetries = 5;
    static constexpr double pingMs = 2000.0;
    static constexpr double silenceMs = 6500.0;   ///< no pong for this long: the Receiver is gone

    void begin (std::string linkId, std::string instanceId, const Performance& restored, bool restoredFromSession);
    const Performance& performance() const noexcept { return perf; }
    const std::string& linkId() const noexcept { return link; }

    std::vector<Message> receive (const Message& message, double nowMs);
    std::vector<Message> tick (double nowMs);
    /** The musician changed a lane: its revision rises and it goes to the lane's Receiver. */
    std::vector<Message> edit (Lane lane, const LaneState& state, double nowMs);

    LaneLink laneLink (Lane lane) const noexcept;
    /** True while the lane's state came from the musician or a saved session (not a default). */
    bool laneHasState (Lane lane) const noexcept { return hasState[static_cast<std::size_t> (laneIndex (lane))]; }

private:
    struct Peer
    {
        std::string id;
        double lastHeard = 0.0;
    };
    struct LaneLinkState
    {
        std::vector<Peer> peers;
        std::uint64_t ackedRevision = 0;
        bool awaitingAck = false, gaveUp = false;
        double sentAt = 0.0;
        int retries = 0;
    };
    Message stateMessage (int lane) const;
    void forgetSilent (double nowMs);

    std::string link, self;
    Performance perf;
    std::array<bool, laneCount> hasState {};
    std::array<LaneLinkState, laneCount> lanes {};
    double lastPing = -1.0e18;
};

/** A Receiver's side: it owns its lane's saved state and keeps playing it without a Master. */
class ReceiverLink
{
public:
    static constexpr double helloMs = 500.0;
    static constexpr double masterSilenceMs = 6500.0;   ///< no word from the Master this long: disconnected (it keeps playing)

    /** `restored`: the lane state saved with the Live Set (nullopt: a fresh device). */
    void begin (Lane lane, std::string instanceId, std::optional<LaneState> restored, std::string storedLinkId);
    std::vector<Message> receive (const Message& message, double nowMs);
    std::vector<Message> tick (double nowMs);

    Lane lane() const noexcept { return laneId; }
    bool connected() const noexcept { return masterHeard; }
    /** What the lane plays now (nullopt: nothing saved and no Master yet: the device stays silent). */
    const std::optional<LaneState>& state() const noexcept { return current; }
    const std::string& linkId() const noexcept { return link; }
    /** Raised whenever state() changed (the external re-reads and recompiles). */
    std::uint64_t changes() const noexcept { return changeCount; }

private:
    Message hello() const;
    Lane laneId = Lane::A;
    std::string self, link;
    std::optional<LaneState> current;
    bool masterHeard = false;
    double lastHello = -1.0e18, lastMaster = -1.0e18;
    std::uint64_t changeCount = 0;
};

} // namespace andor::arp::link
