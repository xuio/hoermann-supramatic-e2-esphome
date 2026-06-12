# SupraMatic 4 Simulator

`garage-supramatic-sim` is the Phase 0b virtual HCP2 / UAP1-HCP master. It drives the
portable `hcp2_core` responder through the same transport shape that will later be used
for HIL:

- `--pty`: starts `tests/hcp2/host_responder` behind a pseudo-terminal.
- `--socketpair`: starts the same responder behind a socketpair for fast host tests.
- `--serial DEVICE`: talks to a real serial device for Phase 1 HIL. This path imports
  `pyserial` lazily so Phase 0 does not require the dependency.

Examples:

```bash
uv run garage-supramatic-sim --selftest
uv run garage-supramatic-sim --pty --cycles 1000 --report report.json
uv run garage-supramatic-sim --socketpair --cycles 1000 --fault corrupt-crc --fault truncated
uv run garage-supramatic-sim --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 --cycles 1000 --report hil.json
```

The report is JSON and includes poll counts, replies, misses, consecutive misses,
latency min/mean/p99/max, fault checks, button observations, and the verdict. The
default error-04 model is pessimistic: 5 consecutive missed status polls fail the
run. The real threshold is not known yet and must be measured during HIL/real-motor
phases.

For HIL command-path checks, `--expect-button NAME` requires at least one decoded
status response for each expected virtual button. Valid names currently match the
protocol decoder (`open`, `close`, `stop`, `vent`, `half`, `light`):

```bash
uv run garage-supramatic-sim --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 \
  --cycles 200 --expect-button open --expect-button close --expect-button light
```

The serial transport requests pyserial low-latency mode, disables software and
hardware flow control, and resets input/output buffers before the run. On the
current NixOS bench the RS-485 adapter is CDC-ACM, so there is no FTDI
`latency_timer`; keep USB autosuspend disabled on the external hub during HIL.

Fault injection currently covers:

- corrupt CRC
- truncated frame
- duplicated poll
- garbage bytes
- jittered gaps
- mid-frame split writes

Known simulation limits:

- PTYs and socketpairs do not model parity, framing errors, baud tolerance, line biasing,
  RS-485 turnaround, or on-wire timing.
- Parity/framing error paths are exposed in the core port API and tested at host level,
  but HIL remains the timing and physical-layer authority.
- The simulator proves protocol logic and recovery behavior, not electrical correctness.
