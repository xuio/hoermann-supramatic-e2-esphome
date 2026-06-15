# HCP2 LP Core Firmware

This plain ESP-IDF project builds the ESP32-C6 LP-core responder blob used by
Phase 0c. It embeds the portable `components/hcp2bridge/core` protocol engine
and exposes only the fixed mailbox ABI at `0x50002400` to the HP side.

Local build:

```sh
env -u IDF_PYTHON_ENV_PATH bash -c 'source /Users/moritz/.platformio/packages/framework-espidf/export.sh && idf.py -C firmware/hcp2-lp build'
```

Important limits:

- GPIO4 is LP-UART RX and GPIO5 is LP-UART TX.
- GPIO0 is the bench DE pin; GPIO1 is /RE.
- The LP disables the transceiver receiver only during local TX: DE is asserted
  first, then /RE is driven high; at the end of TX, /RE is driven low before DE
  is released. The LP port ignores local echo/noise during TX and flushes stale
  RX bytes before returning to receive parsing.
- The LP project reserves 16,320 bytes of LP SRAM. The mailbox occupies a fixed
  1280-byte window at `0x50002400`; the ISS map checker asserts linked LP
  sections stay below it and the stack keeps headroom above it.
- The LP loop sleeps for 75 us between iterations. The HCP2 engine schedules
  status responses 4.2 ms after a valid poll, which stays inside the observed
  OEM 3.97-5.92 ms response window while keeping margin above the fastest trace.
- The HP app only reloads the LP blob when the mailbox ABI/version is wrong or
  live heartbeat proves the LP responder is stale. Poll counters are diagnostics;
  an isolated missed poll must never trigger an HP-driven LP reload.

The mailbox ABI is currently version 7. In addition to the state seqlock and
command epoch/sequence fields, it publishes:

- command deadlines and an ack result (`executed`, `expired`, `unknown`,
  `busy`) so HP resets cannot replay stale commands silently.
- an armed stop-trigger block (`epoch`, target position, hard TTL) so the LP can
  press stop once if the HP dies during a goto-position move.
- LP time, status polls seen/answered, last-poll time, CRC/RX errors, TX aborts,
  collisions, maximum DE hold time, LP reset count, stop-trigger fire count,
  loop/RX health counters, LP-side response timing maxima, a protocol-event
  ring for HP RAM packet logging, and trace events for HIL/logic-analyzer
  correlation.

The LP UART port treats DE as a bounded safety signal. It loops until all bytes
are accepted by the 16-byte FIFO, holds DE for the full 57600 8E1 frame time
plus margin, then checks TX idle. It aborts only if the FIFO wedges or DE would
exceed the deadman budget.
