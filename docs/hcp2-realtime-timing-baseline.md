# HCP2 Realtime Timing Baseline

Measured on the NixOS HIL bench on 2026-06-18 with the auto-direction RS485
module and the ESP32-C6 wired on GPIO4/GPIO5. The active RS485 master adapter was
`/dev/ttyACM0`; `/dev/ttyUSB0` was not connected to the current bus path.

## ESP32-C6 HP Realtime

Firmware: `configs/supramatic-4-c6-hp-realtime.yaml`, backend
`esp32c6_hp_realtime`, `response_delay: 3700us`.

Loaded LA/HIL pass:

- HIL: 5020/5020 replies, 0 misses, 0 max consecutive misses.
- Fault injection: 100/100 recoveries, 0 unexpected invalid-frame responses.
- LA: 152 decoded status responses, 0 counter gaps.
- LA electrical: DE/RE bracketing OK, 0 TX transitions outside DE.
- Poll end to first response byte: min 4112.0 us, mean 4215.8 us, p99 4268.6 us,
  max 4306.0 us.
- DE hold: mean 4175.7 us, max 4232.5 us.
- Artifact root: `/tmp/hcp2-c6-hp-latency-pass/`.

Host round-trip numbers in HIL reports include Python, USB serial, and Linux
scheduling. They are useful for bench regression detection, but LA numbers are
the motor-visible latency authority.

## ESP32-C6 LP Backend Comparison

Firmware: `configs/supramatic-4-tester.yaml`, backend `esp32c6_lp`,
`response_delay: 4200us`.

Loaded LA/HIL comparison:

- HIL: 420/420 replies, 0 misses, 0 max consecutive misses.
- Fault injection: 30 recoveries, 0 unexpected invalid-frame responses.
- LA: 31 decoded status responses, 0 counter gaps.
- LA electrical: DE/RE bracketing OK, 0 TX transitions outside DE.
- Poll end to first response byte: min 5751.3 us, mean 5836.6 us, p99 5950.6 us,
  max 5960.8 us.
- Poll end to response end: min 9761.5 us, mean 9846.8 us, p99 9960.8 us,
  max 9971.0 us.
- Artifact root: `/tmp/hcp2-c6-lp-compare-20260618-183638/`.

The LP backend remains the reboot-safe production architecture. The HP realtime
backend is faster in the measured steady-state path, but it does not preserve bus
continuity through HP reset.

## Standard ESP32 Realtime Carry-Over

The standard ESP32 `esp32_realtime` backend is still experimental and unproven on
hardware. It now shares these hardening changes from the C6 HP realtime work:

- parser/responder pending-TX claim/started/done accounting;
- strict DE before RE assert, and RE before DE release;
- separate maintenance/debug publishing task so diagnostics stay off the hot bus
  path;
- HIL reports that separate host RTT from LA-authoritative motor-visible latency;
- bounded pre-TX timer wait: GPTimer wakes before the response deadline and the
  ISR spins only for the final 700 us instead of the previous multi-millisecond
  inline wait.

Do not treat `esp32_realtime` as motor-ready until the same HIL/LA matrix passes
on an actual ESP32-WROOM/no-PSRAM board.
