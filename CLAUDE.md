# CLAUDE.md — working rules for ANDOR ARP

ANDOR ARP is a product built on ANDOR OSP's arpeggiator (`osp/`, pinned). OSP's rules on
real-time safety, determinism and versioned data apply here too.

- **No second arpeggiator.** Musical behaviour lives in `andor_arp_core` (OSP). A musical change is
  made in OSP, tested there (and in ANDOR ANALOG), then reaches this repository through a
  dependency update. Never copy OSP sources into this repository.
- **One track.** Nothing may require extra MIDI tracks, loopback drivers or manual channel routing.
- **Real time.** `ThreeLaneCoordinator::process`, `MaxSchedulingAdapter::service` and the external's
  scheduler-thread paths never allocate, lock, touch files, parse JSON or create Max symbols.
- **Measured, not assumed.** Do not describe an architecture as working in a host until it has been
  run there; record the host, version and result in `docs/TEST_MATRIX.md`.
- **Stored data** carries a `schemaVersion` and a `kind`; newer versions are refused, never misread.
  Enum values are OSP's stored indices (append-only).
- Keep `ctest` green on every commit; update `docs/STATUS.md` with each piece of work.
- C++20, JUCE-style formatting as in OSP.
