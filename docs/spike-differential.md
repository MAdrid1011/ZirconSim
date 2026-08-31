# M1/M2 Commit-Prefix Differential

`CommitTraceDiff` compares the temporary executable M1/M2 core with the exact
Spike revision in the parent `toolchain.lock.json`. `make diff` runs two
freestanding `RV32I_Zicsr_Zifencei` ELFs at `0x80000000`:

- `rv32i-commit-prefix` compares 17 dynamic instructions covering integer
  dataflow, a taken branch, JAL, and committed CSR read/write behavior.
- `rv32i-alu-branch-prefix` compares 32 dynamic instructions covering every
  register and immediate integer ALU encoding, `LUI`, `JALR`, and all six legal
  conditional branch relations with a mix of taken and not-taken control flow.
- `rv32m-commit-prefix` compares 17 dynamic instructions covering all eight
  RV32M operations, signed/unsigned product halves, normal signed div/rem,
  divide-by-zero, and `INT_MIN / -1` and remainder overflow rules.

Each next instruction is a `tohost` store and must block through M2, because no
LSU exists yet.

Each RTL runner receives explicit nonzero AXI seed 1, a prefix-specific
`--expect-retired` count, and a fixed cycle limit. It may return only the
explicit allowed timeout after the required prefix has retired. Spike uses the
same prefix-specific `--instructions` count, so each log is bounded by
architectural retirement rather than host elapsed time.

For every event, the comparator requires matching M-mode privilege, PC,
instruction bits, optional GPR write, and optional CSR write. It rejects trap,
interrupt, floating, or memory events in these M1/M2-only prefixes. Those fields
are deliberately deferred to the M3/M4 full differential suites; this smoke does
not claim ACT4, full exception, or memory-differential coverage.

Run from `ZirconSim` with an explicitly built locked Spike binary:

```bash
make diff SPIKE=/path/to/spike
```

The command retains both program JSONL and Spike logs in `build/` as
reproducible failure evidence.

## RV32M Sail Cross-Check

The locked Sail-RISC-V source revision is
`beaf44991eee362a062fcaaf6fcb78ca428ff710`, recorded as `sailRiscv` in the
parent `toolchain.lock.json`. The observed local evidence used Sail compiler
0.20.2 and the model's `build/c_emulator/sail_riscv_sim`. Sail compiler 0.20.2
or a later compiler accepted by the checked-out model is required; do not use a
different Sail-RISC-V source revision.

Build the model outside this submodule so generated model artifacts are never
mistaken for source evidence. With a Sail compiler available on `PATH`, run:

```bash
git clone https://github.com/riscv/sail-riscv.git /work/sail-riscv
git -C /work/sail-riscv checkout --detach beaf44991eee362a062fcaaf6fcb78ca428ff710
cd /work/sail-riscv
./build_simulator.sh
```

The Sail project documents its compiler installation options. The build must
report the selected compiler version and produce
`/work/sail-riscv/build/c_emulator/sail_riscv_sim`. Then run from `ZirconSim`:

```bash
make diff-sail-rv32m \
  SAIL=/work/sail-riscv/build/c_emulator/sail_riscv_sim
```

This target runs exactly the existing seed-1 RV32M ELF for 17 retirements and
keeps the JSONL retire trace and Sail log in `build/`. It parses instruction,
GPR, and CSR trace lines. Sail can log internal CSR maintenance (for example a
`mip` write) after an ordinary instruction; the comparator associates CSR
writes only with an architectural CSR instruction, so such model-internal log
lines cannot create a false mismatch. This is bounded M2 cross-check evidence,
not full Sail coverage, trap/interrupt validation, or a replacement for the
required M3--M6 differential suites.
