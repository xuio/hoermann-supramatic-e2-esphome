# ESPHome Configurations

Use `configs/supramatic-e2.yaml` for the normal Waveshare ESP32-S3-ETH PoE garage-door firmware.

## Files

- `supramatic-e2.yaml`: main firmware
- `supramatic-e2-minimal.yaml`: conservative bring-up firmware
- `supramatic-e2-monitor.yaml`: read-only HTTP monitor
- `supramatic-e2-monitor-rx17.yaml`: monitor with RX on GPIO17 for TTL pin-direction diagnosis
- `supramatic-e2-monitor-idle-tx.yaml`: monitor that holds adapter TTL RX idle-high
- `supramatic-e2-proxy.yaml`: TCP RS-485 proxy
- `supramatic-4-dev.yaml`: ESP32-C6 HCP2 development firmware for simulation and bench bring-up
- `supramatic-4-tester.yaml`: ESP32-C6 HCP2 tester firmware with command buttons and HTTP protocol logging
- `supramatic-4-wokwi.yaml`: minimal ESP32-C6 HCP2 firmware for the Wokwi ESPHome gate
- `secrets.example.yaml`: template used by `uv run garage-init-secrets`

## Build

```bash
uv run garage-init-secrets
uv run esphome config configs/supramatic-e2.yaml
uv run esphome compile configs/supramatic-e2.yaml
```

For the Series 4 / HCP2 development target:

```bash
uv run esphome config configs/supramatic-4-dev.yaml
uv run esphome compile configs/supramatic-4-dev.yaml
uv run esphome config configs/supramatic-4-tester.yaml
uv run esphome compile configs/supramatic-4-tester.yaml
uv run esphome config configs/supramatic-4-wokwi.yaml
uv run esphome compile configs/supramatic-4-wokwi.yaml
```

Use `supramatic-4-tester.yaml` for an external SupraMatic Series 4 tester. It
expects HCP2 LP-UART on GPIO4/GPIO5 and RS-485 DE `/RE` on GPIO0/GPIO1. Once the
transceiver is connected to the motor bus, update by ESPHome OTA only; do not
USB-serial flash while attached. Field-test steps and support-bundle collection
are in [docs/hcp2-series4-tester.md](../docs/hcp2-series4-tester.md). The tester
image enables Improv over USB serial and a fallback setup portal for first Wi-Fi
provisioning from a prebuilt firmware artifact. The HCP2 debug UI only starts
after station Wi-Fi is connected.

## Automated Artifacts

GitHub Actions builds downloadable firmware artifacts for the main HCP1 image and
the HCP2 Series 4 tester image in `.github/workflows/firmware-build.yml`. The
newest successful `main` build is published publicly at
<https://github.com/xuio/hoermann-supramatic-e2-esphome/releases/latest>.

- `hcp1-supramatic-e2`: `configs/supramatic-e2.yaml`
- `hcp2-supramatic-4-tester`: `configs/supramatic-4-tester.yaml`

Stable direct-download names on the `firmware-latest` release:

- `hcp1-supramatic-e2-firmware.factory.bin`
- `hcp1-supramatic-e2-firmware.ota.bin`
- `hcp2-supramatic-4-tester-firmware.factory.bin`
- `hcp2-supramatic-4-tester-firmware.ota.bin`

Each public bundle includes `public-credentials.txt` with the ESPHome API key and
OTA password generated for that public image. Build locally when the credentials
must be private.
