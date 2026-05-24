# RS-485 Proxy Mode

Proxy mode flashes the same Waveshare ESP32-S3-ETH PoE hardware as a raw Ethernet-to-UART bridge for HCP1 debugging. It does not load the UAP1 emulator and it does not expose Home Assistant cover, light, switch, or button entities.

Use [supramatic-e2-proxy.yaml](supramatic-e2-proxy.yaml) for this mode.

## Safety Model

- `allow_tx: false` is the default. In that mode, the ESP32 only streams bus bytes to your laptop.
- Active transmit requires `allow_tx: true` and `auth_token: !secret proxy_auth_token`.
- Proxy mode is unaffiliated with HomeKit and should not be left enabled with TX on a general LAN.
- Live laptop-side UAP1 emulation may miss the drive response window because TCP/W5500/laptop scheduling latency is not deterministic. Use passive capture first. If active tests are needed, prefer short explicit `TXB` frames while physically present at the door.
- With the Waveshare TTL TO RS485 (C), omit `rts_pin`; the adapter handles direction internally. For a bare DE/RE transceiver in proxy mode, the proxy firmware can toggle `rts_pin` around its own `TX`/`TXB` writes, but laptop-side generic serial-proxy RTS control is not automatic. Prefer an auto-direction adapter for passive monitoring.
- On the Waveshare isolated adapter, `PE` is the isolated RS-485-side signal ground. Connect adapter PE only to Hörmann BUS pin 3; do not connect it to mains earth, the Ethernet shield, or ESP32 GND.

## TCP Protocol

Connect to TCP port `6638`.

ESP32 to laptop:

```text
HELLO rs485_proxy v1 baud=19200 mode=rx-only auth=none
RX <seq> <micros> <hex-bytes>
GAP <micros> <gap-us>
OK TX <seq> bytes=<n> break=<0-or-1>
ERR <reason>
```

Laptop to ESP32:

```text
AUTH <token>
PING
INFO
TX <hex-bytes>
TXB <hex-bytes>
CLOSE
```

`TX` writes bytes at `19200 8N1`. `TXB` first generates the HCP sync break by briefly switching UART to `9600 7N1`, sending `00`, restoring `19200 8N1`, then writing the provided bytes.

## Python Client

Passive capture:

```bash
python3 tools/hcp_proxy_client.py --host supramatic-e2-proxy.local
```

Active authenticated transmission, only after flashing with `allow_tx: true` and `auth_token`, should use a frame generated from the current master request counter. Do not replay this placeholder as-is:

```bash
python3 tools/hcp_proxy_client.py --host supramatic-e2-proxy.local --token "$PROXY_TOKEN" --tx-break "<counter-derived-frame>"
```

The script prints raw proxy events and decodes CRC-valid HCP candidates it recognizes: broadcasts, slave scans, and UAP1 status requests.
