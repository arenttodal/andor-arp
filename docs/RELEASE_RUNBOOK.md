# ANDOR ARP — release runbook (draft)

Nothing has been released. This is the intended procedure for the first $19 release (Ableton
first); it is to be completed when the Live gates pass.

## Identity

- Product version: `VERSION` (semantic; `0.1.0-dev` now). Tags: `andor-arp-v<version>` (never OSP's
  tags).
- Core: the OSP commit in `osp/` (recorded in every build, `andor.arp.engine` posts it on load and in
  `dump`), plus `osp::arp::coreVersion`.
- Bundle ids: `com.andor.arp.engine` (external). The JUCE plugin, if approved, gets its own ids
  (`docs/JUCE_IMPLEMENTATION.md` §5). Never OSP's.

## Ableton package

1. CI green on the release commit (core golden, codec fixtures, lanes, link, timing model, macOS
   external build).
2. Build `andor.arp.engine.mxo` universal (arm64 + x86_64) on macOS.
3. Sign with the existing Developer ID Application identity (hardened runtime, timestamp), then
   notarise and staple — OSP's `packaging/release` scripts are the model (inside-out signing, fail
   closed). Windows `.mxe64`: when that platform is added.
4. In Max: open the Master and Receiver patchers, include the signed external, **Freeze** each
   device, save `ANDOR ARP Master.amxd`, `ANDOR ARP Receiver.amxd`.
5. In Live: build the Rack with three Receivers and stock instruments, save
   `ANDOR ARP - 3 Instruments.adg`; save the example Set; include the three performances.
6. On a clean Mac (no Max, a fresh Live user library): install, open the example Set, play — the
   M4L-D1 check.
7. Package (zip or Live Pack), release notes, compatibility list (`docs/TEST_MATRIX.md`), known
   limitations.

## Installation (for testers, draft)

1. Unzip. Drag `ANDOR ARP - 3 Instruments.adg` onto a MIDI track (or copy the folder into Live's User
   Library).
2. Play. Replace any of the three instruments with your own (Kontakt, Serum, SWAM…): drop it into the
   chain after the ANDOR ARP Receiver.
3. Work only in the ANDOR ARP Master. No MIDI routing, extra tracks or channels are needed.

No account, licence server or internet connection is required for musical use in V1.
