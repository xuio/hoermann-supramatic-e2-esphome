# Code Architecture

## Components

```text
components/uapbridge/       ESPHome entities shared by bridge variants
components/uapbridge_esp/   ESP32 HCP1/UAP1 emulator and debug server
components/rs485_proxy/     TCP RS-485 proxy firmware component
components/rs485_http_monitor/ read-only HTTP monitor firmware component
tools/                      Python calibration and debugging utilities
```

## Main Runtime Flow

1. UART receives HCP frames from the opener.
2. `uapbridge_esp` parses broadcasts, scan requests, and status requests.
3. CRC-valid frames update diagnostics and decoded state.
4. Scan/status requests are answered as a UAP1-like slave at address `0x28`.
5. Home Assistant commands queue one-shot command bits.
6. The next valid status response carries the command bit, then clears it.
7. `uapbridge` entities publish cover, light, binary sensor, text sensor, button, switch, and output states.

## Safety Gates

Movement commands are blocked when the bus is stale, command timeout expires, obstruction is latched, or a command type is disabled in YAML.

Close, impulse, and venting are treated more cautiously than open because they can move the door toward a hazardous direction.

## Position Estimation

The cover component estimates position from:

- calibrated start delays
- visible motion durations
- empirical opening/closing curves
- interrupted-stop response coefficients
- HCP endpoint bits for final correction

It does not report exact closed solely from timing. The HCP closed bit must be decoded.

## Debug Transport

The main firmware includes an HTTP debug server so traffic can be inspected without replacing the UAP1 emulator firmware. Persistent logs are stored in PSRAM and dumped as compact binary records.
