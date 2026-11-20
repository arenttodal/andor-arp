# ANDOR ARP Master (.amxd, MIDI effect) — device specification

Not built yet (needs Max on a Mac). Patcher outline:

```
[live.thisdevice] ──► [js andor-arp-pairing.js master] ──► (track <key>) ─┐
                                                                          ▼
[midiin] ──► [andor.arp.engine @role master] ──► [midiout]        (MIDI passes on untouched)
                 │ link out             ▲ link in
                 ▼                      │
           [send <channel>]       [receive <channel>]   (names set from the engine's "channel" status)
                 status ──► [route channel link capture version timing role] ──► UI
[pattr ---state @bindto engine @parameter_enable 1, type blob, stored only]   (saved with the Set)
[freebang] ──► (panic)        [live.thisdevice] middle outlet ──► (active $1)
UI (live.* objects / jsui): three strips → "set <lane> <param> <value>", "name <lane> <name>",
  CAPTURE → "capture start" / "capture stop <lane> rhythm|steps" / "capture cancel",
  pattern library → "loadpattern <lane> <path>"
```

Interface: see `docs/M4L_IMPLEMENTATION.md` §6.
