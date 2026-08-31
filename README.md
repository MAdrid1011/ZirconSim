# ZirconSim

`zircon-2026` is the deterministic Verilator and differential-test harness for
Zircon-2026. It replaces wall-clock randomness, raw-binary-only loading, and
illegal-instruction termination with explicit seeds, ELF32 loading, symbol
resolution, and architectural `tohost` completion.

```sh
make unit
make smoke
```

`make unit` checks the deterministic PRNG and loads the software submodule's
RV32 ELF, including `PT_LOAD`, entry point, and `tohost/fromhost` symbols.
`make smoke` generates the trace-enabled parent RTL, builds `VZirconCore`, and
runs a bounded deterministic ELF/AXI/retire-trace harness. The resulting
`build/smoke-retire.jsonl` contains commit events in architectural order. A
timeout is only successful when `--allow-timeout` is explicit; it is a harness
smoke result, never an ELF pass result.

The runner requires explicit `--elf`, `--retire-trace`, and non-zero `--seed`.
Its AXI model loads ELF `PT_LOAD` data, drives legal deterministic backpressure,
holds each R/B response until handshake, tracks IDs/beats/last, and observes
`tohost` writes. It returns success only for `tohost = 1`; M1 has no LSU yet,
so a real tohost store is expected to remain blocked until M3 rather than being
misreported as a pass.

The old handwritten partial RV32IM interpreter remains in branch history and
is not a reference model. Commit-level comparison will use Spike, with Sail as
the nightly and dispute oracle.

`make diff SPIKE=/path/to/spike` runs two M1 RV32I/Zicsr commit-prefix smokes.
It compares 17 dataflow/CSR/control retirements and 32 ALU/branch retirements
against bounded Spike commit logs, then accepts only the expected M1 timeout at
each following `tohost` store. See [`docs/spike-differential.md`](docs/spike-differential.md)
for the exact comparison fields and current scope.
