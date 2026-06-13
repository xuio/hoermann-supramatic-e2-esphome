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
uv run esphome config configs/supramatic-4-wokwi.yaml
uv run esphome compile configs/supramatic-4-wokwi.yaml
```
