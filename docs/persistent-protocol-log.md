# Persistent Protocol Log

The main UAP1 emulation firmware can capture protocol traffic into ESP internal preferences storage while it continues to control the door. This is intended for short, targeted captures such as obstruction tests where the interesting bytes may be missed by a live HTTP stream.

## What It Captures

- Raw RX byte batches from the RS-485 adapter.
- Raw TX responses sent by the UAP1 emulator.
- Inter-frame gaps above the configured HTTP debug gap threshold.
- Decoded frame candidates with CRC result.
- Command queue, send, block, cancel, and expiry events.
- Broadcast status transitions with decoded status bits.

The stored format is a compact binary ring buffer. Adjacent identical records are compressed with a repeat count; `/persistent_log` expands that back to JSON fields such as `first_ms`, `last_ms`, `repeat`, `hex`, `crc`, `status_hex`, and decoded `bits`.

## Runtime Commands

Use the device host name or IP address:

```bash
curl http://supramatic-e2.local:8080/persistent_log/clear
curl http://supramatic-e2.local:8080/persistent_log/start
curl http://supramatic-e2.local:8080/persistent_log/stop
curl http://supramatic-e2.local:8080/persistent_log > persistent-log.json
```

`/persistent_log/start` enables recording without rebooting. `/persistent_log/stop` disables recording and flushes the ring buffer to storage. `/persistent_log/clear` erases the ring and leaves recording in its previous enabled or disabled state.

Do not leave persistent logging enabled for normal daily operation. It is designed for short diagnostic windows and writes flash periodically while active.

## Obstruction Capture

1. Confirm the door is safe to operate and you are physically present.
2. Clear and start the persistent log.
3. Open the door.
4. Close the door and trigger the obstruction.
5. Wait until the opener display shows its final error code.
6. Stop and dump the persistent log.
7. Also fetch the current broadcast summary:

```bash
curl http://supramatic-e2.local:8080/broadcast_status > broadcast-status.json
curl http://supramatic-e2.local:8080/stats > stats.json
```

For protocol analysis, keep the JSON files together with the physical sequence notes: closed, opening, open, closing, obstruction, opener display code, and whether Home Assistant commands were accepted or blocked after the event.

