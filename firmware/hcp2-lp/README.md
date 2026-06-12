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
  the heartbeat is stale. A matching magic word alone is not treated as healthy.
