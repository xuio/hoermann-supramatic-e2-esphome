# HCP2 Bring-Up Firmware

This plain ESP-IDF application is the Phase 0d full-system Wokwi target. Its
default mode is LP-in-the-loop: the HP firmware embeds and loads the same Phase
0c `hcp2_lp` blob used by the ISS harness, then supervises the fixed mailbox ABI
at `HCP2_LP_MAILBOX_ADDR`.

The fallback mode still runs the portable `hcp2_core` responder on the ESP32-C6
HP core. That mode is for Wokwi/bring-up fallback only. Wokwi retests on
2026-06-11 and 2026-06-12 verified that the current backend can run C6 LP code
and native LP-UART pin traffic: the official LP examples pass, the HCP2 scan
repro receives the exact signature response, and this firmware's real
LP-in-the-loop steady-state fixture passes with 200/200 replies and zero misses.
The old GPIO bit-banged UART workaround was deliberately never added, and the
Wokwi-specific LP-UART `reg_update` bypass remains removed. This LP-in-the-loop
build is the primary no-hardware full-firmware gate. Phase 1 HIL remains the
authority for physical LP-UART parity/timing, RS-485 electrical behavior,
transceiver DE behavior, silicon reset behavior, and reset-time bus glitches.

Both modes use the fixed target pins:

- GPIO4: HCP2 RX
- GPIO5: HCP2 TX
- GPIO0: DE signal
- GPIO1: /RE signal, low while listening and high only during local TX
- 57600 baud, 8E1

The Phase 1 bench wiring uses the same pins. On the current NixOS rig the stable
device paths are:

- RS-485 adapter: `/dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00`
- ESP serial console/flashing UART: `/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B61095943-if00`
- ESP USB/JTAG console: `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_20:6E:F1:09:2E:D4-if00`

Prefer the serial console/flashing UART for `idf.py flash` and console capture.
The USB/JTAG console can wedge after an RTC-WDT reset loop. The RS-485 adapter is
CDC-ACM, not FTDI, so there is no Linux `latency_timer`; the simulator requests
pyserial low-latency mode and the bench host should keep USB autosuspend disabled
for the hub while running HIL.

Build with the local ESP-IDF environment:

```sh
idf.py -C firmware/hcp2-bringup build
```

For overlay builds, pass an isolated `SDKCONFIG` path with `-D SDKCONFIG=...`.
ESP-IDF merges defaults only when creating a new sdkconfig; an existing
`firmware/hcp2-bringup/sdkconfig` can otherwise override the requested probe
defaults.

The Wokwi LP probe overlays `sdkconfig.wokwi.defaults`. It enables Wokwi's
8N1/no-parity UART mode so the custom-chip UART API can drive the test bus; real
hardware builds must use HCP2's 8E1 framing. The old Wokwi-only LP-UART
`reg_update` bypass was removed after the backend fix:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi \
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wokwi.defaults" build
```

The HP-fallback Wokwi builds overlay `sdkconfig.hp-fallback.defaults`:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback \
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-hp-fallback-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback-restart \
  -D SDKCONFIG=/tmp/hcp2-bringup-wokwi-hp-fallback-restart-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults;sdkconfig.restart.defaults" build
```

The HIL reset-policy probes use the normal LP-in-the-loop build plus one of the
reset overlays:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-hil-panic-halt \
  -D SDKCONFIG=/tmp/hcp2-bringup-hil-panic-halt-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.panic-halt.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-hil-task-wdt-nonpanic \
  -D SDKCONFIG=/tmp/hcp2-bringup-hil-task-wdt-nonpanic-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.task-wdt-nonpanic.defaults" build
```

The LP-WDT probe overlay compiles a diagnostic LP-core watchdog experiment. It
is not enabled in the default image because ESP32-C6 public headers expose
CPU/system reset actions, and the reset scope must be proven with the LA before
it can become a production safety mechanism:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-hil-lp-wdt-probe \
  -D SDKCONFIG=/tmp/hcp2-bringup-hil-lp-wdt-probe-sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.lp-wdt-probe.defaults" build
```

A first 2026-06-12 bench probe proved this must stay diagnostic-only for now:
the targetless Stage0 `RESET_CPU` LP-WDT setup does not reset the LP core after
feeding stops. The C6 LL helpers expose HP CPU/system reset targets, not a
documented LP-core-local reset target. Production recovery therefore remains the
HP supervisor's stale-heartbeat reload path unless a later probe proves a
bus-continuity-safe LP-only reset.

In LP-in-the-loop mode the boot log contains:

- `HCP2_BRINGUP_MODE lp-in-loop`
- `HCP2_LP_LOAD_RELOAD` on a fresh LP load, or `HCP2_LP_SKIP_RELOAD` if the
  heartbeat is live across an HP restart.
- `HCP2_SUPERVISOR_REAL_MAILBOX_OK` after the HP supervisor verifies skip-reload
  health and command epoch/ack behavior against the real mailbox.
- `polls_seen`, `polls_answered`, `last_poll_us`, `crc_error_count`,
  `rx_error_count`, `tx_abort_count`, `collision_count`, `max_de_hold_us`, and
  `stop_trigger_fire_count` counters in the mailbox/trace logs. These are Phase
  0f diagnostics for HIL/tooling. The HP supervisor does not reload the LP
  merely because a poll counter stalls or a single poll is missed; reload
  remains a stale-heartbeat or ABI/version decision.

In HP-fallback mode, `CONFIG_HCP2_BRINGUP_MAILBOX_TEST_DOUBLE` can exercise the
same supervisor helper functions against a synthetic mailbox. That fallback does
not replace the LP-in-the-loop scenario or silicon validation.

## Load Characterization

The HIL load runner keeps the same virtual SupraMatic master path and starts
optional shell commands while the serial HIL scenario runs. Use it from the
NixOS bench host so the RS-485 adapter and ESP32-C6 are local to the same
machine:

```sh
uv run garage-hcp2-hil-load \
  --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 \
  --cycles 1000 \
  --output captures/hcp2/hil-load-baseline.json
```

Add `--load-command` entries for API, OTA, ping, log-stream, or Wi-Fi traffic
generators. Each run writes a JSON report with the simulator verdict and the
load-command lifecycle:

```sh
uv run garage-hcp2-hil-load \
  --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 \
  --cycles 1000 \
  --load-command 'ping -i 0.05 supramatic-4-dev.local' \
  --output captures/hcp2/hil-load-ping.json
```

For LA correlation, the simulator can also write a per-poll JSONL trace. This is
the easiest way to match a missed simulator poll to the corresponding DE window
and UART frame on the logic analyzer:

```sh
uv run garage-supramatic-sim \
  --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 \
  --cycles 105 \
  --trace captures/hcp2/hil-reset.sim-trace.jsonl \
  --report captures/hcp2/hil-reset.sim.json
```

## Logic Analyzer Tooling

The logic analyzer capture helper uses `sigrok-cli`:

```sh
uv run garage-hcp2-hil-la capture \
  --driver fx2lafw \
  --samplerate 2m \
  --duration 10s \
  --channels de=D0,re=D1,tx=D2,rx=D3 \
  --output captures/hcp2/hil-reset.csv \
  --output-format csv \
  --dry-run
```

The analyzer accepts CSV or JSON samples with `time_s`, `de`, `re`, `tx`, and
`rx` columns/fields. It checks that DE starts low, TX starts idle-high, DE pulses
stay below the configured deadman limit, TX transitions happen only while DE is
high, and `/RE` does not float high unexpectedly. For runtime captures, `/RE`
may be high while DE is high because the firmware disables the receiver during
its own transmit window:

```sh
uv run garage-hcp2-hil-la analyze \
  --input captures/hcp2/hil-reset.csv \
  --max-de-high-us 9000 \
  --allow-re-high-during-de \
  --output captures/hcp2/hil-reset-la-report.json
```

For explicit-DE reset analysis, keep the full-window report above as the
boot-safety verdict, then generate a second runtime-only report after the
startup window has settled. `--allow-tx-outside-de` is valid only for
explicit-DE hardware: TX may toggle while the transceiver is disabled, but DE
must still stay inside the configured pulse limit.

```sh
uv run garage-hcp2-hil-la analyze \
  --input captures/hcp2/hil-reset.csv \
  --max-de-high-us 9000 \
  --ignore-before-us 100000 \
  --allow-re-high-during-de \
  --allow-tx-outside-de \
  --output captures/hcp2/hil-reset-runtime-la-report.json
```

Decode the HCP2 TX bytes from every DE window and validate 57600 8E1 parity plus
Modbus CRC and status-counter continuity with:

```sh
uv run garage-hcp2-hil-la decode-uart \
  --input captures/hcp2/hil-reset.csv \
  --channels tx=D1,de=D3,re=D5,rx=D7 \
  --baud 57600 \
  --output captures/hcp2/hil-reset-uart.json
```

For reset gates, use the combined verifier. It fails if either the electrical
checks fail or the decoded UART stream has any parity/CRC error or missing
status counter:

```sh
uv run garage-hcp2-hil-la verify \
  --input captures/hcp2/hil-reset.csv \
  --channels tx=D1,de=D3,re=D5,rx=D7 \
  --max-de-high-us 9000 \
  --allow-re-high-during-de \
  --allow-tx-outside-de \
  --min-status-frames 100 \
  --output captures/hcp2/hil-reset-verdict.json
```

The current connected LA wiring uses this mapping:

- Saleae CH2 / sigrok D1: TX, ESP32-C6 GPIO5
- Saleae CH4 / sigrok D3: DE, ESP32-C6 GPIO0
- Saleae CH6 / sigrok D5: /RE, ESP32-C6 GPIO1
- Saleae CH8 / sigrok D7: RX, ESP32-C6 GPIO4

## Phase 1 HIL Results

Latest v9 closeout on 2026-06-13 with mailbox ABI v3, LP stop-trigger support,
and GPIO0/GPIO1 direction control:

- Serial closeout: `runtime` 1000/1000 replies and `fault-recovery` 251/251
  replies, zero misses.
- LA closeout preset: 160/160 simulator replies, zero misses; LA electrical
  verdict ok, decoded UART verdict ok, 158 decoded status frames, zero
  status-counter gaps, zero TX transitions outside DE, zero /RE-high samples
  outside DE, and `max_de_high_us ~= 4259`.
- OTA/API-restart closeout preset: OTA upload while simulator traffic ran saw
  500/500 replies with zero misses; API restart while simulator traffic ran saw
  350/350 replies with zero misses. Both invoked commands exited 0.

`garage-hcp2-closeout --preset la` and `--preset ota-restart` now encode these
repeatable checks. On the current NixOS bench, pre-seed the ESPHome
`firmware.bin` and pass `--skip-esphome-compile` for the OTA preset, because
PlatformIO's generic Linux penv helper cannot execute on NixOS without an
extra compatibility layer.

Latest v8 closure on 2026-06-12 with GPIO0/GPIO1 direction control:

- Real-cadence LA gate: 120/120 simulator replies, zero misses, LA decoded 121
  CRC-valid TX frames (scan + 120 status responses), zero status-counter gaps,
  zero TX transitions outside DE, zero /RE-high samples outside DE, and
  `max_de_high_us ~= 4260`.
- OTA continuity proof: while OTA upload/restart ran, the simulator saw 500/500
  replies with zero misses; LA decoded 501 CRC-valid TX frames, 500 status
  responses, zero gaps, and no electrical violations.
- API restart proof: while the ESPHome restart button was invoked over the API,
  the simulator saw 400/400 replies with zero misses; LA decoded 401 CRC-valid
  TX frames, 400 status responses, zero gaps, and no electrical violations.
- Hostile-load soak: 2000/2000 replies at real cadence under ping plus repeated
  API socket connection load, zero misses, `latency_p99_ms ~= 14.7`.
- Closeout runner smoke: default closeout passed on real serial
  (`runtime` 1000/1000, `fault-recovery` 251/251, zero misses).

The 2026-06-12 bench run, rerun after the Phase 0f mailbox/DE hardening, proved
the LP responder over the real C6 LP-UART, GPIO0/GPIO1 RS-485 direction control,
and the USB-RS485 master path:

- Stable responder: 500 HIL cycles, zero misses, zero error-04 verdicts
  (`latency_p99_ms ~= 16.7`).
- Fault injection: corrupt CRC, truncation, duplicate poll, jitter, garbage, and
  split writes recovered with zero missed valid polls over 121 polls.
- Modbus light command: command reply observed and steady-state polling stayed
  healthy.
- HP mailbox command path: scripted open, close, and light commands were acked by
  the LP firmware and observed by the simulator as button press/release responses
  over 260 polls.

Reset matrix, using LA-decoded UART frames as the authority:

- Planned `esp_restart`: clean from a warm stable state. The simulator saw
  105/105 replies, the LA decoded 106/106 CRC-valid frames (scan + counters
  0-104), DE was bounded to about `4.246 ms`, and `/RE` stayed low.
- Panic configured to reboot the HP core: not continuity-clean. The simulator
  missed counter 49, and the LA counter sequence skipped 49 even though DE stayed
  bounded. This is only one missed poll, but the production target is zero.
- Task-WDT configured to panic/reset: not continuity-clean. The simulator missed
  counter 64, and the LA captured a truncated 12-byte frame
  `021710400003010000000000` instead of the full status response.
- Panic configured to halt instead of reboot: bus continuity stayed clean. The
  simulator saw 105/105 replies and the LA decoded 106/106 CRC-valid frames with
  counters 0-104. The HP side remains halted and needs an external recovery path.
- Task-WDT with `trigger_panic = false`: bus continuity stayed clean. The
  simulator saw 105/105 replies and the LA decoded 106/106 CRC-valid frames with
  counters 0-104.
- RTC-WDT system reset: unsafe for continuity. It produced about `304.7 ms`
  DE-high windows, briefly drove `/RE` high, and missed two runs of four polls.

Design consequence: planned OTA/restart should use `esp_restart`; production
crash policy must not rely on HP panic/task-WDT reset as the bus-continuity path.
For safety continuity, prefer panic halt and non-panic task-WDT handling while
the LP keeps answering the motor. RTC-WDT remains forbidden as a continuity
mechanism. The production supervisor still reloads on stale heartbeat or
ABI/version mismatch; a live, advancing heartbeat plus bus-health counters is
the health signal, not the mailbox magic word alone.

Earlier logic-analyzer findings from the same 2026-06-12 bench session:

- Normal traffic passes: 25 simulated cycles produced 25/25 replies, zero
  misses; the LA saw 26 DE windows (scan + status replies), `max_de_high_us =
  4241.5`, `/RE` stayed low, and there were zero TX transitions outside DE.
- After adding the external default resistors, normal LP bus operation still
  passes with a longer strict capture: 180/180 simulated status polls answered,
  zero missing counters, initial DE low, initial `/RE` low, `/RE` never high,
  no TX outside DE, and `max_de_high_us = 4243 us`.
- Runtime DE behavior passes after the initial boot-default window is excluded
  and explicit-DE semantics are used: clean paths kept `/RE` low and bounded DE
  to `4241-4247 us`.
- LA UART decode proved the one-poll losses in the reboot-on-panic and
  reboot-on-task-WDT cases. Aggregate simulator verdicts can remain OK with one
  missed poll, so the LA decoded counter sequence is the reset-matrix authority.
- RTC-WDT is not continuity-safe: after startup it produced two DE-high windows
  of about `304.7 ms`, with `/RE` briefly high and 4 missed polls over 110
  polls. Production must not rely on RTC-WDT reset for bus continuity.
- Boot/download-mode safe-default behavior still fails and blocks motor
  connection even after the external default resistors were added: flashing plus
  hard reset through esptool held DE high for up to `4.295 s` and `/RE` high for
  the same window; serial reset-line probing showed DE/`/RE` high windows up to
  `721 ms` while manipulating DTR/RTS. These are driven/reset-mode windows, not
  normal LP firmware behavior.
- A later GPIO0/GPIO1 direction-pin retest kept runtime behavior clean but did
  not fix serial/download-mode reset behavior. `esptool --no-stub chip-id` held
  DE and `/RE` high for about `775 ms`; a full serial flash held them high for
  about `4.98 s`. Post-reset runtime polling recovered immediately, but serial
  flashing while attached to a real motor remains unsafe.
- The GPIO0/GPIO1 ESPHome dev image (`config_hash = 0xe58c6605` after adding the
  restart button) passed the closure matrix outside serial/download mode:
  4500/4500 replies across three OTA uploads/restarts; 3500/3500 replies across
  an API-triggered software restart; 6500 polls through a Wi-Fi kick/rejoin
  window with one isolated host-side receive timeout before the Wi-Fi drop; and
  command-path traces observed open, close, stop, and light button encodings
  without continuity loss.
- A 100 kHz LA capture is good enough for DE/`/RE` window checks, but it
  undersamples 57600 8E1 UART. Use at least 500 kHz for UART continuity
  verdicts (about 8.7 samples/bit); 1 MHz or higher is preferred when capture
  size is not a concern.

Design consequence: the firmware/LP runtime DE timing is clean for CPU-only
resets, OTA, API restart, Wi-Fi disruption, and normal command handling. The
bench hardware does **not** meet the independent safe-default requirement during
USB serial flashing/download-mode reset before firmware owns the pins. Do not
serial-flash or use reset/download-mode tooling while the transceiver is attached
to a real motor; use OTA-only operation or physically isolate the transceiver
during serial flashing.
