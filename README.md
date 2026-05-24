# SupraMatic E2 ESPHome UAP1 Emulator

ESPHome external component and example configuration for connecting a Waveshare ESP32-S3-ETH board directly to a Hörmann SupraMatic E2 HCP1/BUS connector through a 3.3 V RS-485 transceiver.

This project targets the local path:

Waveshare ESP32-S3-ETH over Ethernet -> Home Assistant ESPHome native API -> Home Assistant HomeKit Bridge -> Apple Home

It does not implement HomeKit on the ESP32, does not use cloud services, does not require a physical Hörmann UAP1, and does not use a second MCU.

## Hardware Target

- Opener: Hörmann SupraMatic E2, Series 2 / HCP1-era.
- User unit: serial `REDACTED_SERIAL`; type plate suffix `Ei`, interpreted as index `E` plus integrated/internal receiver `i`.
- Bus route: plausible because index `E` is later than the usual UAP1/HCP1 compatibility threshold.
- MCU: one Waveshare ESP32-S3-ETH board only, powered by USB-C or the Waveshare IEEE 802.3af PoE module/PoE variant.
- Network: onboard W5500 Ethernet over SPI, DHCP by default.
- Bus interface: Waveshare TTL TO RS485 (C) galvanically isolated half-duplex adapter. Its official wiki documents 3.3 V to 5 V TTL power/signals, half-duplex RS-485, TTL `RXD/TXD/VCC/GND`, RS-485 `A+/B-/PE`, isolation, and a selectable 120 ohm terminator: <https://www.waveshare.com/wiki/TTL_TO_RS485_%28C%29>.

Do not connect ESP32 GPIO directly to the Hörmann bus. Do not connect the opener 24 V line directly to the ESP32. With PoE power, leave BUS pin 2 disconnected.

## Wiring

Use an RJ12 / 6P6C plug or breakout. Ordinary RJ11 telephone leads are often 6P2C or 6P4C and may not expose the bus pins.

Verify the connector orientation before applying power. RJ12 pin numbers are mirror-imaged depending on whether you are looking into the socket, at the plug contacts, or at the cable from the latch side. Use continuity testing from your exact breakout/cable before connecting BUS pin 2, and leave BUS pin 2 disconnected during USB/PoE-powered development.

Expected HCP1 BUS pinout:

| BUS pin | Function |
| --- | --- |
| 1 | Unused / unknown, leave open |
| 2 | +24 V DC, leave disconnected when powering by USB-C or PoE |
| 3 | BUS GND / RS-485 reference |
| 4 | Unused / unknown, leave open |
| 5 | RS-485 DATA- / B / D- |
| 6 | RS-485 DATA+ / A / D+ |

Suggested wiring for the Waveshare TTL TO RS485 (C) isolated adapter:

| ESP32-S3-ETH / adapter | Connection |
| --- | --- |
| 3V3 | Adapter VCC |
| ESP32 GND | Adapter TTL-side GND only |
| GPIO17 TX | Adapter RXD |
| GPIO16 RX | Adapter TXD |
| Adapter A+ | BUS pin 6 |
| Adapter B- | BUS pin 5 |
| Adapter PE | BUS pin 3 |

Do not tie ESP32 GND to BUS pin 3 when using the isolated adapter; use the adapter PE terminal as the RS-485-side reference. The adapter has no documented DE, /RE, or RTS pin, so the default YAML omits `rts_pin` and relies on the adapter's hardware auto-direction.

If you use a non-isolated bare transceiver such as MAX3485/SP3485/SN65HVD72 instead, use `GPIO18` for DE and /RE tied together, connect ESP32 GND to BUS pin 3, and add this to `uapbridge_esp`:

```yaml
rts_pin: GPIO18
```

For first tests, power the board from USB-C or IEEE 802.3af PoE and connect only PE, DATA-, and DATA+ to the opener. If no valid traffic appears, swap A/B once as a diagnostic step because some RS-485 modules label them inconsistently.

Choose exactly one board power source: USB-C, the Waveshare 802.3af PoE module/PoE variant, or a properly regulated and isolated 24 V to 5 V supply. For this PoE setup, do not use the opener's 24 V BUS pin.

Do not use GPIO9, GPIO10, GPIO11, GPIO12, GPIO13, or GPIO14 for the RS-485 adapter; those pins are occupied by the onboard W5500 Ethernet chip. Avoid GPIO4 through GPIO7 if you may use the TF-card slot, avoid GPIO19/GPIO20 because they are USB D-/D+, avoid GPIO21 because it drives the onboard RGB LED, and avoid strapping pins such as GPIO0/GPIO45/GPIO46 unless you explicitly account for boot behavior. If you switch from the isolated adapter to a bare DE/RE transceiver, `GPIO18` is the documented direction pin when no camera is attached.

The Waveshare TTL TO RS485 (C) includes an onboard 120 ohm termination option. For first tests, set that termination to `NC`/off. With all power disconnected, measure A/B resistance on the connected bus and enable termination only if this adapter is physically at an unterminated end of the RS-485 segment.

The ESPHome Ethernet configuration uses the Waveshare/ESPHome W5500 pin map:

| W5500 signal | ESP32-S3 GPIO |
| --- | --- |
| SCLK | GPIO13 |
| MOSI | GPIO11 |
| MISO | GPIO12 |
| CS | GPIO14 |
| INT | GPIO10 |
| RST | GPIO9 |

## Software

The component is vendored under [components](components). It is based on the ESP32-capable ESPHome port of `hoermann_door`, with local safety and diagnostic changes:

- ESP-IDF example config for the Waveshare ESP32-S3-ETH board.
- Ethernet-only networking through the onboard W5500. Wi-Fi is intentionally not configured because ESPHome does not allow Wi-Fi and Ethernet at the same time.
- Optional configurable RS-485 direction pin through `rts_pin` for bare DE/RE transceivers. Omit it for the Waveshare TTL TO RS485 (C) isolated auto-direction adapter.
- `allow_remote_close`, default `false`, blocks explicit close commands during development.
- `allow_remote_impulse`, default `false`, blocks the generic impulse command because impulse can close an open door.
- `use_unverified_stop_command`, default `false`, avoids the E3-derived raw stop word and uses an impulse fallback only while the door is decoded as moving.
- `require_fresh_broadcast_for_commands`, default `true`, blocks commands until a fresh CRC-valid HCP broadcast has been seen.
- `command_timeout`, default `1200ms`, expires queued one-shot commands if the drive does not poll.
- `diagnostic_mode`, logs raw frames, CRC status, decoded status bits, command queueing, command sending, and state transitions.
- `listen_only`, when set true, receives and logs HCP frames without answering scan/status requests. Use this only for first bus capture.
- `valid_broadcast_timeout`, default `10s`, clears the valid-broadcast diagnostic and marks state unknown if the bus goes stale.
- `got_valid_broadcast` is set only after a CRC-valid HCP broadcast.
- Unknown states are treated conservatively; the cover is never reported closed unless the closed bit is decoded. The cover does not advertise toggle or arbitrary position support.

Primary config: [supramatic-e2.yaml](supramatic-e2.yaml)

Minimal fallback config: [supramatic-e2-minimal.yaml](supramatic-e2-minimal.yaml)

Proxy-only config: [supramatic-e2-proxy.yaml](supramatic-e2-proxy.yaml)

`secrets.yaml` needs these keys:

```yaml
api_key_supramatic_e2: "..."
proxy_auth_token: "..." # only needed when proxy allow_tx is enabled
```

[secrets.example.yaml](secrets.example.yaml) contains a syntactically valid placeholder for ESPHome validation. Generate a real ESPHome API encryption key before flashing hardware you will actually use.

## Development Defaults

The primary YAML intentionally starts with:

```yaml
uapbridge_esp:
  diagnostic_mode: true
  listen_only: false
  allow_remote_close: false
  allow_remote_impulse: false
  use_unverified_stop_command: false
  require_fresh_broadcast_for_commands: true
  auto_correction: false
```

This allows receive, scan response, state decoding, open, stop, light, and venting tests while blocking remote close and the generic impulse command. After state decoding and physical safety behavior are verified at the door, set `allow_remote_close: true` to enable Home Assistant close commands. Only set `allow_remote_impulse: true` for deliberate protocol testing while physically present. Even then, the firmware requires a fresh valid HCP broadcast, a known non-stopped state, and no active error/prewarn before it accepts movement commands.

Keep `auto_correction: false` for the SupraMatic E2 until the raw E2 state mapping is known. It is retained only as an upstream compatibility option and should not be used as a substitute for understanding an error or prewarn state.

## Proxy Mode

Flash [supramatic-e2-proxy.yaml](supramatic-e2-proxy.yaml) when you want the ESP32 to act only as an Ethernet proxy for the RS-485 bus. This mode does not load the UAP1 emulator and does not expose garage entities to Home Assistant.

Proxy mode listens on TCP port `6638` and streams line-based events such as `RX <seq> <micros> <hex>` and `GAP <micros> <gap-us>`. It defaults to receive-only. Active transmission requires `allow_tx: true` and `auth_token: !secret proxy_auth_token`; `TXB <hex>` generates the HCP sync break before writing bytes.

Use the helper client:

```bash
python3 tools/hcp_proxy_client.py --host supramatic-e2-proxy.local
```

See [docs/proxy-mode.md](docs/proxy-mode.md) for the full TCP protocol. Passive capture is the primary use. Fully live laptop-side UAP1 emulation over TCP may miss the opener's poll response window, so keep timing-critical status response logic on the ESP32 unless measurements prove otherwise.

## Test Checklist

1. Flash ESP32 and verify it boots in ESPHome/Home Assistant before connecting to the opener.
2. Connect Ethernet and verify the device comes online via DHCP.
3. Connect PE, DATA-, DATA+ only; power the board via USB-C or PoE for the first bus test.
4. Open ESPHome logs.
5. Optional first capture: set `listen_only: true`, flash, power-cycle the opener, and confirm raw broadcasts before allowing the ESP32 to answer the bus. Then set `listen_only: false` and flash again for control tests.
6. Power-cycle the SupraMatic opener.
7. Confirm broadcast frames are received.
8. Confirm `Valid HCP Broadcast` becomes true.
9. Confirm slave scan and status request frames are detected.
10. Confirm the emulator responds as UAP1 address `0x28`.
11. Test the light command first if the opener supports it.
12. Test stop while the door is already moving and while physically present at the door.
13. Enable `allow_remote_close: true` only after state feedback is reliable and safety devices are verified.
14. Test open and close while physically present at the door.
15. Enable and test `allow_remote_impulse: true` only if you explicitly need the generic impulse command for protocol diagnosis.
16. Log and verify states for closed, opening, open, closing, stopped halfway, venting/partial-open, light on/off, and any error/prewarn state.
17. Only after reliable state and safety behavior, expose the cover through Home Assistant's HomeKit Bridge.

## Capturing E2 State Logs

Use:

```bash
esphome logs supramatic-e2.yaml
```

With `diagnostic_mode: true`, capture logs while moving the door through:

- Fully closed.
- Opening.
- Fully open.
- Closing.
- Stopped halfway.
- Venting / partial-open.
- Light on and off.
- Error or prewarn, if available.

For each state, copy the lines containing:

- `broadcast crc=ok data=...`
- `Decoded status 0x....`
- `State changed`
- `Queued one-shot HCP command`
- `Sending one-shot HCP command`

If E2 state decoding differs from the E3-derived mapping, those raw status frames are the data needed to adjust the bit mapping without changing the hardware.

When configuring HomeKit Bridge, include only the garage cover and optionally the garage light. Exclude venting controls, diagnostics, raw state helpers, and error/prewarn sensors from HomeKit. The default YAML does not expose a generic impulse button; add one only for deliberate protocol diagnosis.

## Proxy Mode

Use [supramatic-e2-proxy.yaml](supramatic-e2-proxy.yaml) when you want the ESP32 to act only as an encrypted Ethernet-to-RS485 proxy. It uses ESPHome `serial_proxy`, omits the UAP1 emulator and all garage-door entities, and is intended for laptop-side protocol capture/debugging with [tools/hcp1_proxy_client.py](tools/hcp1_proxy_client.py).

Full notes are in [docs/proxy-mode.md](docs/proxy-mode.md).

## Notes

- The HCP1 UART is `19200 8N1`.
- The drive is the master; the ESP32 behaves as a UAP1 slave at address `0x28`.
- Known protocol values used here include master address `0x80`, broadcast `0x00`, scan command `0x01`, status request `0x20`, status response `0x29`, and CRC-8 polynomial `0x07` with initial value `0xF3`.
- Command responses keep the expected normal bit `0x1000` set and send command bits once on the next status response.
- This is not a Series 4/HCP2 bridge and does not target UAP1-HCP.

## Upstream

The ESPHome component was initially derived from the MIT-licensed `14yannick/hoermann_door` ESPHome UAP1 emulator. The `Tysonpower/hoermann_door` fork was checked as well for current ESPHome compatibility fixes. The HCP2 `HCPBridgeMqtt` family was checked only to confirm it targets different Series 4 / UAP1-HCP use cases and is not the basis for this E2/HCP1 implementation.
