# ESPHome Configurations

Use `configs/supramatic-e2.yaml` for the normal Waveshare ESP32-S3-ETH PoE UAP1 emulator.

## Files

- `supramatic-e2.yaml`: main firmware
- `supramatic-e2-minimal.yaml`: conservative bring-up firmware
- `supramatic-e2-monitor.yaml`: read-only HTTP monitor
- `supramatic-e2-monitor-rx17.yaml`: monitor with RX on GPIO17 for TTL pin-direction diagnosis
- `supramatic-e2-monitor-idle-tx.yaml`: monitor that holds adapter TTL RX idle-high
- `supramatic-e2-proxy.yaml`: TCP RS-485 proxy
- `secrets.example.yaml`: copy to `secrets.yaml` before building

## Build

```bash
cp configs/secrets.example.yaml configs/secrets.yaml
uv run esphome config configs/supramatic-e2.yaml
uv run esphome compile configs/supramatic-e2.yaml
```
