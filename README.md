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
`tohost` writes. It returns success only after a normal `RetireEvent` records
the matching store and `tohost = 1`; an accepted AXI W beat alone is not an ELF
pass. `make tohost-rv32m` runs the explicit seed-1 M3 store completion smoke.

The old handwritten partial RV32IM interpreter remains in branch history and
is not a reference model. Commit-level comparison uses Spike, with the locked
Sail RISC-V model providing an independently parsed RV32M prefix check.

`make diff SPIKE=/path/to/spike` runs two M1 RV32I/Zicsr and one M2 RV32IM
commit-prefix smoke. It compares 17 dataflow/CSR/control retirements, 32
ALU/branch retirements, and 17 RV32M retirements against bounded Spike commit
logs. Each run must subsequently retire its `tohost` store before the harness
reports ELF completion.
See [`docs/spike-differential.md`](docs/spike-differential.md) for the exact
comparison fields and current scope.

`make diff-sail-rv32m SAIL=/path/to/sail_riscv_sim` runs the same explicit
seed-1, 17-retirement RV32M prefix against Sail-RISC-V commit
`beaf44991eee362a062fcaaf6fcb78ca428ff710`, which is locked in the parent
`toolchain.lock.json`. It is bounded reference-model evidence for normal
integer retirement only, not an ELF pass, full Sail suite, or M3 memory
differential. The reproducible model build and comparison command are recorded
in [`docs/spike-differential.md`](docs/spike-differential.md).

`make micro-ipc-rv32m` measures the M2 RV32M prefix through its 17th observed
retirement. `make baseline-ipc-rv32m BASELINE_2024=/path/to/clean/Zircon-2024`
runs the same prefix and deterministic AXI slave through the immutable 2024
Verilator model. These are early fixed-prefix IPC measurements only; their
scope and required baseline checkout are defined in
[`docs/ipc-microbenchmark.md`](docs/ipc-microbenchmark.md).
