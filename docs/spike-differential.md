# M1 Spike Commit-Prefix Differential

`CommitTraceDiff` compares the temporary executable M1 core with the exact
Spike revision in the parent `toolchain.lock.json`. `make diff` runs two
freestanding `RV32I_Zicsr_Zifencei` ELFs at `0x80000000`:

- `rv32i-commit-prefix` compares 17 dynamic instructions covering integer
  dataflow, a taken branch, JAL, and committed CSR read/write behavior.
- `rv32i-alu-branch-prefix` compares 32 dynamic instructions covering every
  register and immediate integer ALU encoding, `LUI`, `JALR`, and all six legal
  conditional branch relations with a mix of taken and not-taken control flow.

Each next instruction is a `tohost` store and must block in M1, because no LSU
exists yet.

Each RTL runner receives explicit nonzero AXI seed 1, a prefix-specific
`--expect-retired` count, and a fixed cycle limit. It may return only the
explicit allowed timeout after the required prefix has retired. Spike uses the
same prefix-specific `--instructions` count, so each log is bounded by
architectural retirement rather than host elapsed time.

For every event, the comparator requires matching M-mode privilege, PC,
instruction bits, optional GPR write, and optional CSR write. It rejects trap,
interrupt, floating, or memory events in this M1-only prefix. Those fields are
deliberately deferred to the M3/M4 full differential suites; this smoke does
not claim ACT4, Sail, full exception, or memory-differential coverage.

Run from `ZirconSim` with an explicitly built locked Spike binary:

```bash
make diff SPIKE=/path/to/spike
```

The command retains both program JSONL and Spike logs in `build/` as
reproducible failure evidence.
