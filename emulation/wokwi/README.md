# HCP2 Wokwi Emulation

This directory contains the Phase 0d Wokwi emulation harness. It boots the
ESP32-C6 HP firmware in Wokwi and connects the target pins to a custom
SupraMatic 4 HCP2 master chip:

- GPIO4: LP-UART RX from the master chip
- GPIO5: LP-UART TX to the master chip
- GPIO0: LP GPIO DE, watched by the master chip for canonical trace events
- GPIO1: /RE, asserted only while DE is high so local echo is suppressed during TX

Wokwi status changed during the 2026-06-11 and 2026-06-12 retests. The earlier
Simulation API `1.0.0-20260608-g56f5da7e` could not run basic ESP32-C6 LP
examples. The 2026-06-11 backend, `1.0.0-20260611-g7cf12162`, now runs enough C6
LP code for:

- The unmodified official `wokwi/esp32c6-i2c-lp` example to repeatedly wake the
  HP core from deep sleep and print lux readings.
- This firmware's real LP blob to load with `ulp_lp_core_load_binary()`, run
  with `ulp_lp_core_run()`, advance heartbeat in shared LP SRAM, pass
  skip-reload health checks, reject a stale command epoch, and ack a fresh
  command.

A later 2026-06-12 backend, `1.0.0-20260612-g17cab5dd`, fixes the native
ESP32-C6 LP-UART pin path used by this harness. The official 9600/57600
LP-UART echo repros pass, the HCP2 scan fixture returns the byte-exact signature
response, and the real LP-in-loop steady-state run completes 200 polls with 200
replies, zero misses, and Wokwi reply latency in the 12.869-12.930 ms range. We
deliberately do not add a GPIO bit-banged UART workaround to the firmware. The
native LP-UART Wokwi gate can now be treated as the primary cloud
full-firmware pre-HIL test. The GitHub Wokwi job is manual-only: dispatch the
CI workflow with `run_wokwi` enabled. Set the GitHub repository variable
`WOKWI_NATIVE_LP_UART_REQUIRED=true` to make the native LP-UART subgate
blocking inside that manual job.

What this proves:

- The real HP firmware build embeds, loads, and runs the Phase 0c LP blob in
  Wokwi, then verifies the same mailbox ABI used by the ISS and HIL paths.
- The Wokwi LP execution probe is a mailbox/supervisor gate. It expects
  `HCP2_SUPERVISOR_REAL_MAILBOX_OK`.
- The HP fallback responder can run against the same pin-to-pin custom-chip
  master for boot, UART, restart, and trace plumbing.
- The Wokwi master and Phase 0c ISS harness emit the same canonical event trace
  format: `master_frame`, `slave_frame`, `reply_latency`, and `de`.
- The trace comparator can fail CI on frame-sequence divergence between the
  Wokwi HP-fallback run and the ISS LP baseline. Latency is intentionally loose
  for that comparison because the responder core differs.
- The Wokwi LP-UART steady-state scenario is the primary no-hardware proof that
  the real HP firmware, real LP blob, LP-UART pins, DE signal, and custom-chip
  master stay in lockstep.

What this does not prove:

- Physical RS-485 timing through a transceiver. Wokwi can become the primary
  full-firmware pre-HIL gate, but HIL remains the authority for parity,
  electrical timing, baud tolerance, line contention, fail-safe biasing, and
  real transceiver DE timing.
- Instruction-budget proof. The ISS remains the deterministic local reference
  for 16-byte FIFO pressure, partial TX acceptance, MMIO coverage, and
  instruction budgets.
- Silicon reset behavior. Wokwi restart-loop mode is a useful integration spike;
  Phase 1 HIL is still the authority for whether `esp_restart()` leaves the real
  LP core running.

Entry spike status:

- LP-UART at 57600 8N1 in Wokwi: the native LP-UART RX/TX pin path now passes
  the real LP-in-loop HCP2 steady-state fixture on Simulation API
  `1.0.0-20260612-g17cab5dd`. The real bus remains 57600 8E1; Wokwi custom-chip
  UARTs do not model parity, so HIL remains the parity authority.
- FIFO partial-TX fidelity: Wokwi behavior is intentionally judged by comparison
  to the ISS trace; any divergence is recorded as a simulator-fidelity finding.
- DE via LP GPIO: GPIO0 is wired to the custom chip and emitted as `de` trace
  events; GPIO1 carries the /RE guard signal.
- `esp_restart()` LP survival: not proven in Wokwi. The restart scenario uses
  HP fallback only; Phase 1 HIL remains the authority.

Build the firmware and custom chip:

```sh
idf.py -C firmware/hcp2-bringup build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi \
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wokwi.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback \
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-hp-fallback-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback-restart \
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-hp-fallback-restart-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults;sdkconfig.restart.defaults" build
uv run esphome compile configs/supramatic-4-wokwi.yaml
garage-generate-wokwi-hcp2-constants --check
cd emulation/wokwi
wokwi-cli chip compile supramatic4.chip.c -o chips/supramatic4.chip.wasm
```

Run the LP execution probe. This is expected to pass the real mailbox probe on
Wokwi backends that include the 2026-06-11 LP execution fix:

```sh
cp emulation/wokwi/wokwi.toml /tmp/hcp2-wokwi.toml
cp emulation/wokwi/wokwi.lp-probe.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --scenario supervisor.yaml
cp /tmp/hcp2-wokwi.toml emulation/wokwi/wokwi.toml
```

Run the native LP-UART smoke path. This validates the mailbox path, sends a
bus-scan frame through the native LP-UART pins, and expects a real response from
the LP blob.

```sh
cp emulation/wokwi/wokwi.toml /tmp/hcp2-wokwi.toml
cp emulation/wokwi/wokwi.lp-probe.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --scenario lp-uart-repro.yaml --timeout 15000
cp /tmp/hcp2-wokwi.toml emulation/wokwi/wokwi.toml
```

Run the native LP-UART steady-state gate. This uses the same real LP-in-loop
firmware as the repro, but waits long enough for the custom chip to complete the
full bus scan and steady-state cycle count:

```sh
cp emulation/wokwi/wokwi.toml /tmp/hcp2-wokwi.toml
cp emulation/wokwi/wokwi.lp-probe.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --scenario lp-uart-steady-state.yaml \
  --timeout 30000 --fail-text HCP2_WOKWI_VERDICT_FAIL | tee emulation/wokwi/lp-uart-steady-state.full.log
grep -q "HCP2_WOKWI_VERDICT_OK reason=steady-state" emulation/wokwi/lp-uart-steady-state.full.log
cp /tmp/hcp2-wokwi.toml emulation/wokwi/wokwi.toml
```

The upstream report text is in [`UPSTREAM_REPRO.md`](UPSTREAM_REPRO.md). It
records the official-example control run, IDF rebuild, minimal LP shared-variable
probe, HP-UART positive control, and LP-UART register probe in a form suitable
for `wokwi/wokwi-features`.

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

Run the minimal ESPHome-built LP firmware against the same custom chip:

```sh
cp emulation/wokwi/wokwi.toml /tmp/hcp2-wokwi.toml
cp emulation/wokwi/wokwi.esphome.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --diagram-file diagram.esphome.json \
  --scenario esphome-steady-state.yaml
cp /tmp/hcp2-wokwi.toml emulation/wokwi/wokwi.toml
```

`PASS` and `FAIL` are still output pins on the custom chip for interactive
inspection, but the manual Wokwi CI job greps the full `wokwi-cli` log for
`HCP2_WOKWI_VERDICT_*` markers because scenario `wait-serial` only watches the
ESP serial stream, while custom-chip `printf()` output is emitted on the chip
console. The ESPHome scenario is therefore a fixed-duration run whose assertion
is the custom-chip verdict in the full log.
