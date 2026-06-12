# HCP2 LP Core Firmware

This plain ESP-IDF project builds the ESP32-C6 LP-core responder blob used by
Phase 0c. It embeds the portable `components/hcp2bridge/core` protocol engine
and exposes only the fixed mailbox ABI at `0x50002000` to the HP side.

Local build:

```sh
env -u IDF_PYTHON_ENV_PATH bash -c 'source /Users/moritz/.platformio/packages/framework-espidf/export.sh && idf.py -C firmware/hcp2-lp build'
```

Important limits:

- GPIO4 is LP-UART RX and GPIO5 is LP-UART TX.
- GPIO2 is the bench DE pin; GPIO3 is /RE and is held low so the
  transceiver's receiver remains enabled during TX.
- Because /RE remains low, the LP-UART can receive its own transmitted response.
  The LP port drains and byte-checks this local echo while DE is asserted, records
  `rx-echo` trace events, and does not feed echoed bytes back into the HCP2 parser.
- The LP project reserves 16,320 bytes of LP SRAM. The mailbox occupies a fixed
  512-byte window at `0x50002000`; the ISS map checker asserts linked LP
  sections stay below it and the stack keeps headroom above it.
- The HP app only reloads the LP blob when the mailbox ABI/version is wrong or
  live health proves the LP responder is stale. A matching magic word alone is
  not treated as healthy.

The mailbox ABI is currently version 2. In addition to the state seqlock and
command epoch/sequence fields, it publishes:

- command deadlines and an ack result (`executed`, `expired`, `unknown`,
  `busy`) so HP resets cannot replay stale commands silently.
- LP time, status polls seen, and status polls answered so the HP supervisor can
  distinguish a live heartbeat from a responder that is no longer servicing the
  bus.
- TX aborts, echo collisions, maximum DE hold time, LP reset count, and trace
  events for HIL/logic-analyzer correlation.

The LP UART port treats DE as a bounded safety signal. It loops until all bytes
are accepted by the 16-byte FIFO, waits for flush, drains the local RX echo while
DE is high, and aborts the transmission if the FIFO wedges, the echo mismatches,
or DE would exceed the deadman budget.
