# ANDOR ARP Receiver (.amxd, MIDI effect, one per Rack chain) — device specification

Not built yet (needs Max on a Mac). One device file; the lane (A/B/C) is a stored device parameter
(`live.tab`), sent to the engine as `lane <0|1|2>` at load and whenever it changes.

```
[live.thisdevice] ──► [js andor-arp-pairing.js receiver] ──► (track <key>)
[midiin] ──► [andor.arp.engine @role receiver]  ◄── [live.tab A B C] → (lane $1) ──► [midiout]
                 link ⇄ [send]/[receive] <channel>        status ──► lane letter + status light
[pattr ---state @bindto engine @parameter_enable 1, type blob, stored only]
[freebang] ──► (panic)        device on/off ──► (active $1)
```

The Receiver plays its saved lane without a Master. It is kept minimised in the Rack.
