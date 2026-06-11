# HCP2 Wokwi Emulation

This directory contains the Phase 0d Wokwi emulation harness. It boots the
ESP32-C6 HP firmware in Wokwi and connects the target pins to a custom
SupraMatic 4 HCP2 master chip:

- GPIO4: LP-UART RX from the master chip
- GPIO5: LP-UART TX to the master chip
- GPIO6: LP GPIO DE, watched by the master chip for canonical trace events

As of the 2026-06-11 check of <https://docs.wokwi.com/guides/esp32>, Wokwi lists
ESP32-C6 UART and the C6 ULP processor as supported. A token-gated Phase 0d run
on 2026-06-11 found two narrower simulator gaps for this firmware path:
ESP-IDF's `lp_core_uart_init()` blocks forever waiting for the LP-UART
`reg_update` bit, and even with a Wokwi-only bypass for that wait,
`ulp_lp_core_run()` returns `ESP_OK` but the LP heartbeat never advances. CI
therefore treats Wokwi as an HP-firmware/custom-chip integration layer, not as
an LP execution authority.

What this proves:

- The real HP firmware can embed and call `ulp_lp_core_load_binary()` /
  `ulp_lp_core_run()` with the real Phase 0c LP blob.
- The Wokwi LP execution probe records the current simulator limitation with
  `HCP2_WOKWI_LP_EXEC_UNSUPPORTED` instead of pretending the LP path passed.
- The HP fallback responder can run against the same pin-to-pin custom-chip
  master for boot, UART, restart, and trace plumbing.
- The Wokwi master and Phase 0c ISS harness emit the same canonical event trace
  format: `master_frame`, `slave_frame`, `reply_latency`, and `de`.
- The trace comparator can fail CI on frame-sequence divergence between the
  Wokwi HP-fallback run and the ISS LP baseline. Latency is intentionally loose
  for that comparison because the responder core differs.

What this does not prove:

- Physical RS-485 timing, parity, baud tolerance, fail-safe biasing, line
  contention, or real transceiver DE timing.
- Wokwi LP-core or LP-UART execution for this ESP-IDF LP blob. The ISS remains
  the Phase 0c reference for 16-byte FIFO pressure, partial TX acceptance, MMIO
  coverage, and instruction budgets.
- Silicon reset behavior. Wokwi restart-loop mode is a useful integration spike;
  Phase 1 HIL is still the authority for whether `esp_restart()` leaves the real
  LP core running.

Entry spike status:

- LP-UART at 57600 8E1: Wokwi currently cannot execute this path. The
  `supervisor.yaml` LP probe expects `HCP2_WOKWI_LP_EXEC_UNSUPPORTED` after
  load/run, documenting the simulator gap.
- FIFO partial-TX fidelity: Wokwi behavior is intentionally judged by comparison
  to the ISS trace; any divergence is recorded as a simulator-fidelity finding.
- DE via LP GPIO: GPIO6 is wired to the custom chip and emitted as `de` trace
  events.
- `esp_restart()` LP survival: not proven in Wokwi. The restart scenario uses
  HP fallback only; Phase 1 HIL remains the authority.

Build the firmware and custom chip:

```sh
idf.py -C firmware/hcp2-bringup build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wokwi.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback-restart \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults;sdkconfig.restart.defaults" build
garage-generate-wokwi-hcp2-constants --check
cd emulation/wokwi
wokwi-cli chip compile supramatic4.chip.c -o chips/supramatic4.chip.wasm
```

Run the LP execution probe:

```sh
cp emulation/wokwi/wokwi.toml /tmp/hcp2-wokwi.toml
cp emulation/wokwi/wokwi.lp-probe.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --scenario supervisor.yaml
cp /tmp/hcp2-wokwi.toml emulation/wokwi/wokwi.toml
```

Run the HP-fallback steady-state scenario and ISS frame comparison:

```sh
uv run garage-hcp2-lp-emu \
  --blob firmware/hcp2-bringup/build/esp-idf/main/hcp2_lp/hcp2_lp.bin \
  --cycles 200 \
  --report iss-wokwi-baseline.json \
  --trace iss-wokwi-trace.jsonl
wokwi-cli emulation/wokwi \
  --scenario steady-state.yaml \
  --fail-text HCP2_WOKWI_VERDICT_FAIL | tee emulation/wokwi/steady-state.full.log
uv run garage-compare-hcp2-traces \
  --wokwi-log emulation/wokwi/steady-state.full.log \
  --iss-report iss-wokwi-baseline.json \
  --latency-tolerance-us 100000 \
  --output trace-compare.json
```

Run the restart-loop scenario:

```sh
cp emulation/wokwi/wokwi.toml /tmp/hcp2-wokwi.toml
cp emulation/wokwi/wokwi.restart.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --diagram-file diagram.restart.json \
  --scenario restart-loop.yaml --fail-text HCP2_WOKWI_VERDICT_FAIL
cp /tmp/hcp2-wokwi.toml emulation/wokwi/wokwi.toml
```

`PASS` and `FAIL` are still output pins on the custom chip for interactive
inspection, but CI greps the full `wokwi-cli` log for
`HCP2_WOKWI_VERDICT_*` markers because scenario `wait-serial` only watches the
ESP serial stream, while custom-chip `printf()` output is emitted on the chip
console.
