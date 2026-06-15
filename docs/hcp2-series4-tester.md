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

The tester image also enables ESPHome Improv over USB serial and a fallback setup
portal. That means a prebuilt image can be flashed first and then provisioned
with the tester's Wi-Fi credentials, without editing or rebuilding the YAML. For
first flashing, use `hcp2-supramatic-4-tester-firmware.factory.bin`; it is the
merged ESP32-C6 image for offset `0x0`. The public prebuilt image does not
contain a shared ESPHome API key; after Wi-Fi is configured, adopt the node
through Home Assistant / ESPHome so the per-device API encryption key is stored
in flash. OTA has no password in the public image to avoid a shared public
password. For a permanent install, add OTA auth in a local/private config and
upload that image after adoption. The HCP2 debug UI intentionally owns port `80`,
but it only starts after the device has joined station Wi-Fi; while the setup
portal is active, the debug UI is closed.

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

`Missed Polls` is the effective safety counter. During the normal reply window
the raw `Polls Seen - Polls Answered` delta may briefly be `1`; the debug page
shows this as `pending response`, not as a missed poll.

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
transitions, important LP trace breadcrumbs (boot, command, health, error), and
full protocol frames when the HP fallback responder is active.

Open `http://supramatic-4-tester.local/` in a browser for the interactive
debug page. It is organised for a two-second health read:

- A **sticky status header** with the continuity verdict and eight always-visible
  vital chips: WebSocket link, bus online, LP heartbeat (the dot pulses on each
  beat), last poll age, missed/consecutive misses, poll rate, response-delay
  jitter p95, and uptime. When the live feed goes quiet for more than two seconds
  the header turns **stale** (desaturated, dashed border); when the stream is
  down it shows **link down**. These states are display-only and never open a
  second socket.
- A **Door** card (position bar with target tick, light/obstruction badges, and
  a best-effort last-command/ack line), a **Bus timing** card, and a **Counters**
  card bound to the live health checks.
- A **Live packet log** with a toolbar: display-only **Pause/Resume** (capture
  keeps running on the device), **Table view** toggle, filters (errors only,
  direction, frame type, record type, free text), **Copy raw**, **Copy decoded**,
  **Clear view** (local only), and `Download JSON`.
- A collapsible **Diagnostics & device controls** drawer with link diagnostics,
  LP/HP counters, device log storage, full stats, the device protocol-log
  controls (`Start capture` / `Stop capture` / `Clear device buffer` /
  `Refresh from device` / `Device NDJSON` / `Device Raw`), and raw JSON views.

The page is WebSocket-primary and adds no extra device load: it reads everything
from the pushed `{"type":"health","health":...}` frames (~every 500 ms) and
`{"type":"log","text":"..."}` log frames (~every 250 ms), and only falls back to
a gentle `/health` poll when the stream is stale. `/stats` is only fetched while
a Raw JSON view is open.

Opening the page does not automatically download the full device ring buffer;
the live view starts from the current stream position. The page keeps its own
bounded browser-side cache of the live stream. The `Download JSON` button
exports that frontend cache as a structured JSON file (`hcp2-debug-log-cache-v1`)
with receive timestamps, raw lines, and parsed log objects (on mobile it offers
the share sheet, falling back to a download). To keep browser RAM bounded, the
export cache keeps only the newest 30 minutes and at most 100 MiB, and the
on-screen log renders at most the newest 5000 lines while preserving scroll
position when you scroll up. `Refresh from device`, `Device NDJSON`, and
`Device Raw` still download the ESP's current RAM ring buffer directly.

### Derived metrics (client-side, no extra device load)

The bus-timing card and the missed/jitter/poll-rate chips are computed in the
browser from the streamed packet records, so they require the device protocol
log to be capturing (chips show `— (log off)` until you press `Start capture`):

- **Response-delay jitter** is `(TX start − matching status-poll RX) − 4200 µs`,
  i.e. the deviation from the configured `HCP2_DEFAULT_RESPONSE_DELAY_US`. It is
  *not* on-wire latency, and the firmware-reported on-wire TX duration
  (`max_response_tx_us`) and `max_response_schedule_to_tx_start_us` are shown
  separately as authoritative maxima.
- **Poll rate** is derived from the `status_poll` cadence over a 10 s wall-clock
  window.
- **Max consecutive misses** is best-effort (a status poll with no matching reply
  before the next poll); it resets on a stream gap or LP↔HP source switch. The
  firmware `missed_polls`/`max_consecutive_missed_polls` counters are
  authoritative.

### Local testing without hardware

`tools/hcp2_debug_mock_server.py` serves the exact embedded page (sliced from the
firmware source) plus synthetic JSON/WebSocket traffic that mirrors the device
contract, so the UI can be exercised without flashing an ESP:

```bash
uv run python tools/hcp2_debug_mock_server.py --port 8080 --scenario nominal
# scenarios: nominal, missed_polls, bad_crc, collisions, tx_abort, stale,
#            disconnect, hp_fallback, soak
```

`tests/hcp2/test_debug_mock_server.py` runs the HTML-extraction, JSON-route, and
WebSocket-contract checks headlessly in CI, and runs the full Playwright browser
E2E (`tools/hcp2_debug_browser_e2e.py`) against the mock when Playwright is
available.

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
