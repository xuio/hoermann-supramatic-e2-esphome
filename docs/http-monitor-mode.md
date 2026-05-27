# HTTP Monitor Mode

HTTP monitor mode flashes the Waveshare ESP32-S3-ETH PoE hardware as a read-only RS-485 capture node. It is intentionally different from proxy mode: it never transmits to the Hörmann bus, so it is useful for remote inspection when TCP-proxy TX latency is too high for live protocol experiments.

Use [supramatic-e2-monitor.yaml](supramatic-e2-monitor.yaml) for this mode.

## Endpoints

Open the monitor page in a browser:

```text
http://supramatic-e2-monitor.local:8080/
```

Useful raw endpoints:

```text
GET /events  Server-Sent Events stream
GET /stream  Plain text stream for curl and scripts
GET /recent  Recent RX/GAP lines kept in RAM
GET /stats   JSON counters and connection state
```

The stream sends a keepalive every five seconds and uses the same line shape as the passive TCP proxy:

```text
HELLO rs485_http_monitor v1 baud=19200 mode=rx-only
RX <seq> <micros> <hex-bytes>
GAP <seq> <micros> <gap-us>
```

Examples:

```bash
curl -N http://supramatic-e2-monitor.local:8080/stream
curl http://supramatic-e2-monitor.local:8080/recent
curl http://supramatic-e2-monitor.local:8080/stats
uv run garage-hcp-proxy-client --host supramatic-e2-monitor.local --http-stream
```

`/events` is the browser-friendly Server-Sent Events endpoint. It emits `hello`, `rx`, and `gap` events with JSON payloads.

## Safety Model

- This mode has no TX command path.
- Config validation rejects UART `tx_pin`; leave the ESP32 TX wire disconnected for a strictly passive monitor.
- Incomplete HTTP request lines are closed after a short timeout so one client cannot block the monitor.
- It does not emulate UAP1 and does not expose Home Assistant garage-door entities.
- It owns the UART, so do not enable it in the same firmware as `uapbridge_esp` or `rs485_proxy`.
- Use the same isolated Waveshare TTL TO RS485 (C) wiring as the main firmware.

Use this mode to collect passive SupraMatic E2 bus traffic and compare raw frames against the firmware's decoded state mapping. For timing-critical command/status-response behavior, keep the protocol logic on the ESP32 firmware.
