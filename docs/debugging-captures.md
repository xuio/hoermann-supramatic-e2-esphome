# Debugging Captures

The project has three debugging paths.

## Live HTTP Monitor

The main firmware exposes a live monitor on port `8080` while the UAP1 emulator continues running:

```text
http://supramatic-e2.local:8080/
```

Use it for raw RX/TX, decoded frames, command queue/send/block events, and recent history.

## Persistent Protocol Log

Enable PSRAM logging at runtime:

```bash
curl http://supramatic-e2.local:8080/persistent_log/clear
curl http://supramatic-e2.local:8080/persistent_log/start
# reproduce the issue
curl http://supramatic-e2.local:8080/persistent_log/stop
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log.bin > persistent-log.bin
```

This does not write ESP flash.

## RS-485 Proxy Mode

Proxy mode is useful for passive capture and laptop-side experiments. It is not recommended for timing-critical live UAP1 emulation because the opener's response window can be short.

See [proxy-mode.md](proxy-mode.md).
