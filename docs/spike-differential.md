# M1 Spike Commit-Prefix Differential

`CommitTraceDiff` compares the temporary executable M1 core with the exact
Spike revision in the parent `toolchain.lock.json`. The test program is a
freestanding `RV32I_Zicsr_Zifencei` ELF at `0x80000000`; its first 17 dynamic
instructions exercise integer dataflow, a taken branch, JAL, and committed CSR
read/write behavior. Instruction 18 is a `tohost` store and must block in M1,
because no LSU exists yet.

The RTL runner receives an explicit nonzero AXI seed, `--expect-retired 17`,
and a fixed cycle limit. It may return only the explicit allowed timeout after
the required prefix has retired. Spike uses `--instructions=17`, so its log is
also bounded by architectural retirement rather than host elapsed time.

For every event, the comparator requires matching M-mode privilege, PC,
instruction bits, optional GPR write, and optional CSR write. It rejects trap,
interrupt, floating, or memory events in this M1-only prefix. Those fields are
deliberately deferred to the M3/M4 full differential suites; this smoke does
not claim ACT4, Sail, full exception, or memory-differential coverage.

Run from `ZirconSim` with an explicitly built locked Spike binary:

```bash
make diff SPIKE=/path/to/spike
```

The command retains `build/rv32i-commit-prefix.jsonl` and
`build/rv32i-commit-prefix.spike.log` as reproducible failure evidence.
