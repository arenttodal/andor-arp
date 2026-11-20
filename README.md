# ANDOR ARP

One musical input. Three independent performances. One DAW track.

ANDOR ARP plays three instruments from one chord: each performance (lane A, B, C) runs its own
arpeggio, pattern or captured gesture over the same keys, in time with the host. **Play more.
Program less.**

Status: **0.1.0-dev, research and feasibility** (`docs/STATUS.md`). Nothing here is released.

| Part | State |
|---|---|
| Shared ARP core (`andor_arp_core`, from ANDOR OSP) | extracted in OSP, pinned here as `osp/`; 18-style golden passes standalone |
| Three-lane coordinator, performance model, pattern codec | built and tested (headless) |
| Max for Live external `andor.arp.engine` | source written against the Max SDK; compiles (Linux type check); macOS build in CI; **not yet loaded in Live** |
| Master/Receiver link, persistence | protocol built and tested in simulation; **not yet tested in Live** |
| Ableton devices (.amxd), Rack (.adg), example Set | **not built** (need Max/Live on a Mac) |
| JUCE instrument host | **deferred** behind its feasibility gate (`docs/JUCE_IMPLEMENTATION.md`) |

## Build and test

```sh
git submodule update --init
cmake -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
./build/andor-arp-timing-report          # the scheduler model's timing table
```

The Max external also needs the Max SDK base:

```sh
git clone https://github.com/Cycling74/max-sdk-base.git
cmake -B build -G Ninja -DANDOR_ARP_BUILD_MAX_EXTERNAL=ON -DANDOR_ARP_MAX_SDK_DIR=$PWD/max-sdk-base
```

## Layout

```
osp/                  ANDOR OSP, pinned (andor_arp_core: the arpeggiator; never copied here)
shared/               the product's pure C++ layer: lanes, performance model, link protocol, state blob
max/external/         andor.arp.engine (Max SDK adapter) and MaxSchedulingAdapter
max/shared/           the devices' Live API pairing script
max/master|receiver/  device specifications (patchers to be built in Max)
juce/                 the instrument-host variant (deferred; plan only)
presets/              performance presets (.andorperformance)
tests/                Catch2 tests and the scheduler simulation
docs/                 architecture, implementation notes, test matrix, release runbook, reports
```

See `docs/ARCHITECTURE.md` first.
