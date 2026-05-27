# Persistent Protocol Log

The main UAP1 emulation firmware can capture protocol traffic into a dedicated SPIFFS filesystem partition while it continues to control the door. This is intended for multi-minute captures such as obstruction tests where the interesting bytes may be missed by a live HTTP stream.

## What It Captures

- Raw RX byte batches from the RS-485 adapter.
- Raw TX responses sent by the UAP1 emulator.
- Inter-frame gaps above the configured HTTP debug gap threshold.
- Decoded frame candidates with CRC result.
- Unknown CRC-valid HCP frames found by the passive frame scanner.
- Command queue, send, block, cancel, and expiry events.
- Broadcast status transitions with decoded status bits.

The firmware creates a 4 MB `hcp_logs` SPIFFS partition and caps the active capture file at 3 MB. Records are staged in a 4 KB RAM buffer as compact binary events, adjacent identical records are compressed with a repeat count, and the staged buffer is flushed to flash about every 10 seconds or when full. `/persistent_log` expands the stored binary records back to JSON fields such as `first_ms`, `last_ms`, `repeat`, `source`, `hex`, `crc`, `status_hex`, and decoded `bits`.

The filesystem-backed capture is designed to hold at least five minutes of continuous SupraMatic E2 HCP traffic. It records the raw byte stream, so protocol details that the decoder does not understand yet are still present in the `hex` fields.

## First OTA Partition Migration

Adding the filesystem partition changes the ESP32 partition table. A normal ESPHome OTA uploads only the application image; the partition table must be sent once with the dedicated OTA mode after a firmware containing `allow_partition_access: true` is already running:

```bash
uv run esphome upload supramatic-e2.yaml --device <local-ip>
uv run esphome upload supramatic-e2.yaml --device <local-ip> --partition-table
uv run esphome upload supramatic-e2.yaml --device <local-ip>
```

The final application upload after the partition-table update ensures the garage firmware is present in the app slot selected by the new table. Then run `/persistent_log/format` once if the summary reports `format_required:true`.

## Runtime Commands

Use the device host name or IP address:

```bash
curl http://supramatic-e2.local:8080/persistent_log/clear
curl http://supramatic-e2.local:8080/persistent_log/start
curl http://supramatic-e2.local:8080/persistent_log/stop
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log > persistent-log.json
```

`/persistent_log/start` enables recording without rebooting. `/persistent_log/stop` disables recording and flushes the RAM staging buffer to the SPIFFS file. `/persistent_log/clear` removes the capture file and leaves recording in its previous enabled or disabled state.

For large captures, `/persistent_log.bin` streams the compact binary capture file directly. This is faster and more reliable than expanding JSON on the ESP; decode it locally with the same record format used by `/persistent_log`.

After first installing a firmware that adds the `hcp_logs` partition, `/persistent_log/clear` may report `"format_required":true`. Run this once:

```bash
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log/format
```

Formatting is explicit because the first SPIFFS format can take tens of seconds on the 4 MB partition. The firmware avoids formatting during boot so the normal ESPHome boot watchdog does not push the device into safe mode.

The summary returned by `/persistent_log/start`, `/persistent_log/stop`, and `/persistent_log/clear` includes `filesystem_total`, `filesystem_used`, `max_file_bytes`, `file_bytes`, `ram_used`, and drop counters. If `dropped_records` is nonzero, the capture reached its cap or the filesystem could not keep up.

Do not leave persistent logging enabled for normal daily operation. It is designed for diagnostic windows and writes flash periodically while active.

## Obstruction Capture

1. Confirm the door is safe to operate and you are physically present.
2. Clear and start the persistent log.
3. Open the door.
4. Close the door and trigger the obstruction.
5. Wait until the opener display shows its final error code.
6. Stop and dump the persistent log. For longer captures, give `curl` enough time to receive the streamed JSON response.
7. Also fetch the current broadcast summary:

```bash
curl http://supramatic-e2.local:8080/broadcast_status > broadcast-status.json
curl http://supramatic-e2.local:8080/stats > stats.json
```

Check `stats.json` for `unknown_valid_frame_count` and `last_valid_frame_hex`. If the count increased during the test, search `persistent-log.json` for `"source":"unknown-valid"`; those frames are candidates for adding explicit E2 error or obstruction decoding.

```bash
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log > persistent-log.json
```

For protocol analysis, keep the JSON files together with the physical sequence notes: closed, opening, open, closing, obstruction, opener display code, and whether Home Assistant commands were accepted or blocked after the event.
