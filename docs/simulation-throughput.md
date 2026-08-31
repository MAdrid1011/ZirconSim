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
