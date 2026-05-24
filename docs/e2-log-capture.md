# SupraMatic E2 Log Capture

Set `diagnostic_mode: true` in `supramatic-e2.yaml`, then run:

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
