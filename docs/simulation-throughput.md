# Deterministic Full-Core Simulation Throughput

## Contract

`make throughput` runs `tests/sim-throughput.S` for a fixed number of core
cycles using explicit AXI seed 1. The ELF defines `tohost` through the shared
linker script but deliberately never writes it, so the required
`--allow-timeout` is a controlled benchmark termination rather than an ELF
pass. The runner retains every architectural `RetireEvent` in JSONL; waveform
instrumentation is not enabled for this target.

```bash
/usr/bin/time -p make throughput THROUGHPUT_CYCLES=100000
wc -l build/sim-throughput.jsonl
sha256sum build/sim-throughput.elf
```

The status JSON reports cycles, seed, and retired events. Divide the retired
count by the measured wall time for retirements per second. A result is valid
only when the ELF hash, seed, RTL commit, ZirconSim commit, Verilator version,
and command are recorded with it.

## Build Modes

Normal `rtl` uses RTL that retains mandatory retire ports, but Verilator is not
built with `--trace`; this keeps full architectural trace correctness without
the waveform model cost. `trace-rtl` is a separate `--trace` model. Only that
binary accepts `--wave`, and `make trace-smoke` exercises that path.

Both modes share the same generated RTL and deterministic AXI slave. If a
failure requires a waveform, reproduce the same ELF, seed, and cycle limit on
the trace binary. A throughput optimization may not remove, sample, filter, or
reorder retire records, or change AXI ready/valid, response, or error behavior.

## Observed Local Baseline

The following warm-model observation was recorded on the development host after
building the normal non-waveform binary:

| Field | Value |
| --- | --- |
| Command | `/usr/bin/time -p make throughput THROUGHPUT_CYCLES=1000000` |
| Wall time | 4.72 s |
| Retired events | 464,333 |
| Retirements/s | 98,375 |
| Cycles / seed | 1,000,000 / 1 |
| ELF SHA-256 | `3300d5796eaa7f2ca7d998a17e018d6d0e1f67941c3508c2129ffe2746322cc1` |
| Core RTL commit | `2eedc78f731a28c100919e1fd2d61df807126cfd` |
| ZirconSim commit | `830c844` |
| Verilator | `5.029 devel rev v5.028-222-g469eca7de` |
| Waveform instrumentation | disabled |

The corresponding cold normal-model build completed in about 13 seconds and
produced 13 C++ translation units / 8.892 MB, versus 16 / 11.620 MB for the
separate waveform model. Both are below the five-minute component budget. This
is a local M3 baseline, not an M5 workload IPC or release-performance claim.
