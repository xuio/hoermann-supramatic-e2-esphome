# Notice

This project is released under The Unlicense. It also contains code and protocol knowledge derived from public reverse-engineering work.

## Upstream References

- `hoermann_door` ESPHome/UAP1 implementations, especially ESP32-compatible forks used as the starting point for the local ESPHome external component.
- Other public HCP/Hörmann protocol notes and experiments used only as protocol references.
- HCP2 Series-4 state/light behavior cross-checks from
  `14yannick/esphome-hcpbridge` at commit
  `47823652491e2465a9b7a8f897bd1bcd01a3f43f`. The repository is used only as
  a protocol-behavior reference; no implementation code is copied.
- HCP2 / UAP1-HCP protocol facts and public capture fixtures from
  `Tysonpower/HCPBridgeMqtt_tynet` at commit
  `e2d8297a868d4dd62781b8b4a305279b787d250c`, file
  `docs/Investigation.zip` (SHA-256
  `9130bf43f433d95bf9dc32f1449b854355e5fd5a94c63b0d79b54e556eeb1567`).
  The canonical original investigation is `hkiam/HCPBridge` under `Investigation/`.
  The repository uses only protocol facts and capture bytes from this corpus, not
  GPL-family implementation code.
- HCP2 Series-4 ID 1 behavior, 1..127 counter wrapping, and movement/light status
  vectors from a user-provided decoded official Hörmann accessory capture
  (`log_2026-06-18_20-22-23.log`). Only curated frame bytes and derived protocol facts
  are committed; the full local capture is not included.

The original upstream license text kept from the `hoermann_door` lineage is in [LICENSE.upstream-hoermann_door](LICENSE.upstream-hoermann_door).

## Trademark Notice

Hörmann, SupraMatic, UAP1, Home Assistant, ESPHome, Apple Home, and HomeKit are trademarks or product names of their respective owners. This project is unofficial and is not endorsed by Hörmann, Home Assistant, Apple, Waveshare, or Espressif.

## Protocol Notice

The HCP1/UAP1 behavior documented here is based on observed traffic and public reverse-engineering. It may be incomplete or wrong for other opener models, firmware indexes, or regional variants.

The HCP2 / UAP1-HCP behavior under development is likewise based on observed public
traffic and must be validated in simulation, hardware-in-the-loop, and finally
listen-only real-motor captures before active motor control.
