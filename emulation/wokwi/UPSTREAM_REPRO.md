# Wokwi ESP32-C6 LP Repro Notes

## 2026-06-11 retest update

Retest environment:

- Wokwi CLI: `0.26.1 (9d71b975b7eb)`
- Wokwi Simulation API: `1.0.0-20260611-g7cf12162`
- Host: macOS

The original LP-execution issue appears fixed in this backend:

- The unmodified official `wokwi/esp32c6-i2c-lp` project now repeatedly prints
  `LP core woke up the main CPU` and `Lux = ...` after entering deep sleep.
  The local CLI run still timed out because the example kept looping, but the
  missing LP wakeup signal is no longer missing.
- This project's no-bypass `firmware/hcp2-bringup` LP-in-the-loop build now
  reaches `ulp_lp_core_load_binary()` / `ulp_lp_core_run()`, advances the real
  LP mailbox heartbeat, passes the HP supervisor skip-reload health check,
  rejects a stale command epoch, and acks a fresh command:

  ```text
  HCP2_SUPERVISOR_REAL_SKIP_RELOAD_OK
  HCP2_SUPERVISOR_REAL_EPOCH_OK
  HCP2_SUPERVISOR_REAL_MAILBOX_OK
  ```

One native Wokwi LP-UART gap remains: ESP-IDF LP-UART initializes, but LP-UART
RX/TX traffic still does not exchange bytes with external custom-chip pins. We
are deliberately not adding a GPIO bit-banged UART workaround; the remaining
failure was reported upstream as native LP-UART simulator fidelity.

## 2026-06-12 LP-UART retest update

Retest environment:

- Wokwi CLI: `0.26.1 (9d71b975b7eb)`
- Wokwi Simulation API: `1.0.0-20260612-g320c8a79`
- Host: macOS

The backend behavior changed, but the native LP-UART path is not green yet:

- HP-UART positive control still passes with the same custom-chip UART crossing
  on GPIO4/GPIO5:

  ```text
  HP_UART_ECHO_CONTROL_PASS
  OFFICIAL_LP_UART_ECHO_OK reason=echo-received rx_count=1
  ```

- The 9600-baud LP-UART RX-only smoke is no longer completely dead: the LP
  mailbox `rx_count` advances. However, it is not byte-correct. The custom chip
  repeatedly sends `0x55`, while the LP app reports `last_rx=0xce`.

  ```text
  BASIC_UART_TX byte=0x55 count=8
  LP_RX9600_STATUS ... rx_count=1 last_rx=0xce ...
  LP_RX9600_PASS rx_count=1 last_rx=0xce
  ```

- The 57600-baud minimal LP-UART RX/TX repro still fails. The LP app receives no
  bytes from the custom-chip probe, and the custom chip receives no intentional
  echo response:

  ```text
  LP_UART_MASTER_TX raw=55aa00ff33cc
  LP_UART_REPRO_FAIL reason=no-response rx_count=1
  LP_UART_REPRO_FAIL reason=rx-empty uart_rx=0
  ```

- This repo's real HCP2 LP-in-loop Wokwi fixture still fails with the same bus
  scan timeout:

  ```text
  HCP2_SUPERVISOR_REAL_MAILBOX_OK
  HCP2_TRACE {"source":"wokwi","type":"master_frame","raw":"02179cb900059c41000306000200000102f835"}
  HCP2_WOKWI_VERDICT_FAIL reason=scan-timeout polls=0 replies=0
  ```

Conclusion: the 2026-06-12 backend appears to have partial LP-UART progress, but
not enough for HCP2. Keep the native LP-UART Wokwi gate non-blocking until the
57600 repro and the real HCP2 steady-state scenario both pass.

## 2026-06-12 LP-UART fix retest update

Retest environment:

- Wokwi CLI: `0.26.1 (9d71b975b7eb)`
- Wokwi Simulation API: `1.0.0-20260612-g17cab5dd`
- Host: macOS

The native ESP32-C6 LP-UART custom-chip pin path now passes the repros that
previously failed:

- Official ESP-IDF `lp_uart_echo` repro passes at both 9600 and 57600 baud. At
  57600 there is still one pre-probe `0xff` byte from the custom-chip side, but
  the actual `0x55` probe is echoed back correctly.
- Minimal 9600 RX-only repro is byte-correct: `last_rx=0x55`.
- Minimal 57600 RX/TX repro emits `LP_UART_REPRO_OK`.
- HCP2 scan repro receives the exact expected response:
  `02170a00000205043010ffa8550f13`.
- Real HCP2 core repro emits `HCP2_CORE_REPRO_OK tx_len=15`.
- Repo-level real LP-in-loop steady-state run passes:

  ```text
  HCP2_WOKWI_VERDICT_OK reason=steady-state polls=200 replies=200 misses=0 max_consecutive_misses=0 latency_min_us=12869 latency_max_us=12930
  ```

Conclusion: Wokwi native LP-UART is green enough to become the primary cloud
full-firmware pre-HIL gate again. HIL remains the authority for 57600 8E1 parity,
RS-485 electrical timing, line contention, transceiver DE behavior, reset
glitches, and silicon reset behavior.

## Submitted follow-up: native ESP32-C6 LP-UART pin traffic still broken

Status: **submitted by the user on 2026-06-12; fixed in Simulation API
`1.0.0-20260612-g17cab5dd` by local retest.** The original text is kept here for
provenance.

Local repro/control workspaces used during the retest:

- `emulation/wokwi/esp32c6-lp-uart-official-echo-repro/`: current upstream
  attachment repro. It packages the official ESP-IDF LP-UART echo example with
  two prebuilt Wokwi modes, 9600 and 57600 baud. The zip intentionally contains
  no Markdown files.
- `emulation/wokwi/esp32c6-lp-uart-hcp2-core-repro/`: self-contained,
  local/debug repro using the real `hcp2_core` sources and the production LP
  responder loop behind a tiny HP loader/status harness. On Simulation API
  `1.0.0-20260612-g17cab5dd`, it passes with `HCP2_CORE_REPRO_OK tx_len=15`.
- `emulation/wokwi/esp32c6-lp-uart-hcp2-scan-repro/`: self-contained, prebuilt
  HCP2-like LP-UART bus-scan repro for sharing with Wokwi maintainers.
- `emulation/wokwi/esp32c6-lp-uart-min-repro/`: minimal native LP-UART RX/TX repro.
- `emulation/wokwi/esp32c6-lp-uart-rx9600-min-repro/`: minimal 9600-baud LP-UART RX-only repro.
- `emulation/wokwi/esp32c6-hp-uart-echo-control/`: HP-UART positive control using the same custom-chip UART and GPIO4/GPIO5 crossing.

These workspaces and their zip archives are local issue-attachment artifacts and
are ignored by git. `emulation/wokwi/esp32c6-lp-uart-official-echo-repro.zip` was
the current attachment artifact for the 2026-06-12 issue; keep generated zips
uncommitted unless a different attachment needs to be regenerated.

````markdown
Thanks for the quick fix. I retested with Simulation API
`1.0.0-20260611-g7cf12162`, and the LP core execution issue looks fixed here:

- the official `wokwi/esp32c6-i2c-lp` example now wakes the HP core and prints
  lux values;
- an ESP-IDF 5.5 LP test firmware now loads/runs an LP binary;
- LP heartbeat/shared-memory checks work.

One thing still looks broken, probably separately: native ESP32-C6 LP-UART pin
traffic does not seem to reach external Wokwi pins / custom chips.

I can reproduce this with Espressif's unmodified official ESP-IDF example:

```text
examples/system/ulp/lp_core/lp_uart/lp_uart_echo
```

Setup:

- target: `board-esp32-c6-devkitc-1`
- example: official `lp_uart_echo`, unchanged
- ESP-IDF: 5.5
- LP-UART pins: the example defaults for ESP32-C6, `GPIO4` RX and `GPIO5` TX
- baud: the example default, 115200 8N1
- Wokwi custom chip `TX` -> ESP32-C6 `GPIO4`
- ESP32-C6 `GPIO5` -> Wokwi custom chip `RX`

Run:

```sh
cp -R "$IDF_PATH/examples/system/ulp/lp_core/lp_uart/lp_uart_echo" /tmp/lp_uart_echo
cd /tmp/lp_uart_echo
idf.py set-target esp32c6 build
```

Custom chip definition used for the Wokwi side:

`chips/official-echo-master.chip.json`

```json
{
  "name": "Official echo master",
  "author": "official-echo-test",
  "pins": ["RX", "TX", "PASS", "FAIL", "GND"],
  "controls": [
    {
      "id": "startDelayUs",
      "label": "Start delay us",
      "type": "range",
      "min": 100000,
      "max": 5000000,
      "step": 100000
    },
    {
      "id": "responseTimeoutUs",
      "label": "Response timeout us",
      "type": "range",
      "min": 10000,
      "max": 2000000,
      "step": 10000
    }
  ]
}
```

`official-echo-master.chip.c`

```c
#include "wokwi-api.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ECHO_BAUD 115200
#define ECHO_BYTE 0x55u

typedef enum {
  PHASE_START = 0,
  PHASE_WAIT_ECHO,
  PHASE_DONE,
  PHASE_FAILED,
} phase_t;

typedef struct {
  uart_dev_t uart;
  timer_t timer;
  pin_t pass_pin;
  pin_t fail_pin;
  uint32_t start_delay_attr;
  uint32_t response_timeout_attr;
  phase_t phase;
  uint32_t rx_count;
  bool verdict_printed;
} chip_state_t;

static uint64_t now_us(void) {
  return get_sim_nanos() / 1000u;
}

static void verdict(chip_state_t *chip, bool ok, const char *reason) {
  if (chip->verdict_printed) {
    return;
  }
  chip->verdict_printed = true;
  chip->phase = ok ? PHASE_DONE : PHASE_FAILED;
  pin_write(chip->pass_pin, ok ? HIGH : LOW);
  pin_write(chip->fail_pin, ok ? LOW : HIGH);
  printf("OFFICIAL_LP_UART_ECHO_%s reason=%s rx_count=%" PRIu32 " t_us=%" PRIu64 "\n",
         ok ? "OK" : "FAIL", reason, chip->rx_count, now_us());
}

static void on_uart_rx(void *user, uint8_t byte) {
  chip_state_t *chip = (chip_state_t *) user;

  chip->rx_count++;
  printf("OFFICIAL_ECHO_MASTER_RX byte=0x%02x count=%" PRIu32 " t_us=%" PRIu64 "\n",
         byte, chip->rx_count, now_us());
  if (chip->phase == PHASE_WAIT_ECHO && byte == ECHO_BYTE) {
    verdict(chip, true, "echo-received");
  }
}

static void send_probe(chip_state_t *chip) {
  uint8_t byte = ECHO_BYTE;

  chip->phase = PHASE_WAIT_ECHO;
  printf("OFFICIAL_ECHO_MASTER_TX byte=0x%02x t_us=%" PRIu64 "\n", byte, now_us());
  (void) uart_write(chip->uart, &byte, 1u);
  timer_start(chip->timer, attr_read(chip->response_timeout_attr), false);
}

static void on_timer(void *user) {
  chip_state_t *chip = (chip_state_t *) user;

  if (chip->phase == PHASE_START) {
    send_probe(chip);
    return;
  }
  if (chip->phase == PHASE_WAIT_ECHO) {
    verdict(chip, false, "no-echo");
  }
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));

  chip->phase = PHASE_START;
  chip->rx_count = 0;
  chip->verdict_printed = false;
  chip->start_delay_attr = attr_init("startDelayUs", 2500000u);
  chip->response_timeout_attr = attr_init("responseTimeoutUs", 500000u);
  chip->pass_pin = pin_init("PASS", OUTPUT_LOW);
  chip->fail_pin = pin_init("FAIL", OUTPUT_LOW);

  uart_config_t uart_config = {
      .tx = pin_init("TX", INPUT),
      .rx = pin_init("RX", INPUT_PULLUP),
      .baud_rate = ECHO_BAUD,
      .rx_data = on_uart_rx,
      .write_done = NULL,
      .user_data = chip,
  };
  chip->uart = uart_init(&uart_config);
  chip->timer = timer_init(&(timer_config_t) {
      .callback = on_timer,
      .user_data = chip,
  });
  timer_start(chip->timer, attr_read(chip->start_delay_attr), false);
  printf("OFFICIAL_ECHO_MASTER_READY baud=%u byte=0x%02x tx_to_gpio4 rx_from_gpio5\n",
         ECHO_BAUD, ECHO_BYTE);
}
```

Compile it with:

```sh
wokwi-cli chip compile official-echo-master.chip.c -o chips/official-echo-master.chip.wasm
```

Then run that ELF in Wokwi with the custom chip wired as above. The custom chip
sends one byte (`0x55`) into GPIO4 at 115200 after `startDelayUs`, and waits
for the echo on GPIO5.

The official example starts cleanly:

```text
Not an LP core wakeup. Cause = 0
Initializing...
LP UART initialized successfully
LP core loaded with firmware and running successfully
Entering deep sleep...
```

Observed: no echo is received from GPIO5 after the custom chip sends `0x55`.

```text
OFFICIAL_ECHO_MASTER_TX byte=0x55
OFFICIAL_LP_UART_ECHO_FAIL reason=no-echo rx_count=0
```

Control check: same custom chip, same Wokwi diagram wiring, same baud, same
GPIO4/GPIO5 pins, but with HP UART (`UART_NUM_1`) echoing GPIO4 RX back out
GPIO5 TX, works:

```text
HP_UART_ECHO_CONTROL_READY rx_gpio=4 tx_gpio=5 baud=115200 parity=none
OFFICIAL_ECHO_MASTER_TX byte=0x55
OFFICIAL_ECHO_MASTER_RX byte=0x55
OFFICIAL_LP_UART_ECHO_OK reason=echo-received rx_count=1
HP_UART_ECHO_CONTROL_PASS
```

So the custom chip UART, Wokwi wire, GPIO4 input path, and GPIO5 output path
all work for the normal ESP32-C6 UART. The failure only appears when the same
external path talks to the native LP-UART. I also diffed the ESP-IDF example
sources used in the LP-UART run against the installed official example; there
were no source changes, only Wokwi wrapper/generated files.

Expected: the official LP-UART echo example should read the byte from GPIO4 and
echo it back on GPIO5.

This looks like an LP-UART peripheral / pin-routing / custom-chip boundary issue
rather than the LP-core execution issue from the original report.
````

The original draft below is kept for provenance of the fixed issue.

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
