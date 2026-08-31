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
is not a reference model. Commit-level comparison uses Spike, with the locked
Sail RISC-V model providing an independently parsed RV32M prefix check.

`make diff SPIKE=/path/to/spike` runs two M1 RV32I/Zicsr and one M2 RV32IM
commit-prefix smoke. It compares 17 dataflow/CSR/control retirements, 32
ALU/branch retirements, and 17 RV32M retirements against bounded Spike commit
logs, then accepts only the expected timeout at each following `tohost` store.
See [`docs/spike-differential.md`](docs/spike-differential.md) for the exact
comparison fields and current scope.

`make diff-sail-rv32m SAIL=/path/to/sail_riscv_sim` runs the same explicit
seed-1, 17-retirement RV32M prefix against Sail-RISC-V commit
`beaf44991eee362a062fcaaf6fcb78ca428ff710`, which is locked in the parent
`toolchain.lock.json`. It is bounded reference-model evidence for normal
integer retirement only, not an ELF pass, full Sail suite, or M3 memory
differential. The reproducible model build and comparison command are recorded
in [`docs/spike-differential.md`](docs/spike-differential.md).
