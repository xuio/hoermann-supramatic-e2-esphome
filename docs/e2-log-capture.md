# SupraMatic E2 Log Capture

Temporarily set `diagnostic_mode: true` in `supramatic-e2.yaml`, then run:

```bash
uv run esphome logs supramatic-e2.yaml
```

Capture one block per physical door state.

| Physical state | Raw broadcast frame | Decoded status | Reported text state | Notes |
| --- | --- | --- | --- | --- |
| Fully closed |  |  |  |  |
| Opening |  |  |  |  |
| Fully open |  |  |  |  |
| Closing |  |  |  |  |
| Stopped halfway |  |  |  |  |
| Venting / partial-open |  |  |  |  |
| Light on |  |  |  |  |
| Light off |  |  |  |  |
| Obstruction / close failure |  |  |  |  |
| Error / prewarn |  |  |  |  |

Useful log lines look like:

```text
broadcast crc=ok data=...
Decoded status 0x....
State changed from ... to ...
Queued one-shot HCP command: ...
Sending one-shot HCP command: ...
Observed unknown CRC-valid HCP frame ...
```

If the cover state is wrong, the raw broadcast and decoded status lines are the important part. The main firmware also exposes `Garage Door Last HCP Frame`, `Garage Door HCP Status Transitions`, and `Garage Door Unknown Valid HCP Frames`; note those values before and after each physical test. Do not rely on HomeKit behavior until Home Assistant shows the correct garage cover state.

For short error or obstruction captures, prefer the main firmware's persistent protocol log because it records raw RX/TX details even when no live log client is connected:

```bash
curl http://supramatic-e2.local:8080/persistent_log/clear
curl http://supramatic-e2.local:8080/persistent_log/start
# run the physical test
curl http://supramatic-e2.local:8080/persistent_log/stop
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log.bin > persistent-log.bin
```

The dump is a compact binary capture from the ESP's PSRAM buffer and does not write flash. See [persistent-protocol-log.md](docs/persistent-protocol-log.md).
