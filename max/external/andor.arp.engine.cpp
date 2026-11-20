// andor.arp.engine: ANDOR ARP's Max for Live external (docs/M4L_IMPLEMENTATION.md).
//
// A host adapter only. The music is andor_arp_core (OSP's arpeggiator) driven by the shared
// ThreeLaneCoordinator; time comes through MaxSchedulingAdapter; the Master/Receiver link is
// shared/LinkProtocol; saved state is shared/StateBlob. All of those are pure C++ with their own
// tests (tests/); this file only connects them to Max.
//
//   [midiin] -> [andor.arp.engine @role receiver @lane 1] -> [midiout]
//
// Inlet: raw MIDI bytes (ints) and messages. Outlets, left to right:
//   0  raw MIDI bytes for [midiout] (the lane's notes; every other MIDI message passed on as is)
//   1  link messages for the Rack's [send] ("link ...")
//   2  status ("channel <name>", "lane <n> <state>", "timing ...", "version ...", "error ...")
//
// Messages:
//   track <key>          the Live track identity found by the device's pairing script (LOM id);
//                        sets the Rack's channel (reported on outlet 2 for [send]/[receive])
//   link <atoms...>      a message from the Rack's [receive]
//   lane <0|1|2>         Receiver: which lane this device plays (its stored tab)
//   panic                every owned note ends now ([freebang], device off, before unload)
//   active <0|1>         the device's on/off ([live.thisdevice]): off releases everything
//   dump                 build, link and timing diagnostics on outlet 2
//   Master only:
//   set <lane> <param> <value>   edit a lane: enabled pattern rate gate swing octaves velocity
//                                custom strum strumdir strumsync strumms transpose low high
//                                channel passthrough seed
//   name <lane> <symbol>          a lane's musical name
//   loadpattern <lane> <path>     a `.andorpattern` file becomes the lane's custom pattern
//   capture start | capture stop <lane> [rhythm|steps] | capture cancel
//   loadperformance <path> / saveperformance <path>
//
// Threads. In Live, Max's scheduler runs in the audio thread ("Scheduler in Audio Interrupt").
// MIDI input, the service clock and output happen there: no allocation, lock, file, JSON or
// symbol creation. Edits, link messages, files and saved state run on Max's main thread
// (defer_low / qelem) and reach the scheduler through lock-free triple buffers.

#include "ext.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_itm.h"
#include "ext_obex.h"

#include "BuildInfo.h"
#include "LinkProtocol.h"
#include "MaxSchedulingAdapter.h"
#include "MidiBytes.h"
#include "PerformanceModel.h"
#include "StateBlob.h"
#include "ThreeLaneCoordinator.h"
#include "TripleBuffer.h"

#include "engine/PatternCapture.h"
#include "model/ArpCoreVersion.h"
#include "model/ArpPatternCodec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace andor::arp;

namespace
{
    constexpr double virtualRate = 48000.0;   // the core's clock; it need not be Live's sample rate
    constexpr double serviceMs = 1.0;         // asked for; in Live the scheduler serves once per vector

    /** One lane's playback data, handed to the scheduler thread whole. */
    struct LanePacket
    {
        LaneSettings settings {};
        osp::arp::CustomPattern pattern {};
        bool hasPattern = false;
        bool active = false;   ///< this device plays the lane
    };

    struct PendingNote
    {
        double wallMs = 0.0;
        std::uint8_t status = 0, note = 0, velocity = 0;
    };

    std::uint64_t splitMix (std::uint64_t x) noexcept
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    /** An identity, not a musical choice (rule 2 is about the music, which stays seeded). */
    std::string newIdentity (const void* object)
    {
        const auto t = static_cast<std::uint64_t> (systimer_gettime() * 1000.0);
        const auto v = splitMix (t ^ splitMix (reinterpret_cast<std::uintptr_t> (object)));
        char text[24];
        std::snprintf (text, sizeof text, "%016llx", static_cast<unsigned long long> (v));
        return text;
    }

    std::string readFile (const char* path, std::string& error)
    {
        std::ifstream in (path, std::ios::binary);
        if (! in)
        {
            error = std::string ("cannot open ") + path;
            return {};
        }
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }
}

/** Everything C++ of one object (created on the main thread with the object). */
struct Engine
{
    // -- scheduler thread ----------------------------------------------------------------
    ThreeLaneCoordinator coordinator;
    MaxSchedulingAdapter adapter;
    MidiByteParser parser;
    std::array<ThreeLaneCoordinator::Input, 256> inputs {};
    int inputCount = 0, inputsDropped = 0;
    std::array<PendingNote, 8192> pending {};
    int pendingCount = 0, pendingDropped = 0;
    std::array<TripleBuffer<LanePacket>, laneCount> packets;
    bool released = false;
    osp::PatternCapture capture;   // Master: the keys as played (pushed here, polled on the main thread)
    double lastServiceMs = -1.0;

    // -- shared, lock-free ---------------------------------------------------------------
    std::atomic<bool> panicRequested { false }, activeFlag { true };
    std::atomic<int> laneRequest { -1 };   ///< a lane change, applied by the scheduler thread
    std::atomic<double> timingErrorQuarters { 0.0 };
    std::atomic<long long> jumps { 0 };
    std::atomic<int> dropped { 0 };

    // -- main thread ---------------------------------------------------------------------
    bool master = false;
    Lane lane = Lane::A;
    std::string instanceId, channel;
    link::MasterLink masterLink;
    link::ReceiverLink receiverLink;
    std::uint64_t publishedChanges = 0;
    t_dictionary* payloadDict = nullptr;
    t_symbol* payloadName = nullptr;
};

struct t_arp_engine
{
    t_object ob;
    void* midiOut;
    void* linkOut;
    void* statusOut;
    t_clock* service;
    t_clock* output;
    t_qelem* linkTick;
    t_clock* linkClock;
    t_itm* itm;
    t_symbol* role;
    long lane;
    double latency;
    long stopReleases;
    Engine* engine;
};

static t_class* engineClass = nullptr;

// ---------------------------------------------------------------------------------------------
// Scheduler thread
// ---------------------------------------------------------------------------------------------

static void sendMidi (t_arp_engine* x, std::uint8_t status, std::uint8_t d1, std::uint8_t d2)
{
    outlet_int (x->midiOut, status);
    outlet_int (x->midiOut, d1);
    outlet_int (x->midiOut, d2);
}

static void flushDue (t_arp_engine* x, double nowMs)
{
    auto& e = *x->engine;
    // Due notes in time order (the array is small: a few ms of notes).
    std::sort (e.pending.begin(), e.pending.begin() + e.pendingCount, [] (const PendingNote& a, const PendingNote& b) {
        return a.wallMs < b.wallMs || (a.wallMs == b.wallMs && (a.status & 0xF0) == 0x80 && (b.status & 0xF0) == 0x90);
    });
    int due = 0;
    while (due < e.pendingCount && e.pending[static_cast<std::size_t> (due)].wallMs <= nowMs + 1.0e-6)
    {
        const auto& p = e.pending[static_cast<std::size_t> (due)];
        sendMidi (x, p.status, p.note, p.velocity);
        ++due;
    }
    if (due > 0)
    {
        std::move (e.pending.begin() + due, e.pending.begin() + e.pendingCount, e.pending.begin());
        e.pendingCount -= due;
    }
    if (e.pendingCount > 0)
        clock_fdelay (x->output, std::max (0.0, e.pending[0].wallMs - nowMs));
}

static void queueOutputs (t_arp_engine* x, const MaxSchedulingAdapter::Block* block, double nowMs)
{
    auto& e = *x->engine;
    auto& c = e.coordinator;
    for (int i = 0; i < c.numOutputs(); ++i)
    {
        const auto& o = c.output (i);
        if (e.pendingCount >= static_cast<int> (e.pending.size()))
        {
            ++e.pendingDropped;
            continue;
        }
        const auto status = static_cast<std::uint8_t> ((o.on ? 0x90 : 0x80) | ((o.channel - 1) & 0x0F));
        const double when = block != nullptr ? e.adapter.eventWallMs (*block, o.offset) : nowMs;
        e.pending[static_cast<std::size_t> (e.pendingCount++)] = { when, status, o.note, o.on ? o.velocity : std::uint8_t { 0 } };
    }
}

static void releaseNow (t_arp_engine* x, double nowMs)
{
    auto& e = *x->engine;
    // Notes waiting to start are dropped; notes waiting to end end now with the rest.
    int kept = 0;
    for (int i = 0; i < e.pendingCount; ++i)
        if ((e.pending[static_cast<std::size_t> (i)].status & 0xF0) == 0x80)
            e.pending[static_cast<std::size_t> (kept++)] = { nowMs, e.pending[static_cast<std::size_t> (i)].status, e.pending[static_cast<std::size_t> (i)].note, 0 };
    e.pendingCount = kept;
    e.coordinator.releaseAll (0);
    queueOutputs (x, nullptr, nowMs);
    flushDue (x, nowMs);
}

static void engineService (t_arp_engine* x)
{
    auto& e = *x->engine;
    double now = 0.0;
    clock_getftime (&now);
    clock_fdelay (x->service, serviceMs);

    if (e.master)
    {
        // The Master plays nothing: it only feeds Capture's clock.
        if (e.lastServiceMs >= 0.0 && e.capture.armed())
            e.capture.advance (static_cast<int> ((now - e.lastServiceMs) * virtualRate / 1000.0));
        e.lastServiceMs = now;
        return;
    }

    if (const int want = e.laneRequest.exchange (-1); want >= 0)
        e.coordinator.setActiveLanes (1u << want);   // the old lane's notes end in the next block
    for (int l = 0; l < laneCount; ++l)
    {
        LanePacket p;
        if (e.packets[static_cast<std::size_t> (l)].read (p))
        {
            e.coordinator.setLaneSettings (laneAt (l), p.settings);
            e.coordinator.setCustomPattern (laneAt (l), p.hasPattern ? &p.pattern : nullptr);
        }
    }

    const bool active = e.activeFlag.load (std::memory_order_acquire);
    if (e.panicRequested.exchange (false) || (! active && ! e.released))
    {
        releaseNow (x, now);
        e.inputCount = 0;
        e.released = ! active;
        if (! active)
            return;
    }
    if (! active)
        return;
    e.released = false;

    MaxSchedulingAdapter::TransportReading r;
    if (x->itm != nullptr)
    {
        long num = 4, den = 4;
        itm_gettimesignature (x->itm, &num, &den);
        r.valid = true;
        r.playing = itm_getstate (x->itm) != 0;
        r.bpm = itm_gettempo (x->itm);
        r.ppq = itm_getticks (x->itm) / std::max (1.0, itm_getresolution());
        r.numerator = static_cast<int> (num);
        r.denominator = static_cast<int> (den);
    }
    const auto block = e.adapter.service (now, r);
    if (block.numSamples > 0)
    {
        if (x->stopReleases != 0 && ! block.timing.playing && e.coordinator.status (e.lane).running && r.valid)
            e.coordinator.releaseAll (0);
        e.coordinator.process (block.timing, block.numSamples, e.inputs.data(), e.inputCount);
        e.inputCount = 0;
        queueOutputs (x, &block, now);
        e.timingErrorQuarters.store (e.adapter.lastErrorQuarters(), std::memory_order_relaxed);
        e.jumps.store (e.adapter.jumps(), std::memory_order_relaxed);
        e.dropped.store (e.inputsDropped + e.pendingDropped + e.coordinator.droppedOutputs(), std::memory_order_relaxed);
    }
    flushDue (x, now);
}

static void engineOutput (t_arp_engine* x)
{
    double now = 0.0;
    clock_getftime (&now);
    flushDue (x, now);
}

static void engineInt (t_arp_engine* x, long byte)
{
    auto& e = *x->engine;
    MidiByteParser::Message m;
    if (! e.parser.feed (static_cast<int> (byte), m))
        return;
    const int type = m.status & 0xF0;
    const int channel = (m.status & 0x0F) + 1;
    const bool note = m.length == 3 && (type == 0x90 || type == 0x80);
    const bool pedal = m.length == 3 && type == 0xB0 && m.data1 == 64;
    const bool allOff = m.length == 3 && type == 0xB0 && (m.data1 == 123 || m.data1 == 120);

    if (e.master)
    {
        // The Master never consumes MIDI: everything goes on to the Rack (and Capture listens).
        if (note && e.capture.armed())
            e.capture.push (0, m.data1, type == 0x90 ? m.data2 : 0, x->itm != nullptr ? itm_gettempo (x->itm) : 120.0);
        outlet_int (x->midiOut, m.status);
        if (m.length > 1)
            outlet_int (x->midiOut, m.data1);
        if (m.length > 2)
            outlet_int (x->midiOut, m.data2);
        return;
    }
    if (! (note || pedal || allOff) || ! e.activeFlag.load (std::memory_order_relaxed))
    {
        // Not the lane's business (or the device is off): passed on untouched.
        outlet_int (x->midiOut, m.status);
        if (m.length > 1)
            outlet_int (x->midiOut, m.data1);
        if (m.length > 2)
            outlet_int (x->midiOut, m.data2);
        if (allOff)
            e.panicRequested.store (true);
        return;
    }
    if (e.inputCount >= static_cast<int> (e.inputs.size()))
    {
        ++e.inputsDropped;
        return;
    }
    // Applied at the next block's first sample: no later than it happened (MaxSchedulingAdapter).
    ThreeLaneCoordinator::Input in;
    if (note)
        in = type == 0x90 ? ThreeLaneCoordinator::Input::on (0, m.data1, m.data2, channel) : ThreeLaneCoordinator::Input::off (0, m.data1, channel);
    else if (pedal)
        in = ThreeLaneCoordinator::Input::pedal (0, m.data2 >= 64);
    else
        in = ThreeLaneCoordinator::Input::panic (0);
    e.inputs[static_cast<std::size_t> (e.inputCount++)] = in;
}

// ---------------------------------------------------------------------------------------------
// Main thread
// ---------------------------------------------------------------------------------------------

static void status (t_arp_engine* x, const char* selector, std::initializer_list<t_atom> atoms)
{
    outlet_anything (x->statusOut, gensym (selector), static_cast<short> (atoms.size()), const_cast<t_atom*> (atoms.begin()));
}

static t_atom symAtom (const std::string& s)
{
    t_atom a;
    atom_setsym (&a, gensym (s.c_str()));
    return a;
}

static t_atom longAtom (long long v)
{
    t_atom a;
    atom_setlong (&a, static_cast<t_atom_long> (v));
    return a;
}

static t_atom floatAtom (double v)
{
    t_atom a;
    atom_setfloat (&a, v);
    return a;
}

/** Hands a lane's state to the scheduler thread (compiled; no allocation there). */
static void publishLane (t_arp_engine* x, Lane lane, const LaneState* state)
{
    auto& e = *x->engine;
    LanePacket p;
    p.active = state != nullptr;
    if (state != nullptr)
    {
        p.settings = state->settings;
        if (state->pattern && state->pattern->pattern.valid())
        {
            p.pattern = osp::arppattern::compile (state->pattern->pattern);
            p.hasPattern = p.pattern.valid();
        }
    }
    else
        p.settings.enabled = false;
    e.packets[static_cast<std::size_t> (laneIndex (lane))].write (p);
}

static void publishReceiverState (t_arp_engine* x)
{
    auto& e = *x->engine;
    if (e.master || e.receiverLink.changes() == e.publishedChanges)
        return;
    e.publishedChanges = e.receiverLink.changes();
    const auto& s = e.receiverLink.state();
    publishLane (x, e.lane, s ? &*s : nullptr);
    status (x, "lane", { longAtom (laneIndex (e.lane)), symAtom (s ? "playing" : "empty") });
}

/** Large payloads travel in a registered dictionary (never as a symbol: symbols live forever). */
static std::vector<link::Atom> encodeForMax (t_arp_engine* x, link::Message m)
{
    auto& e = *x->engine;
    if (! m.payload.empty())
    {
        if (e.payloadDict == nullptr)
        {
            e.payloadDict = dictionary_new();
            t_symbol* name = gensym (("andor.arp.payload." + e.instanceId).c_str());
            e.payloadDict = dictobj_register (e.payloadDict, &name);
            e.payloadName = name;
        }
        const std::string key = "lane" + std::to_string (m.lane) + "." + std::to_string (m.revision);
        dictionary_appendstring (e.payloadDict, gensym ("latest"), m.payload.c_str());
        m.payload = "dict:" + std::string (e.payloadName->s_name);
        (void) key;
    }
    return link::encode (m);
}

static void sendLink (t_arp_engine* x, const std::vector<link::Message>& messages)
{
    for (const auto& m : messages)
    {
        const auto atoms = encodeForMax (x, m);
        std::vector<t_atom> out (atoms.size());
        for (std::size_t i = 0; i < atoms.size(); ++i)
        {
            if (const auto* v = std::get_if<std::int64_t> (&atoms[i]))
                atom_setlong (&out[i], static_cast<t_atom_long> (*v));
            else if (const auto* d = std::get_if<double> (&atoms[i]))
                atom_setfloat (&out[i], *d);
            else
                atom_setsym (&out[i], gensym (std::get<std::string> (atoms[i]).c_str()));
        }
        outlet_anything (x->linkOut, gensym ("link"), static_cast<short> (out.size()), out.data());
    }
}

static void handleLink (t_arp_engine* x, t_symbol*, long argc, t_atom* argv)
{
    auto& e = *x->engine;
    std::vector<link::Atom> atoms;
    for (long i = 0; i < argc; ++i)
    {
        if (atom_gettype (argv + i) == A_LONG)
            atoms.emplace_back (static_cast<std::int64_t> (atom_getlong (argv + i)));
        else if (atom_gettype (argv + i) == A_FLOAT)
            atoms.emplace_back (atom_getfloat (argv + i));
        else
            atoms.emplace_back (std::string (atom_getsym (argv + i)->s_name));
    }
    auto m = link::decode (atoms);
    if (! m)
        return;
    if (m->payload.rfind ("dict:", 0) == 0)
    {
        t_symbol* name = gensym (m->payload.c_str() + 5);
        m->payload.clear();
        if (t_dictionary* d = dictobj_findregistered_retain (name))
        {
            const char* text = nullptr;
            if (dictionary_getstring (d, gensym ("latest"), &text) == MAX_ERR_NONE && text != nullptr)
                m->payload = text;
            dictobj_release (d);
        }
    }
    double now = 0.0;
    clock_getftime (&now);
    if (e.master)
        sendLink (x, e.masterLink.receive (*m, now));
    else
    {
        sendLink (x, e.receiverLink.receive (*m, now));
        publishReceiverState (x);
    }
}

static void linkWork (t_arp_engine* x)
{
    auto& e = *x->engine;
    double now = 0.0;
    clock_getftime (&now);
    if (e.master)
    {
        sendLink (x, e.masterLink.tick (now));
        for (int l = 0; l < laneCount; ++l)
        {
            static const char* names[] = { "none", "syncing", "ok", "silent", "duplicate" };
            status (x, "link", { longAtom (l), symAtom (names[static_cast<int> (e.masterLink.laneLink (laneAt (l)))]) });
        }
    }
    else
    {
        sendLink (x, e.receiverLink.tick (now));
        publishReceiverState (x);
        status (x, "connected", { longAtom (e.receiverLink.connected() ? 1 : 0) });
    }
}

static void linkClockFired (t_arp_engine* x)
{
    qelem_set (x->linkTick);   // the link's work runs on the main thread
    clock_fdelay (x->linkClock, 250.0);
}

static void engineEdit (t_arp_engine* x, Lane lane, const LaneState& state)
{
    auto& e = *x->engine;
    double now = 0.0;
    clock_getftime (&now);
    sendLink (x, e.masterLink.edit (lane, state, now));
}

static void engineSet (t_arp_engine* x, t_symbol*, long argc, t_atom* argv)
{
    auto& e = *x->engine;
    if (! e.master || argc < 3)
        return;
    const auto lane = laneAt (static_cast<int> (atom_getlong (argv)));
    auto state = e.masterLink.performance().lane (lane);
    auto& s = state.settings;
    const std::string param = atom_getsym (argv + 1)->s_name;
    const double v = atom_getfloat (argv + 2);
    const int i = static_cast<int> (atom_getlong (argv + 2));
    if (param == "enabled") s.enabled = i != 0;
    else if (param == "pattern") s.arp.pattern = static_cast<osp::ArpPattern> (i);
    else if (param == "rate") s.arp.rate = static_cast<osp::ArpRate> (i);
    else if (param == "gate") s.arp.gate = v;
    else if (param == "swing") s.arp.swing = v;
    else if (param == "octaves") s.arp.octaves = i;
    else if (param == "velocity") s.arp.velocityScale = v;
    else if (param == "custom") s.arp.custom = i != 0;
    else if (param == "strum") s.arp.strum = i != 0;
    else if (param == "strumdir") s.arp.strumDirection = static_cast<osp::StrumDirection> (i);
    else if (param == "strumsync") s.arp.strumSync = i != 0;
    else if (param == "strumms") s.arp.strumMs = v;
    else if (param == "transpose") s.transpose = i;
    else if (param == "low") s.registerLow = i;
    else if (param == "high") s.registerHigh = i;
    else if (param == "channel") s.outputChannel = i;
    else if (param == "passthrough") s.passThrough = i != 0;
    else if (param == "seed") s.seed = static_cast<std::uint64_t> (std::max (1, i));
    else
    {
        object_error (&x->ob, "unknown lane parameter %s", param.c_str());
        return;
    }
    s = s.sanitised();
    engineEdit (x, lane, state);
}

static void engineName (t_arp_engine* x, t_symbol*, long argc, t_atom* argv)
{
    auto& e = *x->engine;
    if (! e.master || argc < 2)
        return;
    const auto lane = laneAt (static_cast<int> (atom_getlong (argv)));
    auto state = e.masterLink.performance().lane (lane);
    state.name = atom_getsym (argv + 1)->s_name;
    engineEdit (x, lane, state);
}

static void loadPatternDeferred (t_arp_engine* x, t_symbol*, long argc, t_atom* argv)
{
    auto& e = *x->engine;
    if (! e.master || argc < 2)
        return;
    const auto lane = laneAt (static_cast<int> (atom_getlong (argv)));
    char path[MAX_PATH_CHARS];
    path_nameconform (atom_getsym (argv + 1)->s_name, path, PATH_STYLE_NATIVE, PATH_TYPE_BOOT);
    std::string error;
    const auto text = readFile (path, error);
    auto doc = error.empty() ? osp::arppattern::documentFromText (text, error) : std::nullopt;
    if (! doc)
    {
        status (x, "error", { symAtom ("pattern"), symAtom (error) });
        return;
    }
    auto state = e.masterLink.performance().lane (lane);
    state.pattern = std::move (*doc);
    state.settings.arp.custom = true;
    engineEdit (x, lane, state);
}

static void engineLoadPattern (t_arp_engine* x, t_symbol* s, long argc, t_atom* argv)
{
    defer_low (x, (method) loadPatternDeferred, s, static_cast<short> (argc), argv);
}

static void captureDeferred (t_arp_engine* x, t_symbol*, long argc, t_atom* argv)
{
    auto& e = *x->engine;
    if (! e.master || argc < 1)
        return;
    const std::string what = atom_getsym (argv)->s_name;
    if (what == "start")
    {
        e.capture.start (virtualRate);
        status (x, "capture", { symAtom ("recording") });
    }
    else if (what == "cancel")
    {
        e.capture.cancel();
        status (x, "capture", { symAtom ("cancelled") });
    }
    else if (what == "stop" && argc >= 2)
    {
        e.capture.stop();
        e.capture.poll();
        const auto lane = laneAt (static_cast<int> (atom_getlong (argv + 1)));
        osp::arppattern::Interpretation how;
        if (argc >= 3 && std::string (atom_getsym (argv + 2)->s_name) == "steps")
            how.timing = osp::arppattern::Interpretation::Timing::steps;
        osp::arppattern::InterpretReport report;
        osp::arppattern::Document doc;
        doc.id = "capture." + newIdentity (x);
        doc.name = "Captured";
        doc.capture = e.capture.notes();
        doc.captureBpm = e.capture.bpm();
        doc.interpretation = how;
        doc.pattern = osp::arppattern::interpret (doc.capture, how, &report);
        if (! doc.pattern.valid())
        {
            status (x, "capture", { symAtom ("empty") });
            return;
        }
        auto state = e.masterLink.performance().lane (lane);
        state.pattern = std::move (doc);
        state.settings.arp.custom = true;
        engineEdit (x, lane, state);
        status (x, "capture", { symAtom ("applied"), longAtom (report.placedNotes), longAtom (report.chordNotesDropped + report.beyondCapacity) });
    }
}

static void engineCapture (t_arp_engine* x, t_symbol* s, long argc, t_atom* argv)
{
    defer_low (x, (method) captureDeferred, s, static_cast<short> (argc), argv);
}

static void engineTrack (t_arp_engine* x, t_symbol*, long argc, t_atom* argv)
{
    auto& e = *x->engine;
    std::string key;
    for (long i = 0; i < argc; ++i)
    {
        if (atom_gettype (argv + i) == A_LONG)
            key += std::to_string (atom_getlong (argv + i));
        else if (atom_gettype (argv + i) == A_SYM)
            key += atom_getsym (argv + i)->s_name;
    }
    e.channel = link::channelName (key);
    status (x, "channel", { symAtom (e.channel) });
}

static void engineLinkMessage (t_arp_engine* x, t_symbol* s, long argc, t_atom* argv)
{
    // From [receive], possibly on the scheduler thread: the work (JSON) is done on the main thread.
    defer_low (x, (method) handleLink, s, static_cast<short> (argc), argv);
}

/** Receiver: the device's stored lane tab changed. The old lane's state is let go; the Master
    sends the new lane's (or, without a Master, the Receiver is silent until one appears). */
static void engineLane (t_arp_engine* x, long lane)
{
    auto& e = *x->engine;
    const auto next = laneAt (static_cast<int> (lane));
    if (e.master || next == e.lane)
        return;
    publishLane (x, e.lane, nullptr);
    e.lane = next;
    x->lane = laneIndex (next);
    e.receiverLink.begin (next, e.instanceId, std::nullopt, e.receiverLink.linkId());
    e.publishedChanges = 0;
    e.laneRequest.store (laneIndex (next));
    publishReceiverState (x);
}

static void enginePanic (t_arp_engine* x)
{
    x->engine->panicRequested.store (true);
}

static void engineActive (t_arp_engine* x, long on)
{
    x->engine->activeFlag.store (on != 0);
}

static void engineDump (t_arp_engine* x)
{
    auto& e = *x->engine;
    status (x, "version", { symAtom (build::productVersion), symAtom (osp::arp::coreVersion), symAtom (build::ospCoreCommit) });
    status (x, "timing", { floatAtom (e.timingErrorQuarters.load()), longAtom (e.jumps.load()), longAtom (e.dropped.load()) });
    status (x, "role", { symAtom (e.master ? "master" : "receiver"), longAtom (laneIndex (e.lane)), symAtom (e.channel) });
}

// -- saved state (the pattr getvalueof / setvalueof interface) --------------------------------

static std::string stateText (t_arp_engine* x)
{
    auto& e = *x->engine;
    auto root = osp::arppattern::JsonValue::makeObject();
    using J = osp::arppattern::JsonValue;
    root.set ("schemaVersion", J::makeInt (1));
    if (e.master)
    {
        root.set ("kind", J::makeString ("andor.arp-master-state"));
        root.set ("link", J::makeString (e.masterLink.linkId()));
        root.set ("performance", J::makeString (performanceToText (e.masterLink.performance())));
    }
    else
    {
        root.set ("kind", J::makeString ("andor.arp-receiver-state"));
        root.set ("lane", J::makeInt (laneIndex (e.lane)));
        root.set ("link", J::makeString (e.receiverLink.linkId()));
        if (const auto& s = e.receiverLink.state())
            root.set ("state", laneToJson (*s));
    }
    return osp::arppattern::writeJson (root);
}

static t_max_err engineGetValue (t_arp_engine* x, long* ac, t_atom** av)
{
    const auto words = blob::encode (stateText (x));
    char alloc = 0;
    if (atom_alloc_array (static_cast<long> (words.size()), ac, av, &alloc) != MAX_ERR_NONE)
        return MAX_ERR_OUT_OF_MEM;
    for (std::size_t i = 0; i < words.size(); ++i)
        atom_setlong (*av + i, words[i]);
    return MAX_ERR_NONE;
}

static t_max_err engineSetValue (t_arp_engine* x, long ac, t_atom* av)
{
    auto& e = *x->engine;
    std::vector<std::int32_t> words (static_cast<std::size_t> (std::max (0L, ac)));
    for (long i = 0; i < ac; ++i)
        words[static_cast<std::size_t> (i)] = static_cast<std::int32_t> (atom_getlong (av + i));
    std::string error;
    const auto text = blob::decode (words, &error);
    const auto root = text ? osp::arppattern::parseJson (*text, error) : std::nullopt;
    if (! root)
    {
        object_warn (&x->ob, "saved state not restored: %s", error.c_str());
        return MAX_ERR_NONE;   // the device keeps its defaults (and says so)
    }
    const auto& r = *root;
    if (e.master)
    {
        auto perf = performanceFromText (r["performance"].text, error);
        if (! perf)
        {
            object_warn (&x->ob, "saved performance not restored: %s", error.c_str());
            return MAX_ERR_NONE;
        }
        e.masterLink.begin (r["link"].isString() && ! r["link"].text.empty() ? r["link"].text : newIdentity (x), e.instanceId, *perf, true);
    }
    else
    {
        std::optional<LaneState> state;
        if (r["state"].isObject())
            state = laneFromJson (r["state"], error);
        e.receiverLink.begin (e.lane, e.instanceId, state, r["link"].isString() ? r["link"].text : std::string());
        publishReceiverState (x);
    }
    return MAX_ERR_NONE;
}

// ---------------------------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------------------------

static void* engineNew (t_symbol*, long argc, t_atom* argv)
{
    auto* x = static_cast<t_arp_engine*> (object_alloc (engineClass));
    if (x == nullptr)
        return nullptr;
    x->role = gensym ("receiver");
    x->lane = 0;
    x->latency = 4.0;
    x->stopReleases = 0;
    attr_args_process (x, static_cast<short> (argc), argv);

    x->statusOut = outlet_new (x, nullptr);
    x->linkOut = outlet_new (x, nullptr);
    x->midiOut = outlet_new (x, "int");

    x->engine = new Engine();
    auto& e = *x->engine;
    e.master = x->role == gensym ("master");
    e.lane = laneAt (static_cast<int> (x->lane));
    e.instanceId = newIdentity (x);
    MaxSchedulingAdapter::Config cfg;
    cfg.sampleRate = virtualRate;
    cfg.latencyMs = x->latency;
    e.adapter.configure (cfg);
    e.coordinator.prepare (virtualRate);
    e.coordinator.setActiveLanes (e.master ? 0u : 1u << laneIndex (e.lane));
    if (e.master)
        e.masterLink.begin (newIdentity (x), e.instanceId, defaultPerformance(), false);
    else
        e.receiverLink.begin (e.lane, e.instanceId, std::nullopt, std::string());

    x->itm = static_cast<t_itm*> (itm_getglobal());
    if (x->itm != nullptr)
        itm_reference (x->itm);
    x->service = clock_new (x, (method) engineService);
    x->output = clock_new (x, (method) engineOutput);
    x->linkTick = qelem_new (x, (method) linkWork);
    x->linkClock = clock_new (x, (method) linkClockFired);
    clock_fdelay (x->service, serviceMs);
    clock_fdelay (x->linkClock, 100.0);
    return x;
}

static void engineFree (t_arp_engine* x)
{
    // Unload: notes still owned end first ([freebang] -> panic is the reliable path; this is the
    // last chance). The scheduler's clocks stop first, so nothing else touches the engine.
    clock_unset (x->service);
    clock_unset (x->output);
    clock_unset (x->linkClock);
    if (x->engine != nullptr && ! x->engine->master)
    {
        double now = 0.0;
        clock_getftime (&now);
        releaseNow (x, now);
    }
    object_free (x->service);
    object_free (x->output);
    object_free (x->linkClock);
    qelem_free (x->linkTick);
    if (x->itm != nullptr)
        itm_dereference (x->itm);
    if (x->engine != nullptr && x->engine->payloadDict != nullptr)
    {
        dictobj_unregister (x->engine->payloadDict);
        object_free (x->engine->payloadDict);
    }
    delete x->engine;
}

static void engineAssist (t_arp_engine*, void*, long io, long index, char* text)
{
    if (io == ASSIST_INLET)
        std::snprintf (text, 256, "MIDI bytes from [midiin]; messages");
    else
    {
        static const char* outlets[] = { "MIDI bytes to [midiout]", "link messages to the Rack's [send]", "status" };
        std::snprintf (text, 256, "%s", outlets[std::clamp<long> (index, 0, 2)]);
    }
}

extern "C" C74_EXPORT void ext_main (void*)
{
    t_class* c = class_new ("andor.arp.engine", (method) engineNew, (method) engineFree, sizeof (t_arp_engine), nullptr, A_GIMME, 0);
    class_addmethod (c, (method) engineInt, "int", A_LONG, 0);
    class_addmethod (c, (method) engineLinkMessage, "link", A_GIMME, 0);
    class_addmethod (c, (method) engineTrack, "track", A_GIMME, 0);
    class_addmethod (c, (method) engineSet, "set", A_GIMME, 0);
    class_addmethod (c, (method) engineName, "name", A_GIMME, 0);
    class_addmethod (c, (method) engineLoadPattern, "loadpattern", A_GIMME, 0);
    class_addmethod (c, (method) engineCapture, "capture", A_GIMME, 0);
    class_addmethod (c, (method) enginePanic, "panic", 0);
    class_addmethod (c, (method) engineLane, "lane", A_LONG, 0);
    class_addmethod (c, (method) engineActive, "active", A_LONG, 0);
    class_addmethod (c, (method) engineDump, "dump", 0);
    class_addmethod (c, (method) engineAssist, "assist", A_CANT, 0);
    class_addmethod (c, (method) engineGetValue, "getvalueof", A_CANT, 0);
    class_addmethod (c, (method) engineSetValue, "setvalueof", A_CANT, 0);

    CLASS_ATTR_SYM (c, "role", ATTR_SET_OPAQUE_USER, t_arp_engine, role);
    CLASS_ATTR_ENUM (c, "role", 0, "receiver master");
    CLASS_ATTR_LABEL (c, "role", 0, "Role (set at creation)");
    CLASS_ATTR_LONG (c, "lane", ATTR_SET_OPAQUE_USER, t_arp_engine, lane);
    CLASS_ATTR_FILTER_CLIP (c, "lane", 0, 2);
    CLASS_ATTR_LABEL (c, "lane", 0, "Lane 0 A, 1 B, 2 C (set at creation)");
    CLASS_ATTR_DOUBLE (c, "latency", ATTR_SET_OPAQUE_USER, t_arp_engine, latency);
    CLASS_ATTR_FILTER_CLIP (c, "latency", 1.0, 50.0);
    CLASS_ATTR_LONG (c, "stopreleases", 0, t_arp_engine, stopReleases);
    CLASS_ATTR_STYLE_LABEL (c, "stopreleases", 0, "onoff", "Release held notes when the transport stops");

    class_register (CLASS_BOX, c);
    engineClass = c;
    post ("andor.arp.engine %s (core %s, OSP %s)", build::productVersion, osp::arp::coreVersion, build::ospCoreCommit);
}
