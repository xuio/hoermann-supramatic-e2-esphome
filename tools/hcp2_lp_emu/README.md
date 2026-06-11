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
```

This is instruction-set emulation, not a full C6 SoC timing model. The report
maps instruction/cycle counts to 20 MHz and uses fast-forwarded LP busy-wait
helpers so long closed-loop runs stay practical. Phase 1 calibrates the timing
factor against real silicon.
