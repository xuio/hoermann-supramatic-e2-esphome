# HCP2 Wokwi Emulation

This directory contains the Phase 0d full-firmware emulation harness. It boots
the ESP32-C6 HP core in Wokwi, runs the plain ESP-IDF bring-up firmware, and
connects UART1 on GPIO4/GPIO5 to a custom SupraMatic 4 HCP2 master chip.

What this proves:

- ESP-IDF boot and UART1 configuration on the fixed HCP2 pins.
- The HP fallback responder can answer bus scan and steady-state polls.
- `esp_restart()` integration returns to the responder after an HP reboot.
- HP supervisor code exercises skip-reload, stale-heartbeat, and epoch paths
  against a mailbox test double.

What this does not prove:

- Physical RS-485 timing, parity, baud tolerance, line contention, or DE
  turnaround. Wokwi UART emulation is byte-level for this harness.
- Production LP-core behavior. The LP responder is validated by the Phase 0c
  ISS harness; Wokwi is an integration test for the HP firmware path.

As of the June 2026 Wokwi feature table, ESP32-C6 UART and the C6 ULP processor
are both listed as simulated. This harness still keeps Wokwi LP coverage as a
differential reference only: the Phase 0c ISS harness remains the authority for
the exact LP blob until Wokwi's C6 LP-UART path is modeled and cross-checked.

Build the firmware and custom chip:

```sh
idf.py -C firmware/hcp2-bringup build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-restart \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.restart.defaults" build
garage-generate-wokwi-hcp2-constants --check
cd emulation/wokwi
wokwi-cli chip compile supramatic4.chip.c -o chips/supramatic4.chip.wasm
```

Run the steady-state scenario:

```sh
wokwi-cli emulation/wokwi --scenario steady-state.yaml --fail-text HCP2_WOKWI_VERDICT_FAIL
```

Run the restart-loop scenario:

```sh
cp emulation/wokwi/wokwi.restart.toml emulation/wokwi/wokwi.toml
wokwi-cli emulation/wokwi --diagram-file diagram.restart.json \
  --scenario restart-loop.yaml --fail-text HCP2_WOKWI_VERDICT_FAIL
git checkout -- emulation/wokwi/wokwi.toml
```

`PASS` and `FAIL` are output pins on the custom chip so automation scenarios can
assert verdicts without depending on chip-console capture.
