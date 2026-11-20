// andor-arp-pairing.js — finds which ANDOR ARP Rack this device belongs to (Live Object Model),
// for [js andor-arp-pairing.js] in the Master and in each Receiver. Low priority, never timing
// critical: it runs once when [live.thisdevice] reports the device ready, and again on "bang".
//
// The Rack's key is "<track id>-<rack id>":
//   - a Receiver: the Instrument Rack (InstrumentGroupDevice) that contains its chain, and that
//     rack's track;
//   - the Master: the first Instrument Rack after it in its own track's device list.
// So two Racks never share a key: not on two tracks (a duplicated track gets new ids), and not
// two Racks on one track. The key is sent to andor.arp.engine as "track <key>", which turns it
// into the link channel name. Ids are runtime ids: valid for this session, found again at the
// next load; nothing here is saved.
//
// Unpaired (a Master with no Instrument Rack after it, a Receiver outside a Rack): "track
// unpaired-<device id>", a channel nobody else uses, and "pairing <reason>" on the status outlet.

autowatch = 1;
inlets = 1;
outlets = 2;   // 0: "track <key>" to andor.arp.engine, 1: "pairing ok|<reason>"

var role = jsarguments.length > 1 ? jsarguments[1] : "receiver";

function idOf(api) { return parseInt(api.id, 10); }

function parentOf(api) {
    var p = new LiveAPI(null, api.unquotedpath);
    p.goto("canonical_parent");
    return p;
}

function findUp(api, type) {
    var cur = new LiveAPI(null, api.unquotedpath);
    for (var depth = 0; depth < 16 && idOf(cur) !== 0; depth++) {
        cur.goto("canonical_parent");
        if (cur.type === type)
            return cur;
    }
    return null;
}

function bang() {
    var me = new LiveAPI(null, "this_device");
    if (idOf(me) === 0) {
        outlet(1, "pairing", "not-ready");
        return;
    }
    var track = findUp(me, "Track");
    if (track === null) {
        outlet(0, "track", "unpaired-" + idOf(me));
        outlet(1, "pairing", "no-track");
        return;
    }
    var rackId = 0;
    if (role === "master") {
        // The first Instrument Rack after the Master in the track's device list.
        var devices = track.get("devices");   // ["id", n, "id", m, ...]
        var seenMe = false;
        for (var i = 1; i < devices.length; i += 2) {
            var id = devices[i];
            if (id === idOf(me)) { seenMe = true; continue; }
            if (!seenMe) continue;
            var d = new LiveAPI(null, "id " + id);
            if (d.get("class_name") == "InstrumentGroupDevice") { rackId = id; break; }
        }
    } else {
        var rack = findUp(me, "RackDevice");
        if (rack !== null && rack.get("class_name") == "InstrumentGroupDevice")
            rackId = idOf(rack);
    }
    if (rackId === 0) {
        outlet(0, "track", "unpaired-" + idOf(me));
        outlet(1, "pairing", role === "master" ? "no-rack-after-master" : "not-in-instrument-rack");
        return;
    }
    outlet(0, "track", idOf(track) + "-" + rackId);
    outlet(1, "pairing", "ok");
}
