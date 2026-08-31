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
`make smoke` elaborates the parent RTL, builds `VZirconCore`, and checks the M0
top-level wiring for a bounded number of cycles. A timeout is only successful
when `--allow-timeout` is explicit.

The old handwritten partial RV32IM interpreter remains in the branch history
but is no longer the reference model. Commit-level comparison will use Spike,
with Sail as the nightly and dispute oracle.
