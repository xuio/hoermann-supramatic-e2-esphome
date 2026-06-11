# HCP2 Bring-Up Firmware

This plain ESP-IDF application is the Phase 0d full-system Wokwi target. Its
default mode is LP-in-the-loop: the HP firmware embeds and loads the same Phase
0c `hcp2_lp` blob used by the ISS harness, then supervises the fixed mailbox ABI
at `HCP2_LP_MAILBOX_ADDR`.

The fallback mode still runs the portable `hcp2_core` responder on the ESP32-C6
HP core. That mode is for Wokwi/bring-up fallback only. A token-gated Wokwi
research pass on 2026-06-11 showed that current Wokwi C6 LP execution is not
usable for this firmware path: the official Wokwi C6 LP example no longer
completes, an IDF 5.5.4 rebuild of that example also fails, a minimal
no-peripheral LP shared-variable example fails, and the LP-UART `reg_update` bit
does not self-clear. Production HCP2 support remains LP-core based and Phase 1
HIL remains the authority for LP behavior.

Both modes use the fixed target pins:

- GPIO4: HCP2 RX
- GPIO5: HCP2 TX
- GPIO6: DE signal
- 57600 baud, 8E1

Build with the local ESP-IDF environment:

```sh
idf.py -C firmware/hcp2-bringup build
```

The Wokwi LP probe overlays `sdkconfig.wokwi.defaults`. This enables a
Wokwi-only bypass around the ESP-IDF LP-UART `reg_update` wait so the probe can
reach `ulp_lp_core_run()` and record the current simulator limitation:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wokwi.defaults" build
```

The HP-fallback Wokwi builds overlay `sdkconfig.hp-fallback.defaults`:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults" build
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-wokwi-hp-fallback-restart \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults;sdkconfig.restart.defaults" build
```

In LP-in-the-loop mode the boot log contains:

- `HCP2_BRINGUP_MODE lp-in-loop`
- `HCP2_LP_LOAD_RELOAD` on a fresh LP load, or `HCP2_LP_SKIP_RELOAD` if the
  heartbeat is live across an HP restart.
- `HCP2_SUPERVISOR_REAL_MAILBOX_OK` after the HP supervisor verifies skip-reload
  health and command epoch/ack behavior against the real mailbox.

In HP-fallback mode, `CONFIG_HCP2_BRINGUP_MAILBOX_TEST_DOUBLE` can exercise the
same supervisor helper functions against a synthetic mailbox. That fallback does
not replace the LP-in-the-loop scenario or silicon validation.
