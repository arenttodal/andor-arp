# ANDOR ARP — status

## 20 Nov 2026 — R0.1 feasibility (spec "ANDOR ARP — product spinoff specification R0.1")

Done (headless, tested):

- Repository created; ANDOR OSP pinned as `osp/` (branch `claude/andor-arp-spec-xy4umx`, which
  extracts `andor_arp_core`; source line `claude/custom-arp` e39b0bb).
- `shared/`: host contract, `ThreeLaneCoordinator`, `PerformanceModel` (`.andorperformance` 1),
  `LinkProtocol` (protocol 1), `StateBlob` (1), `TripleBuffer`/`SpscQueue`, `MidiByteParser`.
- `max/external/MaxSchedulingAdapter` and the scheduler model (`docs/results/timing-simulation.md`).
- `max/external/andor.arp.engine.cpp`: compiles against max-sdk-base (pinned in CI); never run in Max.
- `max/shared/andor-arp-pairing.js`: written; never run in Live.
- Three example performances (`presets/`).
- CI: Ubuntu + macOS (tests, timing report, unsigned universal .mxo artifact); weekly core-update PR.

Not done: everything in Live (devices, Rack, Set, timing measurement, persistence, pairing), the
Master's UI, Capture's preview/editor in Max, Windows, signing/notarisation, the JUCE host.

## NEXT

See `docs/reports/R0.1-feasibility-report.md` §12 (exact next tasks) and §13 (owner decisions).
