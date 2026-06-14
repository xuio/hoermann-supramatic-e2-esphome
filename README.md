# Hörmann SupraMatic E2 ESPHome Garage Door Controller

[![CI](https://github.com/xuio/hoermann-supramatic-e2-esphome/actions/workflows/ci.yml/badge.svg)](https://github.com/xuio/hoermann-supramatic-e2-esphome/actions/workflows/ci.yml)
[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](LICENSE)

This project turns one **Waveshare ESP32-S3-ETH board with PoE** and one **isolated RS-485 adapter** into a wired Home Assistant controller for a **Hörmann SupraMatic E2** garage-door opener.

In practical terms:

- The ESP32 plugs into the opener's small BUS/accessory connector.
- Home Assistant gets a normal garage-door `cover` entity, plus light and diagnostics.
- Apple Home can control the door through Home Assistant's HomeKit Bridge.
- No physical Hörmann UAP1 module, cloud service, relay hack, BlueSecur bridge, or second microcontroller is required.

The firmware works by speaking the older Hörmann accessory-bus protocol directly. Technically, it behaves like a UAP1 accessory on the HCP1 bus, but the goal is simple: a local Ethernet garage-door integration for Home Assistant.

```text
Hörmann SupraMatic E2 BUS port
  -> isolated RS-485 adapter
  -> Waveshare ESP32-S3-ETH with PoE
  -> ESPHome
  -> Home Assistant
  -> HomeKit Bridge
  -> Apple Home
```

## What The Terms Mean

| Term | Plain meaning |
| --- | --- |
| BUS / HCP1 | The older wired Hörmann accessory connector used by the tested SupraMatic E2. |
| UAP1 | A Hörmann accessory module that lets external systems control and read the opener. This firmware imitates the bus conversation, so you do not need the physical UAP1 box. |
| RS-485 | The electrical signalling used on the BUS. The ESP32 must use an RS-485 adapter; do not wire ESP32 GPIO directly to the opener. |
| ESPHome | Firmware framework that makes the ESP32 appear directly in Home Assistant. |
| HomeKit Bridge | A Home Assistant feature that exposes the Home Assistant garage-door entity to Apple Home. HomeKit code does not run on the ESP32. |
| Position estimate | A calibrated timer-based door position, because this opener does not provide continuous position over the bus. |

## Current Status

Tested on one SupraMatic E2 installation with:

- Hörmann SupraMatic E2, Series 2 / HCP1-era opener
- Waveshare ESP32-S3-ETH board powered by IEEE 802.3af PoE
- Waveshare TTL TO RS485 (C) isolated adapter
- RJ12 / 6P6C cable or breakout for the opener BUS socket

Supported and tested in Home Assistant:

- Garage-door cover with `device_class: garage`
- Open, close, stop, light toggle, and optional vent/impulse controls
- Open/closed/moving state from BUS status frames
- Time-based position estimation with calibrated motion curves
- Obstruction/close-failure latch when a close does not reach the confirmed closed state
- HTTP debug stream and PSRAM protocol capture for troubleshooting
- OTA updates over Ethernet

This is an unofficial reverse-engineered integration. Treat it as a careful builder project, not as an official Hörmann product.

### Series 4 / HCP2 Development Status

Support for **Hörmann Series 4 / HCP2 / UAP1-HCP** drives is under active development
for a separate **ESP32-C6** target. The HCP2 protocol core, simulator, LP-core firmware,
Wokwi full-firmware harness, dual-ISS mailbox harness, and ESPHome component skeleton are
in-tree. A dedicated Series 4 tester image exposes all known commands and a RAM-only
protocol log/support-bundle path for remote debugging. The path is still simulation-first
and bench-first: Wokwi is the primary
no-hardware full-firmware gate on the fixed ESP32-C6 native LP-UART backend, but its
GitHub Actions job is manual-only (`run_wokwi`) and does not run on normal push/PR CI.
The local ISS covers deterministic mailbox/FIFO/MMIO checks, and the first ESP32-C6 plus
USB-RS485 HIL bench now passes polling, fault, command, CPU-only reset, OTA, API restart,
and Wi-Fi disruption scenarios. Intermediate position moves now arm an LP-core stop
trigger so the LP can press stop if the HP side dies mid-move. The remaining known
unsafe case is USB serial flashing / download-mode reset while attached to the bus;
real-motor testing must use OTA-only operation or physically isolate the transceiver
during serial flashing.

## Safety First

Garage doors can injure people and damage property. Build and test this only while physically present at the door.

Before enabling remote close:

- Verify that open/closed/moving state is reported correctly.
- Verify your obstruction protection and photocell/safety hardware.
- Test all commands while standing at the door.
- Read [docs/safety.md](docs/safety.md).

The firmware avoids reporting exact `0%` closed from timing alone. It only reports fully closed after the opener's BUS status confirms it.

## Hardware

Known working hardware:

- Hörmann SupraMatic E2 opener
- Waveshare ESP32-S3-ETH board with IEEE 802.3af PoE
- Waveshare TTL TO RS485 (C) isolated half-duplex adapter
- RJ12 / 6P6C cable or breakout for the opener BUS socket

Do not use a normal RJ11 telephone cable unless you have verified it has all six contacts and the needed conductors.

See [docs/hardware-wiring.md](docs/hardware-wiring.md) for the wiring diagram, pinout, cable color example, termination notes, and adapter wiring.

## Build And Flash

Install `uv`, then build with the pinned Python and ESPHome versions:

```bash
git clone https://github.com/xuio/hoermann-supramatic-e2-esphome.git
cd hoermann-supramatic-e2-esphome
uv sync
uv run garage-init-secrets
```

This creates `configs/secrets.yaml` with a valid ESPHome API encryption key, OTA password, and proxy token. The file is private and intentionally ignored by git. To intentionally regenerate existing secrets, run `uv run garage-init-secrets --force`.

Validate and compile:

```bash
uv run esphome config configs/supramatic-e2.yaml
uv run esphome compile configs/supramatic-e2.yaml
```

The first flash is normally over USB-C:

```bash
uv run esphome run configs/supramatic-e2.yaml
```

After the first flash, update over Ethernet:

```bash
uv run esphome upload configs/supramatic-e2.yaml --device supramatic-e2.local
```

For conservative first bus bring-up, use [configs/supramatic-e2-minimal.yaml](configs/supramatic-e2-minimal.yaml). It exposes diagnostics while keeping remote close, impulse, and unverified stop disabled. Use the full [configs/supramatic-e2.yaml](configs/supramatic-e2.yaml) only after state and safety behavior have been verified.

See [docs/getting-started.md](docs/getting-started.md) and [docs/flashing-ota.md](docs/flashing-ota.md) for the complete setup flow.

## Home Assistant And Apple Home

The ESP exposes entities to Home Assistant through the ESPHome native API:

- Main garage-door cover
- Separate garage light
- Position estimate and clear-opening height sensors
- Diagnostic binary sensors and debug endpoints

Apple Home support comes from Home Assistant's HomeKit Bridge. Include the Home Assistant cover and light in HomeKit Bridge; no direct HomeKit firmware is needed on the ESP32.

See [docs/home-assistant-homekit.md](docs/home-assistant-homekit.md).

## Position Calibration

This step is optional. If you only want normal garage-door behavior such as open, close, stop, light, Home Assistant state, and HomeKit Bridge support, you do not need the visual calibration workflow.

The calibration exists for one extra feature: percentage position control, where Home Assistant can request a target like `25%` or `50%` open. The SupraMatic E2 does not appear to expose reliable continuous position over the BUS, so percentage control has to be estimated from travel time.

The included defaults are based on the tested door. Reproducing the calibration for another door is deliberately a bit overengineered: it uses printed ArUco markers, phone video, QR timecode alignment, and HCP/BUS protocol logs. That complexity is only useful if you want to tune position control accurately.

The model has two parts:

- Full open and full close use measured motion curves and confirmed endpoint status.
- Intermediate percentage targets use a measured interrupted-stop model.

See [docs/time-based-position.md](docs/time-based-position.md) for the firmware settings and [tools/README.md](tools/README.md) for the visual calibration workflow with screenshots and plots.

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

- [configs/supramatic-e2.yaml](configs/supramatic-e2.yaml): main Ethernet/PoE firmware
- [configs/supramatic-e2-minimal.yaml](configs/supramatic-e2-minimal.yaml): safer minimal bring-up config
- [configs/supramatic-4-dev.yaml](configs/supramatic-4-dev.yaml): ESP32-C6 HCP2 simulation/bench development firmware
- [configs/supramatic-4-tester.yaml](configs/supramatic-4-tester.yaml): ESP32-C6 HCP2 Series 4 tester image with command buttons and RAM protocol logging
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
- [HCP2 Series 4 tester image](docs/hcp2-series4-tester.md)
- [Python tools and visual calibration workflow](tools/README.md)
- [FAQ](docs/FAQ.md)
- [Release checklist](docs/release-checklist.md)

## Python Tools

The helper tools are managed with `uv` and pinned to Python 3.11:

```bash
uv sync
uv run garage-decode-phone-sync-video --self-test
uv run garage-phone-sync --dry-run
```

Normal users only need ESPHome. The Python tools are mainly for protocol debugging and motion calibration.

## Compatibility

The production-ready firmware targets the older wired accessory BUS used by the tested Hörmann SupraMatic E2 setup.
Series 4 / HCP2 support is present only as simulation and ESP32-C6 bench-development code until the HIL and real-motor phases are complete.
This project is not intended for:

- BlueSecur cloud/app integrations
- Relay-only installations
- Setups that require a physical UAP1 module

If you test another opener or index letter, please share sanitized logs and the exact opener model/index. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

This project is released under [The Unlicense](LICENSE). Upstream attribution is documented in [NOTICE.md](NOTICE.md).
