# Wokwi ESP32-C6 LP Repro Draft

Draft issue title:

```text
ESP32-C6 LP core examples no longer execute; LP-UART reg_update does not clear
```

Draft issue body:

```markdown
## Summary

Current Wokwi advertises ESP32-C6 ULP/LP processor support, and the official
`wokwi/esp32c6-i2c-lp` project appears intended to validate that path. Under
current Wokwi CLI/API, I cannot get either the official prebuilt example, an
ESP-IDF 5.5.4 rebuild of that example, or a minimal no-peripheral LP
shared-variable example to execute successfully. A separate HP-side register
probe also shows `LP_UART_REG_UPDATE` remains set forever after being written,
which makes ESP-IDF's LP-UART init spin.

## Environment

- Wokwi CLI: `0.26.1 (9d71b975b7eb)`
- Wokwi Simulation API: `1.0.0-20260608-g56f5da7e`
- Host: macOS
- Target: `board-esp32-c6-devkitc-1`
- ESP-IDF rebuild version: `v5.5.4`
- Official example repo: `wokwi/esp32c6-i2c-lp`
- Official example commit tested:
  `e065aa65ef924bf4e57e011ab659b9f1788e833c`

## Repro 1: official prebuilt example

Steps:

1. Clone `https://github.com/wokwi/esp32c6-i2c-lp`.
2. Check out commit `e065aa65ef924bf4e57e011ab659b9f1788e833c`.
3. Run:

   ```sh
   wokwi-cli . --scenario lp.test.yaml --timeout 20000
   ```

Observed:

- The HP firmware prints:
  - `LP I2C initialized successfully`
  - `LP core loaded with firmware successfully`
  - `Entering deep sleep...`
- The expected LP wakeup / lux output never appears.
- The simulation hits watchdog resets, including:
  - `Guru Meditation Error: Core  0 panic'ed (Interrupt wdt timeout on CPU0).`
  - `rst:0x8 (TG1_WDT_HPSYS)`
- The scenario times out.

Expected:

- The official scenario should complete and print the LP wakeup / lux result.

## Repro 2: same example rebuilt with ESP-IDF 5.5.4

Steps:

1. Copy Espressif's `examples/system/ulp/lp_core/lp_i2c`.
2. Build with ESP-IDF `v5.5.4` for `esp32c6`:

   ```sh
   idf.py set-target esp32c6
   idf.py build
   ```

3. Copy `build/lp_i2c_example.elf` into the Wokwi example's `firmware/`
   directory and point `wokwi.toml` at that ELF.
4. Run the same `lp.test.yaml` scenario.

Observed:

- The firmware repeatedly prints:
  - `LP I2C initialized successfully`
  - `LP core loaded with firmware successfully`
  - `Entering deep sleep...`
- The board repeatedly resets with `rst:0x8 (TG1_WDT_HPSYS)`.
- No LP wakeup / lux output appears.
- The scenario times out.

Expected:

- The rebuilt official ESP-IDF example should complete, or there should be a
  documented supported ESP-IDF version range for the current Wokwi LP model.

## Repro 3: minimal HP_CPU-started LP shared-variable example

Steps:

1. Copy Espressif's `examples/system/ulp/lp_core/build_system`.
2. Build with ESP-IDF `v5.5.4` for `esp32c6`.
3. Run the ELF in a minimal Wokwi project with only `board-esp32-c6-devkitc-1`.
4. Scenario waits for:

   ```text
   Sum calculated by ULP using external library func: 11
   ```

Observed:

- The HP task waits forever for the LP-written exported variable to change.
- The task watchdog fires while `main` is running:
  - `E (...) task_wdt: Task watchdog got triggered.`
  - `CPU 0: main`
- The scenario times out.

Expected:

- The LP program should run after `ulp_lp_core_run()` and update the exported
  shared variable.

## Repro 4: LP-UART register update bit

I also ran an HP-only register probe against the LP-UART register block:

- `DR_REG_LP_UART_BASE=0x600b1400`
- `LP_UART_DATE` read as `0x02201260`
- `LP_UART_ID` read as `0x00000500`
- FIFO/status registers returned plausible values.
- `LP_UART_REG_UPDATE` initially read `0`.
- After writing `LP_UART_REG_UPDATE`, it read `1`.
- After 1000 polling iterations with short delays, it still read `1`.

Observed:

```text
LP_UART_REG_UPDATE_BEFORE addr=0x600b1498 value=0x00000000
LP_UART_REG_UPDATE_AFTER_SET=0x00000001
LP_UART_REG_UPDATE_FINAL=0x00000001 CLEAR_ITER=-1
```

Expected:

The ESP32-C6 LP-UART register definition marks this bit as self-clearing after
hardware synchronization. ESP-IDF's LP-UART init waits for that clear, so the
bit should eventually return to `0` in simulation.

## Why this matters

I am trying to test an ESP32-C6 firmware that keeps a safety-critical RS-485
responder on the LP core while the HP core runs Wi-Fi/OTA. Wokwi is otherwise a
good fit for full-firmware integration with a custom chip, but current behavior
means LP-in-the-loop scenarios cannot be trusted. I can still run HP-fallback
tests in Wokwi, but LP execution has to be covered by a separate instruction-set
emulator and real hardware.
```
