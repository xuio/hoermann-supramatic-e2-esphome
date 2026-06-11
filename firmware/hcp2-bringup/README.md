# HCP2 Bring-Up Firmware

This plain ESP-IDF application is the Phase 0d full-system Wokwi target. Its
default mode is LP-in-the-loop: the HP firmware embeds and loads the same Phase
0c `hcp2_lp` blob used by the ISS harness, then supervises the fixed mailbox ABI
at `HCP2_LP_MAILBOX_ADDR`.

The fallback mode still runs the portable `hcp2_core` responder on the ESP32-C6
HP core. That mode is for Wokwi/bring-up fallback only if the Wokwi LP-UART spike
shows a simulator gap; production HCP2 support remains LP-core based.

Both modes use the fixed target pins:

- GPIO4: HCP2 RX
- GPIO5: HCP2 TX
- GPIO6: DE signal
- 57600 baud, 8E1

Build with the local ESP-IDF environment:

```sh
idf.py -C firmware/hcp2-bringup build
```

The restart-loop build used by Wokwi CI overlays `sdkconfig.restart.defaults`
and keeps LP-in-the-loop mode enabled:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-restart \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.restart.defaults" build
```

The HP-fallback build overlays `sdkconfig.hp-fallback.defaults`:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-hp-fallback \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hp-fallback.defaults" build
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
