# Getting Started

This guide assumes the known working hardware path:

- Waveshare ESP32-S3-ETH with IEEE 802.3af PoE
- Waveshare TTL TO RS485 (C) isolated adapter
- Hörmann SupraMatic E2 / HCP1-era opener
- Home Assistant with ESPHome and HomeKit Bridge

## 1. Build the Firmware

```bash
git clone https://github.com/xuio/garage-door-controller.git
cd garage-door-controller
uv sync
cp configs/secrets.example.yaml configs/secrets.yaml
```

Edit `configs/secrets.yaml`, then validate:

```bash
uv run esphome config configs/supramatic-e2.yaml
uv run esphome compile configs/supramatic-e2.yaml
```

## 2. First Flash

Flash once over USB-C:

```bash
uv run esphome run configs/supramatic-e2.yaml
```

After that, use Ethernet OTA. See [flashing-ota.md](flashing-ota.md).

## 3. Bring Up Without Moving the Door

1. Power the ESP from USB-C or PoE.
2. Leave the Hörmann bus disconnected.
3. Confirm ESPHome boots and appears in Home Assistant.
4. Open ESPHome logs.

## 4. Connect the Bus

1. Disconnect power before wiring.
2. Connect only BUS GND/reference, DATA-, and DATA+ as documented in [hardware-wiring.md](hardware-wiring.md).
3. Leave BUS +24 V disconnected when using PoE or USB-C.
4. Reconnect power and power-cycle the opener.
5. Confirm `Garage Door Valid HCP Broadcast` turns on.

## 5. Test Commands

Test in this order while physically present:

1. Light toggle
2. Stop/impulse behavior
3. Open
4. Close
5. Position targets
6. Obstruction behavior

Disable remote close and impulse again if you do not need them after testing.
