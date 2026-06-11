# HCP2 Bring-Up Firmware

This plain ESP-IDF application runs the portable `hcp2_core` responder on the
ESP32-C6 HP core for firmware-integration emulation. It is a bring-up target for
Wokwi and HIL plumbing only; production HCP2 support is still the LP-core path
from `firmware/hcp2-lp/`.

The app configures UART1 on the fixed LP-UART pins used by the target hardware:

- GPIO4: HCP2 RX
- GPIO5: HCP2 TX
- GPIO6: bench DE signal for the responder port callback
- 57600 baud, 8E1

Build with the local ESP-IDF environment:

```sh
idf.py -C firmware/hcp2-bringup build
```

The restart-loop build used by Wokwi CI overlays `sdkconfig.restart.defaults`:

```sh
idf.py -C firmware/hcp2-bringup -B firmware/hcp2-bringup/build-restart \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.restart.defaults" build
```

At boot, `CONFIG_HCP2_BRINGUP_MAILBOX_TEST_DOUBLE` exercises the LP-supervisor
skip-reload, stale-heartbeat, and command-epoch paths against a synthetic healthy
mailbox. This does not replace silicon validation; it only proves the HP firmware
integration calls the intended supervisor paths.
