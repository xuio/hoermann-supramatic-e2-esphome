# Flashing and OTA

## First Flash

First flash normally uses USB-C:

```bash
uv run esphome run configs/supramatic-e2.yaml
```

Keep the Hörmann bus disconnected for the first boot test.

## OTA Updates

After the first flash, update over Ethernet:

```bash
uv run esphome upload configs/supramatic-e2.yaml --device supramatic-e2.local
```

If mDNS is unreliable, use the device IP or set `ESP_HOST` in your shell.

## Switching Modes

Target the hostname of the firmware currently running on the board, not the hostname in the YAML being uploaded.

Example: the board is currently running the main firmware and you want monitor mode:

```bash
uv run esphome upload configs/supramatic-e2-monitor.yaml --device supramatic-e2.local
```

After reboot, the board advertises the monitor hostname from the monitor YAML.

## Safe Mode

All configs include ESPHome safe mode. If a bad firmware prevents normal boot, use ESPHome's safe-mode recovery flow or reflash by USB-C.
