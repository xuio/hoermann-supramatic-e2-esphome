# HCP2 Series 4 Tester Image

Use `configs/supramatic-4-tester.yaml` for a first external tester with a
SupraMatic Series 4 drive. This image targets an ESP32-C6 and the HCP2 /
UAP1-HCP bus. It is separate from the stable HCP1/E2 firmware.

## Hardware Assumptions

- ESP32-C6 with its own stable power supply.
- Isolated half-duplex RS-485 transceiver.
- LP-UART pins: RX `GPIO4`, TX `GPIO5`.
- RS-485 direction pins: DE `GPIO0`, `/RE` `GPIO1`.
- Pulldowns on DE and `/RE`.

Do not USB-serial flash the ESP32-C6 while the transceiver is connected to the
motor bus. Serial download-mode reset can disturb the bus. Flash over USB only
with the transceiver isolated or disconnected; once installed on the motor, use
ESPHome OTA updates.

## Build And Flash

```bash
uv sync
uv run garage-init-secrets
uv run esphome config configs/supramatic-4-tester.yaml
uv run esphome compile configs/supramatic-4-tester.yaml
```

For first flash, keep the RS-485 transceiver disconnected from the motor bus.
After the first flash, use OTA:

```bash
uv run esphome upload configs/supramatic-4-tester.yaml --device supramatic-4-tester.local
```

The tester config exposes the normal Home Assistant garage-door cover and light,
plus explicit command buttons for `open`, `close`, `stop`, `half`, `vent`, and
`light`.

## First Registration

1. Power the ESP32-C6 and confirm it joins Wi-Fi.
2. Connect the RS-485 transceiver to the Series 4 HCP2 bus.
3. Run the motor's accessory/bus scan so it registers the bridge.
4. Confirm these entities become healthy:
   - `Valid HCP2 Broadcast`
   - `HCP2 Bus Online`
   - `HCP2 Continuity Healthy`
   - `Safe For OTA Restart`
   - `Polls Seen`
   - `Polls Answered`
   - `Missed Polls` stays `0`
5. If the motor shows error 04, stop and collect a support bundle before power
   cycling or changing wiring.

## Command Checklist

Run this while physically present at the door:

1. From closed, press `Open Command`; the door opens and state becomes opening.
2. While opening, press `Stop Command`; the door stops and remains controllable.
3. Press `Close Command`; the door closes and state becomes closing.
4. While closing, press `Stop Command`; the door stops and remains controllable.
5. Test `Light Command`; the light entity follows the decoded or commanded state.
6. Test `Vent Command` and `Half Command`; verify the drive behavior matches the
   installed motor's configured partial-open positions.
7. From Home Assistant cover controls, test open, close, stop, and a conservative
   intermediate position.

After every command, confirm `Missed Polls`, `TX Aborts`, `Collisions`,
`RX Starvations`, `Stuck DE Recoveries`, and `Mailbox Repairs` stay at `0`.

## RAM Protocol Log

The tester image enables a RAM-only HCP2 debug log on port `80`. It never
writes this log to ESP flash. The log is a ring buffer, so support bundles keep
the newest events before a failure instead of only the first events after boot.
It contains command queue/execution events, state changes, LP health
transitions, LP trace breadcrumbs (`rx`, `tx`, `de`, errors), and full protocol
frames when the HP fallback responder is active.

Open `http://supramatic-4-tester.local/` in a browser for the interactive
debug page. It shows continuity health, core counters, RAM-log controls, a live
WebSocket log stream, and raw JSON views without writing anything to flash.
The page keeps its own bounded browser-side cache of the live stream. The
`Download JSON` button exports that frontend cache as a structured JSON file
with receive timestamps, raw lines, and parsed log objects. To keep browser RAM
bounded, the cache keeps only the newest 10 minutes and at most 1 MiB. The
`Device NDJSON` and `Device Raw` links still download the ESP's current RAM ring
buffer directly.

Raw endpoints:

- `http://supramatic-4-tester.local/health`
- `http://supramatic-4-tester.local/preflight`
- `http://supramatic-4-tester.local/stats`
- `http://supramatic-4-tester.local/support`
- `http://supramatic-4-tester.local/hcp2_log/start`
- `http://supramatic-4-tester.local/hcp2_log/stop`
- `http://supramatic-4-tester.local/hcp2_log/clear`
- `http://supramatic-4-tester.local/hcp2_log`
- `http://supramatic-4-tester.local/hcp2_log.bin`
- `ws://supramatic-4-tester.local/hcp2_log/ws`

For a reproducible issue, start a fresh RAM log immediately before reproducing
the problem:

```bash
uv run garage-hcp2-support-bundle --host supramatic-4-tester.local --action start
# reproduce the issue
uv run garage-hcp2-support-bundle --host supramatic-4-tester.local --action stop-and-collect
```

The command prints the bundle directory. Send the whole directory, including
`manifest.json`, `health.json`, `support.json`, `stats.json`, `hcp2-log.ndjson`, and
`hcp2-log.bin`.

## Tester Exit Criteria

Before treating the image as usable for daily testing:

- The bridge survives an ESPHome OTA update without motor error 04.
- The bridge survives an ESPHome API restart without motor error 04.
- Open, close, stop, light, vent, and half commands all work from Home Assistant.
- The door remains controllable after stop-during-open and stop-during-close.
- Reversing direction from the Home Assistant cover first stops, then starts the
  requested direction after the drive settles.
- A support bundle collected after command tests shows zero missed polls,
  zero collisions, zero TX aborts, and no LP health flags.
