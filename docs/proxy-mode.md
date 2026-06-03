# RS-485 Proxy Mode

Proxy mode flashes the same Waveshare ESP32-S3-ETH PoE hardware as a raw Ethernet-to-UART bridge for HCP1 debugging. It does not load the UAP1 emulator and it does not expose Home Assistant cover, light, switch, or button entities.

Use [configs/supramatic-e2-proxy.yaml](../configs/supramatic-e2-proxy.yaml) for this mode.

## Safety Model

- `allow_tx: false` is the default. In that mode, the ESP32 only streams bus bytes to your laptop.
- Active transmit requires adding `tx_pin` to the UART, setting `allow_tx: true`, and configuring `auth_token: !secret proxy_auth_token`.
- Config validation requires the proxy UART to be `19200 8N1`, with RX always configured and TX configured only when `allow_tx: true`.
- Authenticated TX is rejected while UART bytes are pending or the last RX byte is newer than `gap_threshold`.
- When auth is required, unauthenticated clients are disconnected after a short timeout so they cannot monopolize the capture socket.
- Proxy mode is unaffiliated with HomeKit and should not be left enabled with TX on a general LAN.
- Live laptop-side UAP1 emulation may miss the drive response window because TCP/W5500/laptop scheduling latency is not deterministic. Use passive capture first. If active tests are needed, prefer short explicit `TXB` frames while physically present at the door.
- With the Waveshare TTL TO RS485 (C), omit `rts_pin`; the adapter is assumed to handle direction internally because it exposes no DE/RE pin. Verify TX on your board before relying on it. For a bare DE/RE transceiver in proxy mode, the proxy firmware can toggle `rts_pin` around its own `TX`/`TXB` writes, but laptop-side generic serial-proxy RTS control is not automatic.
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

For active TX, the UART section must include a TX pin. Keep this out of the default receive-only capture config:

```yaml
uart:
  id: uart_bus
  baud_rate: 19200
  data_bits: 8
  parity: NONE
  stop_bits: 1
  rx_pin: GPIO16
  tx_pin: GPIO17

rs485_proxy:
  allow_tx: true
  auth_token: !secret proxy_auth_token
```

## Python Client

Passive capture:

```bash
uv run garage-hcp-proxy-client --host supramatic-e2-proxy.local
```

Active authenticated transmission, only after flashing with `allow_tx: true` and `auth_token`, should use a frame generated from the current master request counter. Do not replay this placeholder as-is:

```bash
uv run garage-hcp-proxy-client --host supramatic-e2-proxy.local --token "$PROXY_TOKEN" --tx-break "<counter-derived-frame>"
```

The script prints raw proxy events and decodes CRC-valid HCP candidates it recognizes: broadcasts, slave scans, and UAP1 status requests.

The ESP32 treats TCP backpressure or a partial nonblocking write as a capture-client disconnect. That is intentional: dropped capture lines are worse than a visible reconnect during protocol analysis.
