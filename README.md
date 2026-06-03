# Hörmann SupraMatic E2 ESPHome UAP1 Emulator

[![CI](https://github.com/xuio/hoermann-supramatic-e2-esphome/actions/workflows/ci.yml/badge.svg)](https://github.com/xuio/hoermann-supramatic-e2-esphome/actions/workflows/ci.yml)
[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](LICENSE)

ESPHome firmware for a single ESP32 that connects directly to a Hörmann SupraMatic E2 HCP1/BUS connector through an RS-485 adapter, emulates a UAP1 accessory, and exposes the garage door to Home Assistant.

The intended path is:

```text
Hörmann SupraMatic E2 HCP1/BUS
  -> RS-485 adapter
  -> Waveshare ESP32-S3-ETH with PoE
  -> ESPHome native API
  -> Home Assistant
  -> HomeKit Bridge
  -> Apple Home
```

This project does not use a physical Hörmann UAP1 module, does not require a second MCU, does not implement HomeKit on the ESP32, and does not use cloud services.

## Status

This firmware is tested on one SupraMatic E2 installation with a Waveshare ESP32-S3-ETH board, Waveshare IEEE 802.3af PoE power, and a Waveshare TTL TO RS485 (C) isolated adapter. It should be treated as a reverse-engineered integration for careful builders, not as an official Hörmann product.

Supported and tested in Home Assistant:

- Garage-door cover with `device_class: garage`
- Open, close, stop, light toggle, and optional vent/impulse controls
- Open/closed/moving state from HCP status
- Time-based position estimation with calibrated motion curves
- Obstruction/close-failure latch when a close does not reach the HCP closed bit
- HTTP debug stream and PSRAM protocol capture for troubleshooting
- OTA updates over Ethernet

## Safety

Garage doors can injure people and damage property. Build and test this only while physically present at the door. Do not enable remote closing until state feedback, obstruction protection, and your installation's safety hardware have been verified. Read [docs/safety.md](docs/safety.md) before wiring or flashing.

## Hardware

Known working hardware:

- Hörmann SupraMatic E2, Series 2 / HCP1-era opener
- Waveshare ESP32-S3-ETH board with IEEE 802.3af PoE power
- Waveshare TTL TO RS485 (C) isolated half-duplex adapter
- RJ12 / 6P6C cable or breakout for the opener BUS socket

Do not use a normal RJ11 telephone cable unless you have verified it has all six contacts and the needed conductors.

See [docs/hardware-wiring.md](docs/hardware-wiring.md) for the full pinout, cable color example, termination notes, and adapter wiring.

## Quick Start

Install `uv`, then build with the pinned Python and ESPHome versions:

```bash
git clone https://github.com/xuio/hoermann-supramatic-e2-esphome.git
cd hoermann-supramatic-e2-esphome
uv sync
cp configs/secrets.example.yaml configs/secrets.yaml
```

Edit `configs/secrets.yaml` and set a real ESPHome API encryption key and OTA password:

```yaml
api_key_supramatic_e2: "replace-with-a-real-esphome-api-key"
ota_password_supramatic_e2: "replace-with-a-long-random-ota-password"
proxy_auth_token: "replace-with-a-long-random-token"
```

Validate and compile:

```bash
uv run esphome config configs/supramatic-e2.yaml
uv run esphome compile configs/supramatic-e2.yaml
```

First flash is normally over USB-C. For conservative first bus bring-up, flash `configs/supramatic-e2-minimal.yaml`; it exposes state diagnostics while keeping remote close, impulse, and unverified stop disabled. The full `configs/supramatic-e2.yaml` is the tested working setup after state and safety behavior have been verified.

```bash
uv run esphome run configs/supramatic-e2.yaml
```

After the first flash, update over Ethernet:

```bash
uv run esphome upload configs/supramatic-e2.yaml --device supramatic-e2.local
```

See [docs/getting-started.md](docs/getting-started.md) and [docs/flashing-ota.md](docs/flashing-ota.md) for the complete setup flow.

## Repository Layout

```text
components/             ESPHome external components
configs/                ESPHome YAML configurations and secrets example
docs/                   User, protocol, debugging, and safety documentation
docs/markers/           Printable ArUco marker PDFs for calibration
docs/research/analysis/ Calibration and reverse-engineering artifacts
tools/                  Python helper tools for captures and calibration
.github/workflows/      CI validation
```

Primary files:

- [configs/supramatic-e2.yaml](configs/supramatic-e2.yaml): main Ethernet/PoE UAP1 emulator firmware
- [configs/supramatic-e2-minimal.yaml](configs/supramatic-e2-minimal.yaml): safer minimal bring-up config
- [configs/supramatic-e2-proxy.yaml](configs/supramatic-e2-proxy.yaml): RS-485 network proxy/debug mode
- [configs/supramatic-e2-monitor.yaml](configs/supramatic-e2-monitor.yaml): read-only HTTP monitor firmware
- [docs/FAQ.md](docs/FAQ.md): common hardware, protocol, and Home Assistant questions
- [docs/code-architecture.md](docs/code-architecture.md): component structure and protocol flow

## Documentation

- [Getting started](docs/getting-started.md)
- [Hardware wiring](docs/hardware-wiring.md)
- [Flashing and OTA](docs/flashing-ota.md)
- [Home Assistant and HomeKit Bridge](docs/home-assistant-homekit.md)
- [Safety](docs/safety.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Protocol notes](docs/protocol-notes.md)
- [Time-based position estimation](docs/time-based-position.md)
- [Debugging captures](docs/debugging-captures.md)
- [HTTP monitor mode](docs/http-monitor-mode.md)
- [RS-485 proxy mode](docs/proxy-mode.md)
- [Persistent protocol log](docs/persistent-protocol-log.md)
- [FAQ](docs/FAQ.md)
- [Release checklist](docs/release-checklist.md)

## Python Tools

The helper tools are managed with `uv` and pinned to Python 3.11:

```bash
uv sync
uv run garage-decode-phone-sync-video --self-test
uv run garage-phone-sync --dry-run
```

The tools are mainly for protocol debugging and motion calibration. Normal users only need ESPHome unless they want to reproduce the calibration process.

## Home Assistant and HomeKit

The ESP exposes a Home Assistant `cover` entity through the ESPHome native API. HomeKit is handled by Home Assistant's HomeKit Bridge. No direct HomeKit code runs on the ESP32.

The garage light is exposed as a separate Home Assistant light and can also be included in HomeKit Bridge. See [docs/home-assistant-homekit.md](docs/home-assistant-homekit.md).

## Compatibility

This project targets Hörmann HCP1/UAP1-era openers, specifically the SupraMatic E2 setup described above. It is not intended for Series 4/HCP2, BlueSecur, UAP1-HCP, relay-only installations, or setups that require a physical UAP1.

If you test another opener/index, please share sanitized logs and the exact opener model/index. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

This project is released under [The Unlicense](LICENSE). Upstream attribution is documented in [NOTICE.md](NOTICE.md).
