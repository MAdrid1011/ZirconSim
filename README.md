# ZirconSim

`zircon-2026` is the deterministic Verilator and differential-test harness for
Zircon-2026. It replaces wall-clock randomness, raw-binary-only loading, and
illegal-instruction termination with explicit seeds, ELF32 loading, symbol
resolution, and architectural `tohost` completion.

```sh
cmake -S .. -B ../build/cmake
cmake --build ../build/cmake --target zircon-sim --parallel
../build/cmake/bin/zircon-sim --elf path/to/test.elf
```

The build requires the Spike development library and its `riscv-riscv.pc`
pkg-config metadata. ZirconSim steps Spike as an in-process reference model,
avoiding the large commit-log stream of an external Spike process.

CMake tracks the Chisel sources, elaborates `ZirconCore`, invokes Verilator
through its native CMake integration, and links the simulator. The retained
Makefile is a compatibility wrapper around these CMake targets. Passing
`--parallel` without a number lets the native build tool use its maximum
parallelism, so the commands do not hard-code a host-specific job count.

Interactive runs use a colored terminal dashboard with live cycles, retired
instructions, IPC, and simulation speed. The final summary reports the
program result, Spike status, performance, and Markdown report path. Redirected
output remains a single JSON record for scripts and CI; use `--json` to force
that format in a terminal. Set `NO_COLOR=1` or pass `--no-color` for plain text,
and pass `--no-progress` to suppress the live status line.

The default build omits VCD instrumentation for simulation speed. Configure a
separate waveform build with `-DZIRCON_SIM_ENABLE_VCD=ON`, then use `--wave`
with `--wave-start` and `--wave-cycles` to keep the dump window bounded. Long
simulations reject an unbounded waveform request.

Pipeline and cache events are accumulated by RTL counters. ZirconSim reads
those counters once when the program exits and writes a Markdown report under
`reports/`; it does not sample the performance interface every cycle. The only
per-cycle debug reads are the three retirement lanes required by Difftest.

Counter CSRs cannot be compared by absolute value because Spike instructions
and RTL cycles advance them on different time bases. For `cycle`, `time`,
`instret`, `mcycle`, and `minstret` reads, Difftest compares the PC and
instruction, accepts the DUT result for that operation, copies the complete
architectural GPR/FPR state into Spike, and resumes strict comparison on the
next instruction. Synchronous exception steps are omitted from the Spike
retirement stream because the RTL retirement interface reports only
instructions that increment `minstret`.

When the `RV-Software` submodule is present, the `zircon-functest-dummy` target
builds the initial software test through the same top-level CMake build.

`zircon-sim-unit` checks the deterministic PRNG and loads an RV32 ELF,
including `PT_LOAD`, entry point, and `tohost/fromhost` symbols. Pass the ELF to
CTest at configure time with `-DZIRCON_TEST_ELF=/absolute/path/to/test.elf`.
A timeout is only successful when `--allow-timeout` is explicit.

The old handwritten partial RV32IM interpreter remains in the branch history
but is no longer the reference model. Commit-level comparison uses Spike.
