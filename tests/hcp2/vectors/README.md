# HCP2 Test Vectors

This directory contains a minimal committed subset of HCP2 / UAP1-HCP frames used by
the Phase 0a host tests. The full public capture corpus is intentionally not committed;
use `uv run garage-fetch-hcp2-reference` to download and normalize it into the
gitignored `captures/hcp2/` working directory.

Provenance:

- Upstream repository: `Tysonpower/HCPBridgeMqtt_tynet`
- Pinned commit: `e2d8297a868d4dd62781b8b4a305279b787d250c`
- Pinned file: `docs/Investigation.zip`
- SHA-256: `9130bf43f433d95bf9dc32f1449b854355e5fd5a94c63b0d79b54e556eeb1567`
- Original corpus path inside the zip: `Investigation/records/dump.zip` -> `dump2.txt`
- Canonical original investigation repository: `hkiam/HCPBridge`

The bytes here are used as protocol facts and captures only. Do not copy GPL-family
reference implementation code into this repository.

Fixture coverage:

- bus scan request and signature response
- steady-state status poll and idle status response
- light on/off command requests and responses
- virtual button press/release encodings for open, close, stop, vent, half-open, and light
- representative broadcast status frames for light state and door open/close ramps
