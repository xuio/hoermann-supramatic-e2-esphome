# HCP2 Bring-Up Firmware

This plain ESP-IDF application is the Phase 0d full-system Wokwi target. Its
default mode is LP-in-the-loop: the HP firmware embeds and loads the same Phase
0c `hcp2_lp` blob used by the ISS harness, then supervises the fixed mailbox ABI
at `HCP2_LP_MAILBOX_ADDR`.

The fallback mode still runs the portable `hcp2_core` responder on the ESP32-C6
HP core. That mode is for Wokwi/bring-up fallback only. A token-gated Wokwi
retest on 2026-06-11 showed that the current Wokwi backend can run C6 LP code:
the official LP example wakes the HP core from deep sleep, and this firmware's
real LP blob passes the mailbox health, skip-reload, and epoch/ack probe. The
2026-06-12 backend shows partial LP-UART progress at 9600 baud, but byte decode
is still wrong and the 57600 HCP2 bus-scan fixture still times out. We
deliberately do not add a GPIO bit-banged UART workaround to the firmware; the
Wokwi result was reported upstream as a simulator fidelity gap. Once the Wokwi
backend fix lands completely, this LP-in-the-loop build is the primary
no-hardware full-firmware gate. Phase 1 HIL remains the authority for physical
LP-UART timing, RS-485 electrical behavior, and reset-time bus glitches.

Both modes use the fixed target pins:

- GPIO4: HCP2 RX
- GPIO5: HCP2 TX
- GPIO2: DE signal
- GPIO3: /RE signal, held low so the receiver remains enabled during TX
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

The Wokwi LP probe overlays `sdkconfig.wokwi.defaults`. It enables Wokwi's
8N1/no-parity UART mode so the custom-chip UART API can drive the test bus; real
hardware builds must use HCP2's 8E1 framing. The old Wokwi-only LP-UART
`reg_update` bypass was removed after the backend fix:

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

## Phase 1 HIL Results

The 2026-06-12 bench run proved the LP responder over the real C6 LP-UART,
GPIO2/GPIO3 RS-485 direction control, and the USB-RS485 master path:

- Stable responder: 1000 HIL cycles, zero misses, zero error-04 verdicts.
- Fault injection: corrupt CRC, truncation, duplicate poll, jitter, garbage, and
  split writes recovered with zero missed valid polls.
- Modbus light command: command reply observed and steady-state polling stayed
  healthy.
- HP mailbox command path: scripted open, close, and light commands were acked by
  the LP firmware and observed by the simulator as button press/release responses.

Reset matrix:

- `esp_restart`: LP stayed alive, HP logged `HCP2_LP_SKIP_RELOAD`, 500/500 replies.
- Panic reset: LP stayed alive, HP logged `HCP2_LP_SKIP_RELOAD`, 500/500 replies.
- Task-WDT reset: LP stayed alive, HP logged `HCP2_LP_SKIP_RELOAD`, 250/250 replies.
- RTC-WDT system reset: LP did not stay continuously healthy. The boot log showed
  `rst:0x10 (LP_WDT_SYS)`, HP performed `HCP2_LP_LOAD_RELOAD`, and the simulator
  saw 16 missed polls over 250 cycles with a maximum run of 4 consecutive misses.

Design consequence: treat CPU-only resets as the continuity path, but do not rely
on RTC-WDT for safety-continuity. The production supervisor still reloads on stale
heartbeat or ABI/version mismatch; a live, advancing heartbeat is the health
signal, not the mailbox magic word alone.
