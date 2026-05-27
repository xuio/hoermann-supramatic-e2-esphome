# SupraMatic E2 Log Capture

Temporarily set `diagnostic_mode: true` in `supramatic-e2.yaml`, then run:

```bash
esphome logs supramatic-e2.yaml
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
| Error / prewarn |  |  |  |  |

Useful log lines look like:

```text
broadcast crc=ok data=...
Decoded status 0x....
State changed from ... to ...
Queued one-shot HCP command: ...
Sending one-shot HCP command: ...
```

If the cover state is wrong, the raw broadcast and decoded status lines are the important part. Do not rely on HomeKit behavior until Home Assistant shows the correct garage cover state.

For short error or obstruction captures, prefer the main firmware's persistent protocol log because it records raw RX/TX details even when no live log client is connected:

```bash
curl http://supramatic-e2.local:8080/persistent_log/clear
curl http://supramatic-e2.local:8080/persistent_log/start
# run the physical test
curl http://supramatic-e2.local:8080/persistent_log/stop
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log > persistent-log.json
```

The dump is expanded JSON from a compact binary capture file stored on the ESP's `hcp_logs` SPIFFS partition. See [persistent-protocol-log.md](docs/persistent-protocol-log.md).
