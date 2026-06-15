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
uv run garage-supramatic-sim --socketpair --scenario open-from-closed --door-travel-cycles 40 --cycles 80
uv run garage-supramatic-sim --socketpair --scenario closing-obstruction --obstruction-cycle 8 --cycles 80
uv run garage-supramatic-sim --socketpair --scenario goto-position --goto-position-raw 80 --cycles 120
uv run garage-supramatic-sim --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 --cycles 1000 --report hil.json
uv run garage-supramatic-sim --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 --cycles 1000 --trace hil-trace.jsonl --report hil.json
uv run garage-supramatic-sim --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 \
  --scenario open-from-closed --emulate-esphome-commands --door-travel-cycles 260 --cycles 320
```

The report is JSON and includes poll counts, replies, misses, consecutive misses,
latency min/mean/p99/max, fault checks, button observations, and the verdict. The
default error-04 model is pessimistic: 5 consecutive missed status polls fail the
run. The real threshold, and whether a given communication failure is surfaced as
Error 04 or Error 07 by a specific motor firmware, is not known yet and must be
measured during HIL/real-motor phases.

The optional JSONL trace writes every bus scan, status poll, command, reply, and
miss with a relative timestamp. Use it to correlate simulator poll numbers with
logic-analyzer DE windows and decoded UART counters.

For HIL command-path checks, `--expect-button NAME` requires at least one decoded
status response for each expected virtual button. Valid names currently match the
protocol decoder (`open`, `close`, `stop`, `vent`, `half`, `light`):

```bash
uv run garage-supramatic-sim --serial /dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC032762-if00 \
  --cycles 200 --expect-button open --expect-button close --expect-button light
```

For HIL movement-model checks without an ESPHome native-API command path, use
`--emulate-esphome-commands`. It treats scenario actions as ESPHome commands that were
accepted and advances only the simulator's virtual door model. The report records these
under `emulated_button_observations`; they intentionally do not satisfy
`--expect-button`, because no decoded HCP2 button output from the DUT was proven.

Named scenarios exercise the behavior model around the same poll loop:

- `steady`: scan plus steady broadcast/status-poll cycles.
- `open-from-closed`, `close-from-open`: full travel with raw position `0..200`.
- `stop-mid-opening`: stop/impulse while opening, with configurable stop latency.
- `reverse-open-to-close`, `reverse-close-to-open`: profile-dependent reversal.
- `half-position`, `vent-position`: move to configurable raw presets.
- `light-toggle`: button-commanded light-word transition.
- `closing-obstruction`: provisional force-obstruction model; increments the synthetic incomplete-cycle counter and optionally emits synthetic unknown flags.
- `goto-position`: bridge-assisted arbitrary positioning; stop is issued with one-step stop-lag compensation.

Unknown motor behavior is parameterized, not treated as fact. The most important
parameters are `--reverse-profile stop_then_reverse|immediate_reverse|ignore_until_stop`,
`--reverse-dwell-cycles`, `--stop-latency-cycles`, `--overshoot-raw-ticks`,
`--half-position-raw`, `--vent-position-raw`, `--obstruction-cycle`, and
`--speculative-obstruction-flags`. Calibrate these with real Series 4 captures before
using the simulator as a physical-behavior authority.

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
- wrong slave ID
- wrong register
- malformed byte count

Use `--fault-every-cycles N` and repeated `--fault-cycle C` arguments to run the same
faults repeatedly instead of only once near startup. The simulator asserts no response
to malformed frames and recovery by the next valid poll.

For real HIL command scenarios, use `garage-hcp2-hil-load` with ESPHome native API
object IDs so the simulator master can keep polling while commands are scheduled on
the ESP:

```bash
uv run garage-hcp2-hil-load --serial /dev/serial/by-id/usb-... \
  --esp-host supramatic-4-tester.local \
  --esp-expected-name supramatic-4-tester \
  --esp-cover-object-id garage_door_hcp2_tester \
  --esp-button-object-id half=garage_door_hcp2_tester_half_command \
  --esp-button-object-id vent=garage_door_hcp2_tester_vent_command \
  --esp-button-object-id light=garage_door_hcp2_tester_light_command \
  --scenario goto-position --goto-position-raw 80 --expect-button open --expect-button stop
```

If the ESPHome command entities are unavailable but the HIL bench should still exercise
movement scenarios, `garage-hcp2-hil-load` auto-selects emulated command mode for named
scenarios without configured object IDs. You can also force it with
`--command-mode emulated`. Do not combine that mode with `--expect-button`; native API
mode remains the command-path proof.

Known simulation limits:

- PTYs and socketpairs do not model parity, framing errors, baud tolerance, line biasing,
  RS-485 turnaround, or on-wire timing.
- Parity/framing error paths are exposed in the core port API and tested at host level,
  but HIL remains the timing and physical-layer authority.
- The simulator proves protocol logic and recovery behavior, not electrical correctness.
