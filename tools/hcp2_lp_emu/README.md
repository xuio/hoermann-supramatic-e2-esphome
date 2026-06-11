# HCP2 LP Emulator

`garage-hcp2-lp-emu` runs the ESP32-C6 LP-core HCP2 responder ELF under
Unicorn and connects its modeled LP-UART pins directly to the Phase 0b
SupraMatic simulator.

The harness models the pieces the LP blob actually touches: LP SRAM at
`0x50000000`, the fixed mailbox at `0x50002000`, LP-UART RX/TX FIFOs with a
16-byte depth, DE on LP GPIO6, and benign PMU/clock/wake-cause stubs. Any MMIO
outside the modeled pages fails the run; every modeled register access is
reported in the manifest.

Useful commands:

```sh
uv run garage-hcp2-lp-emu --rvc-smoke
uv run garage-hcp2-lp-emu --blob firmware/hcp2-lp/build/hcp2_lp.bin --cycles 1000 --report lp-report.json
uv run pytest tests/hcp2/test_lp_emu.py
```

The same package also contains the Phase 0d Tier 1 dual-ISS harness. It maps one
host LP-SRAM buffer into two Unicorn engines with `uc_mem_map_ptr()`: the LP
engine runs the real LP ELF, while the HP engine runs a tiny RV32 bare-metal
control loop linked against the portable `hcp2_supervisor` and mailbox sources.
The HP ISS build needs a RISC-V bare-metal GCC (`riscv32-esp-elf-gcc` or
`riscv64-unknown-elf-gcc`); without one, local tests skip the dual-ISS cases.
CI installs the compiler in the LP emulation job.

This is instruction-set emulation, not a full C6 SoC timing model. The report
maps instruction/cycle counts to 20 MHz and uses fast-forwarded LP busy-wait
helpers so long closed-loop runs stay practical. Phase 1 calibrates the timing
factor against real silicon.
