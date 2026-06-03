# Troubleshooting

## No RX Bytes

Check:

- RJ12 cable really has six contacts.
- BUS pin orientation.
- Adapter has 3.3 V power.
- Adapter TTL TXD goes to ESP32 RX.
- Adapter TTL RXD goes to ESP32 TX.
- A+ and B- are not shorted.
- A/B polarity. Swap A/B once as a diagnostic if no valid frames appear.
- Opener was power-cycled after connecting the bus.

## Valid Broadcast Never Turns On

Use monitor firmware first:

```bash
uv run esphome upload configs/supramatic-e2-monitor.yaml --device supramatic-e2.local
```

Open:

```text
http://supramatic-e2-monitor.local:8080/
```

If no RX appears in monitor mode, the issue is physical wiring or bus availability, not UAP1 emulation.

## Commands Do Not Work

Check:

- `allow_remote_close` and `allow_remote_impulse` in the YAML.
- `require_fresh_broadcast_for_commands` diagnostic state.
- Active obstruction latch.
- Whether the opener is sending scan/status requests and receiving UAP1 responses.

## Door State Is Wrong

Do not assume unknown or stopped frames mean closed. Capture `/stats`, `/broadcast_status`, and a persistent log, then compare raw HCP status values while the door is fully closed, opening, fully open, closing, stopped halfway, and obstructed.

## Home Assistant Works but HomeKit Does Not

Check Home Assistant first. HomeKit Bridge maps Home Assistant cover states, not raw HCP frames. If Home Assistant state is correct but Apple Home is stale, restart the HomeKit Bridge or re-pair it.
