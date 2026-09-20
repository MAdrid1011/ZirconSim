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

The supported Linux entry point lives at the repository root and selects the
validated build, PGO, differential-test, checkpoint, logging, and UART settings:

```sh
make -C RV-Software/linux-system linux
```

It computes a fingerprint over the RTL, simulator, build configuration, tool
versions, and Linux payload. A missing or stale profile triggers a 20-million-
cycle differential PGO training run; an unchanged profile is reused. The final
simulator uses Release `O3`, ThinLTO, native host instructions, five Verilator
runtime threads, and four model build jobs. This default PGO path requires
Clang/AppleClang and a matching `llvm-profdata`; the launcher selects
`clang++` from `PATH` when `CXX` is unset.

For simulator development, an equivalent save/restore build can be configured
manually:

```sh
cmake -S .. -B ../build/sim-checkpoint \
    -DZIRCON_SIM_ENABLE_CHECKPOINTS=ON \
    -DZIRCON_VERILATOR_THREADS=5
cmake --build ../build/sim-checkpoint --target zircon-sim --parallel 4
../build/sim-checkpoint/bin/zircon-sim \
    --elf path/to/fw_payload.elf \
    --platform linux \
    --max-cycles 0xffffffffffffffff \
    --checkpoint-save build/linux-latest \
    --checkpoint-interval 40000000
```

Each interval atomically replaces `build/linux-latest.rtl` and
`build/linux-latest.host`, so only the newest checkpoint is retained. The host
file contains memory, AXI, device, UART, statistics, and Spike differential
state. It also records the WFI watchdog state and RTL file fingerprint, and
rejects a mismatched pair, ELF image, platform, differential-testing mode, or
seed. Version 2 is the current format; the reader remains compatible with
version 1 files. Resume with the same
simulation options plus `--checkpoint-load build/linux-latest`. The
`--max-cycles` value remains the global cycle limit after a restore. A one-time
save can instead use `--checkpoint-cycle`, or `--checkpoint-marker` can save on
the first matching UART text.

For an interactive Linux shell, omit `--pass-marker` and `--uart-input`, then
add `--uart-stdio`. ZirconSim polls standard input without blocking simulation
and forwards typed bytes to the UART. The same option can be used when
restoring a Linux checkpoint; once the saved UART input queue is empty, the
terminal remains attached to the shell.

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
