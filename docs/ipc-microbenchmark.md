# M2 Deterministic RV32M IPC Microbenchmark

## Purpose

This is the first comparable RV32IM IPC measurement for M2. It measures the
same fixed `rv32m-commit-prefix` ELF on Zircon-2026 and the immutable
Zircon-2024 core revision `65a3dd381f4c83a5844858a927dafdbc8263c35e` under
the same explicit AXI seed and the same deterministic AXI slave.

The benchmark stops at the first cycle that observes the 17th retired
instruction. The measured instruction count is exactly 17 and the reported IPC
is `17 / cycles`. Zircon-2024 can also retire the immediately younger `tohost`
store in the same dual-commit cycle; that store is deliberately excluded from
the measured prefix and never determines success. Zircon-2026 correctly blocks
that store until M3 provides an LSU. A `retire-limit` result means only that the
prescribed prefix retired; it is not an ELF pass result.

## Inputs and Interface

The workload is `tests/rv32m-commit-prefix.S`, linked as
`build/rv32m-commit-prefix.elf` with `RV32IM_Zicsr_Zifencei` and an entry point
at `0x80000000`. It covers all eight RV32M operations and the divide/remainder
zero-divisor and signed-overflow rules.

Both runners require a nonzero `--seed`, use `DeterministicAxiMemory`, and emit
a single JSON record containing `status`, `cycles`, `seed`, and `retired`.

```bash
make micro-ipc-rv32m
make baseline-ipc-rv32m BASELINE_2024=/work/Zircon-2024
```

`BASELINE_2024` must be a clean detached checkout at the immutable core SHA
above with the locked RV-Software and ZirconSim submodules. The target builds
that checkout's `VCPU` only to obtain its generated Verilator model; it does not
change the checkout's tracked source or use its wall-clock-random AXI memory.
`check-baseline-2024` verifies all three locked SHAs and the clean checkout
before the baseline runner is built.

## Observed Seed-1 Result

On the fixed 17-retirement prefix, the first measured result is:

| Core | Cycles to retirement 17 | Retired count | IPC |
| --- | ---: | ---: | ---: |
| Zircon-2024 fixed baseline | 186 | 17 | 0.09140 |
| Zircon-2026 M2 | 235 | 17 | 0.07234 |

Repeated local runs at seed 1 produced the same JSON records. Zircon-2026 is
20.9% lower on this divider-heavy microbenchmark. The result identifies a
LongPipe follow-up for later IPC work; it does not establish a general-workload
regression because the formal memory profile and workload suite are absent.

## Sampling and Invariants

The Zircon-2026 runner samples `RetireEvent` lanes in monotonically increasing
order. The Zircon-2024 runner drives both debug commit sinks ready and counts
accepted debug commit lanes only through the requested prefix count. In both
cases the cycle count is the first simulation cycle after reset in which the
17th retirement is observed.

The shared slave accepts only legal INCR AXI bursts, applies all backpressure
and response delays from the explicit seed, holds responses until handshake,
and checks the 4 KiB burst rule. A prefix must never be counted twice, skip an
older retirement, or pass because of a timeout.

## Scope Limits

The default microbenchmark AXI profile is the harness's seed-1 short-delay
profile, not the Handoff nominal 30-cycle-plus-jitter profile. It reports no
cache, MSHR, branch-MPKI, MMIO, or stall-breakdown data. It is evidence that
starts the M2 IPC comparison; it cannot satisfy the M0 baseline report, M5
performance gate, or any release performance claim. Those gates require the
full deterministic profile adapter, common tohost-completing workloads, and the
complete M3 memory subsystem.

## Verification Mapping

- `make unit` verifies ELF loading and deterministic AXI protocol behavior.
- `make micro-ipc-rv32m` verifies the M2 trace runner reaches exactly 17
  retirement events without accepting the blocked `tohost` store as success.
- `make baseline-ipc-rv32m BASELINE_2024=/work/Zircon-2024` verifies the same
  ELF and AXI model execute through the immutable 2024 Verilator top-level.
- Repeating either command with the same seed must preserve the JSON result.
