# Persistent Protocol Log

The main UAP1 emulation firmware can capture protocol traffic into PSRAM while it continues to control the door. This is intended for multi-minute captures such as obstruction tests where the interesting bytes may be missed by a live HTTP stream.

## What It Captures

- Raw RX byte batches from the RS-485 adapter.
- Raw TX responses sent by the UAP1 emulator.
- Inter-frame gaps above the configured HTTP debug gap threshold.
- Decoded frame candidates with CRC result.
- Unknown CRC-valid HCP frames found by the passive frame scanner.
- Command queue, send, block, cancel, and expiry events.
- Broadcast status transitions with decoded status bits.

The firmware stores records in a 6 MiB PSRAM live buffer as compact binary events. Adjacent identical records are compressed with a repeat count. `/persistent_log` expands the stored binary records back to JSON fields such as `first_ms`, `last_ms`, `repeat`, `source`, `hex`, `crc`, `status_hex`, and decoded `bits`.

The PSRAM capture avoids flash wear entirely. On measured idle SupraMatic E2 HCP traffic, 6 MiB is roughly 35 to 40 minutes of continuous capture. It records the raw byte stream, so protocol details that the decoder does not understand yet are still present in the `hex` fields. Captures are lost on reboot or power loss, so stop and download the log after the diagnostic run.

## OTA Notes

No persistent-log partition migration is needed. A normal ESPHome OTA upload is enough:

```bash
uv run esphome upload supramatic-e2.yaml --device <local-ip>
```

The old `/persistent_log/format` endpoint remains as a compatibility alias for clearing the PSRAM buffer. It does not format flash.

## Runtime Commands

Use the device host name or IP address:

```bash
curl http://supramatic-e2.local:8080/persistent_log/clear
curl http://supramatic-e2.local:8080/persistent_log/start
curl http://supramatic-e2.local:8080/persistent_log/stop
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log.bin > persistent-log.bin
```

`/persistent_log/start` enables recording without rebooting. `/persistent_log/stop` disables recording. `/persistent_log/clear` resets the PSRAM buffer and drop counters.

For large captures, `/persistent_log.bin` streams the compact binary buffer directly. This is faster and more reliable than expanding JSON on the ESP; decode it locally with the same record format used by `/persistent_log`.

The summary returned by `/persistent_log/start`, `/persistent_log/stop`, and `/persistent_log/clear` includes `storage:"psram"`, `ram_used`, `ram_capacity`, `flash_writes:false`, and drop counters. If `dropped_records` is nonzero, the capture reached the PSRAM buffer cap.

## Obstruction Capture

1. Confirm the door is safe to operate and you are physically present.
2. Clear and start the persistent log.
3. Open the door.
4. Close the door and trigger the obstruction.
5. Wait until the opener display shows its final error code.
6. Stop and dump the persistent log. For longer captures, prefer the binary endpoint.
7. Also fetch the current broadcast summary:

```bash
curl http://supramatic-e2.local:8080/broadcast_status > broadcast-status.json
curl http://supramatic-e2.local:8080/stats > stats.json
```

Check `stats.json` for `unknown_valid_frame_count` and `last_valid_frame_hex`. If the count increased during the test, search `persistent-log.json` for `"source":"unknown-valid"`; those frames are candidates for adding explicit E2 error or obstruction decoding.

```bash
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log > persistent-log.json
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log.bin > persistent-log.bin
```

For protocol analysis, keep the JSON files together with the physical sequence notes: closed, opening, open, closing, obstruction, opener display code, and whether Home Assistant commands were accepted or blocked after the event.
