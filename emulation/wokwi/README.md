# HCP2 Wokwi Emulation

This directory contains the Phase 0d Wokwi emulation harness. It boots the
ESP32-C6 HP firmware in Wokwi and connects the target pins to a custom
SupraMatic 4 HCP2 master chip:

- GPIO4: LP-UART RX from the master chip
- GPIO5: LP-UART TX to the master chip
- GPIO6: LP GPIO DE, watched by the master chip for canonical trace events

As of the 2026-06-11 check of <https://docs.wokwi.com/guides/esp32>, Wokwi lists
ESP32-C6 UART and the C6 ULP processor as supported. A token-gated Phase 0d
research pass on 2026-06-11 found that LP-in-the-loop is not currently viable
for this project:

- The unmodified official `wokwi/esp32c6-i2c-lp` prebuilt ELF at commit
  `e065aa65ef924bf4e57e011ab659b9f1788e833c` loads the LP firmware and enters
  deep sleep, then hits HP watchdog resets and times out.
- Rebuilding the same Espressif `lp_i2c` example with ESP-IDF 5.5.4 also loops
  through `TG1_WDT_HPSYS` resets without an LP wakeup.
- A minimal no-peripheral HP_CPU-started LP shared-variable example also fails:
  the HP task waits forever for an LP-written value and trips the task watchdog.
- HP-side reads from the LP-UART register block return plausible reset values,
  but `LP_UART_REG_UPDATE` never self-clears after being set, so ESP-IDF's
  `lp_core_uart_init()` can hang in its hardware-sync wait.

CI therefore treats Wokwi as an HP-firmware/custom-chip integration layer, not
as an LP execution authority.

What this proves:

- The real HP firmware build embeds the Phase 0c LP blob and reaches the LP
  reload decision path with the same mailbox ABI. The Wokwi-specific LP probe
  deliberately bypasses LP-control register calls, including
  `ulp_lp_core_load_binary()` / `ulp_lp_core_run()`, because current Wokwi
  hangs inside that unsupported path.
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

- LP-UART at 57600 8E1: Wokwi currently cannot execute this path. The LP-UART
  register block is at least partially modeled, but `LP_UART_REG_UPDATE` remains
  set after the IDF driver writes it. The `supervisor.yaml` LP probe expects
  `HCP2_WOKWI_LP_EXEC_UNSUPPORTED` from the Wokwi-only bypass path before any
  LP-control register calls, documenting the simulator gap without hanging CI.
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
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wokwi.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback \
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-hp-fallback-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback-restart \
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-hp-fallback-restart-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults;sdkconfig.restart.defaults" build
uv run esphome compile configs/supramatic-4-dev.yaml
garage-generate-wokwi-hcp2-constants --check
cd emulation/wokwi
wokwi-cli chip compile supramatic4.chip.c -o chips/supramatic4.chip.wasm
```

Run the LP execution probe. This is expected to record the current Wokwi
limitation, not prove LP execution:

```sh
cp emulation/wokwi/wokwi.toml /tmp/hcp2-wokwi.toml
cp emulation/wokwi/wokwi.lp-probe.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --scenario supervisor.yaml
cp /tmp/hcp2-wokwi.toml emulation/wokwi/wokwi.toml
```

The upstream repro draft is in
[`UPSTREAM_REPRO.md`](UPSTREAM_REPRO.md). It records the official-example
control run, IDF 5.5.4 rebuild, minimal LP shared-variable probe, and LP-UART
register probe in a form suitable for `wokwi/wokwi-features`.

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

Run the ESPHome-built HP-fallback firmware against the same custom chip:

```sh
cp emulation/wokwi/wokwi.toml /tmp/hcp2-wokwi.toml
cp emulation/wokwi/wokwi.esphome.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --diagram-file diagram.esphome.json \
  --scenario esphome-steady-state.yaml
cp /tmp/hcp2-wokwi.toml emulation/wokwi/wokwi.toml
```

`PASS` and `FAIL` are still output pins on the custom chip for interactive
inspection, but CI greps the full `wokwi-cli` log for
`HCP2_WOKWI_VERDICT_*` markers because scenario `wait-serial` only watches the
ESP serial stream, while custom-chip `printf()` output is emitted on the chip
console. The ESPHome scenario is therefore a fixed-duration run whose assertion
is the custom-chip verdict in the full log.
