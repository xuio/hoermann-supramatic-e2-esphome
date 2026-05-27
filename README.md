# SupraMatic E2 ESPHome UAP1 Emulator

ESPHome external component and example configuration for connecting a Waveshare ESP32-S3-ETH board directly to a Hörmann SupraMatic E2 HCP1/BUS connector through a 3.3 V RS-485 transceiver.

This project targets the local path:

Waveshare ESP32-S3-ETH over Ethernet -> Home Assistant ESPHome native API -> Home Assistant HomeKit Bridge -> Apple Home

It does not implement HomeKit on the ESP32, does not use cloud services, does not require a physical Hörmann UAP1, and does not use a second MCU.

## Python Tools

Python helper tools are managed with `uv` and pinned to Python 3.11 via [.python-version](.python-version). From the repository root, run tools with the console commands from [pyproject.toml](pyproject.toml):

```bash
uv sync
uv run garage-phone-sync --dry-run
```

If you are not currently in the repository directory, pass the project directory explicitly. This avoids launching Python from `<home-directory>` and accidentally looking for `<home-directory>/tools/...`:

```bash
uv --directory  run garage-phone-sync --dry-run
```

The fullscreen phone sync tool uses Tk. The current `uv` environment has Tk available; if that ever fails on a different Python installation, install the matching Homebrew `python-tk` package.

## Hardware Target

- Opener: Hörmann SupraMatic E2, Series 2 / HCP1-era.
- User unit: serial `REDACTED_SERIAL`; type plate suffix `Ei`, interpreted as index `E` plus integrated/internal receiver `i`.
- Bus route: plausible because index `E` is later than the usual UAP1/HCP1 compatibility threshold.
- MCU: one Waveshare ESP32-S3-ETH board only, powered by USB-C or the Waveshare IEEE 802.3af PoE module/PoE variant.
- Network: onboard W5500 Ethernet over SPI, DHCP by default.
- Bus interface: Waveshare TTL TO RS485 (C) galvanically isolated half-duplex adapter. Its official wiki documents 3.3 V to 5 V TTL power/signals, half-duplex RS-485, TTL `RXD/TXD/VCC/GND`, RS-485 `A+/B-/PE`, isolation, and a 120 ohm terminator option enabled by soldering: <https://www.waveshare.com/wiki/TTL_TO_RS485_%28C%29>.

Do not connect ESP32 GPIO directly to the Hörmann bus. Do not connect the opener 24 V line directly to the ESP32. With PoE power, use only the Waveshare PoE module/ESP32-S3-POE-ETH variant with an IEEE 802.3af switch or injector. Do not use passive PoE, and leave BUS pin 2 disconnected.

## Wiring

Use an RJ12 / 6P6C plug or breakout. Ordinary RJ11 telephone leads are often 6P2C or 6P4C and may not expose the bus pins.

Verify the connector orientation before applying power. RJ12 pin numbers are mirror-imaged depending on whether you are looking into the socket, at the plug contacts, or at the cable from the latch side. Use continuity testing from your exact breakout/cable before connecting BUS pin 2, and leave BUS pin 2 disconnected during USB/PoE-powered development.

Expected HCP1 BUS pinout:

| BUS pin | Function |
| --- | --- |
| 1 | Unused / unknown, leave open |
| 2 | +24 V DC, leave disconnected when powering by USB-C or PoE |
| 3 | BUS GND / RS-485 reference |
| 4 | Unused / unknown, leave open |
| 5 | RS-485 DATA- / B / D- |
| 6 | RS-485 DATA+ / A / D+ |

Suggested wiring for the Waveshare TTL TO RS485 (C) isolated adapter:

| ESP32-S3-ETH / adapter | Connection |
| --- | --- |
| 3V3 | Adapter VCC |
| ESP32 GND | Adapter TTL-side GND only |
| GPIO17 TX | Adapter RXD |
| GPIO16 RX | Adapter TXD |
| Adapter A+ | BUS pin 6 |
| Adapter B- | BUS pin 5 |
| Adapter PE | BUS pin 3 |

On this adapter, `TXD` is the adapter's TTL transmit output and goes to ESP32 RX. `RXD` is the adapter's TTL receive input and goes to ESP32 TX. Do not swap these names by intuition; follow signal direction.

Do not tie ESP32 GND to BUS pin 3 when using the isolated adapter; use the adapter PE terminal as the RS-485-side signal reference. On this adapter, `PE` is the isolated RS-485-side signal ground. Do not connect adapter PE to mains earth, the Ethernet shield, or ESP32 GND; for this isolated adapter, PE goes only to Hörmann BUS pin 3. The adapter has no documented DE, /RE, or RTS pin, so the main control YAML omits `rts_pin` and assumes the adapter's built-in half-duplex direction handling. Verify that assumption during bring-up by confirming UAP1 scan/status responses on the bus; use a DE/RE transceiver on `GPIO18` if you need deterministic direction control.

Verified cable color map for the current RJ12 lead:

| Wire color | RJ12 pin | Connect to |
| --- | --- | --- |
| Red | 1 | Leave open |
| Grey | 2 | Leave open / insulate, +24 V |
| Yellow | 3 | Adapter PE |
| Green | 4 | Leave open |
| Clear | 5 | Adapter B- |
| White | 6 | Adapter A+ |

If you use a non-isolated bare transceiver such as MAX3485/SP3485/SN65HVD72 instead, use `GPIO18` for DE and /RE tied together, connect ESP32 GND to BUS pin 3, and add this to `uapbridge_esp`:

```yaml
rts_pin: GPIO18
```

For first tests, power the board from USB-C or IEEE 802.3af PoE and connect only PE, DATA-, and DATA+ to the opener. If no valid traffic appears, swap A/B once as a diagnostic step because some RS-485 modules label them inconsistently.

Choose exactly one board power source: USB-C, the Waveshare 802.3af PoE module/PoE variant with an IEEE 802.3af switch or injector, or a properly regulated and isolated 24 V to 5 V supply. Do not use passive PoE. For this PoE setup, do not use the opener's 24 V BUS pin.

Do not use GPIO9, GPIO10, GPIO11, GPIO12, GPIO13, or GPIO14 for the RS-485 adapter; those pins are occupied by the onboard W5500 Ethernet chip. Avoid GPIO4 through GPIO7 if you may use the TF-card slot, avoid GPIO19/GPIO20 because they are USB D-/D+, avoid GPIO21 because it drives the onboard RGB LED, and avoid strapping pins such as GPIO0/GPIO45/GPIO46 unless you explicitly account for boot behavior. If you switch from the isolated adapter to a bare DE/RE transceiver, `GPIO18` is the documented direction pin when no camera is attached.

The Waveshare TTL TO RS485 (C) includes an onboard 120 ohm termination option enabled by soldering. Leave it open for first tests. With all power disconnected, measure A/B resistance on the connected bus: about `120 ohm` means one terminator is already present, about `60 ohm` means two terminators are present, and much lower resistance suggests over-termination or a wiring fault. Enable the adapter terminator only if this adapter is physically at an unterminated end of the RS-485 segment. Do not add bias resistors unless scope traces or logs show an idle bus that is actually floating; the opener likely already biases the bus.

The ESPHome Ethernet configuration uses the Waveshare/ESPHome W5500 pin map:

| W5500 signal | ESP32-S3 GPIO |
| --- | --- |
| SCLK | GPIO13 |
| MOSI | GPIO11 |
| MISO | GPIO12 |
| CS | GPIO14 |
| INT | GPIO10 |
| RST | GPIO9 |

## Software

The component is vendored under [components](components). It is based on the ESP32-capable ESPHome port of `hoermann_door`, with local safety and diagnostic changes:

- ESP-IDF example config for the Waveshare ESP32-S3-ETH board.
- Ethernet-only networking through the onboard W5500. Wi-Fi is intentionally not configured because ESPHome does not allow Wi-Fi and Ethernet at the same time.
- Optional configurable RS-485 direction pin through `rts_pin` for bare DE/RE transceivers. Omit it for the Waveshare TTL TO RS485 (C) isolated adapter after validating its direction behavior on your board.
- `allow_remote_close`, default `false`, blocks explicit close commands during development.
- `allow_remote_impulse`, default `false`, blocks the generic impulse command because impulse can close an open door.
- `use_unverified_stop_command`, default `false`, avoids the E3-derived raw stop word and uses an impulse fallback only after a recent decoded moving broadcast.
- `require_fresh_broadcast_for_commands`, default `true`, blocks commands until a fresh CRC-valid HCP broadcast has been seen.
- `command_timeout`, default `1200ms`, expires queued one-shot commands if the drive does not poll.
- `diagnostic_mode`, default `false`, logs raw frames, CRC status, decoded status bits, command queueing, command sending, and state transitions when temporarily enabled for captures.
- `trust_light_feedback`, default `true`, uses the decoded HCP light bit as authoritative. The SupraMatic E2 YAML sets this to `false` because the observed one-byte E2 broadcasts do not carry reliable light state; the Home Assistant light is therefore optimistic and each UI state change sends one toggle command.
- `close_obstruction_grace`, default `5s`, latches the `obstruction_state` diagnostic when a full close estimate has elapsed but the HCP closed bit never appears. This covers the observed E2 display `5` behavior where no explicit HCP error bit was captured.
- `http_debug_port`, set to `8080` in the primary YAML, exposes the live UAP1 emulator monitor without adding a second UART reader. It streams raw RX, TX responses, decoded frames, command queue/block/send events, gaps, stats, and recent history while the emulator continues to answer the opener.
- `persistent_log`, default `false`, adds an on-demand filesystem-backed protocol capture on a dedicated SPIFFS partition. It stages compact binary records in RAM, periodically flushes them to flash, and can be enabled and dumped at runtime over HTTP so obstruction or error tests can be captured even if no browser stream is open.
- A passive CRC-valid frame scanner runs alongside the known UAP1 parser. It records the last valid HCP frame, counts status transitions, and counts/logs unknown CRC-valid frames so longer or undocumented E2 frames are visible without changing command behavior.
- `listen_only`, when set true, receives and logs HCP frames without answering scan/status requests. Use this only for first bus capture.
- `valid_broadcast_timeout`, default `10s`, clears the valid-broadcast diagnostic and marks state unknown if the bus goes stale.
- `got_valid_broadcast` is set only after a CRC-valid HCP broadcast.
- Unknown states are treated conservatively; the cover is never reported closed unless the closed bit is decoded. Toggle remains disabled; use the explicit impulse button for deliberate protocol testing.
- `obstruction_state` is a separate problem binary sensor for close-failure detection. While it is active, close, venting, and generic impulse are blocked; open remains allowed as the recovery direction and clears the latch when accepted or when HCP already reports open.
- Optional `time_based_position` support exposes a Shelly-style position-capable cover in Home Assistant. Position is estimated from calibrated open/close durations and corrected by decoded HCP end states.
- `api.reboot_timeout: 0s` is set in every YAML so proxy/monitor/main firmware does not reboot just because Home Assistant or an API client is offline.

Primary config: [supramatic-e2.yaml](supramatic-e2.yaml)

Minimal fallback config: [supramatic-e2-minimal.yaml](supramatic-e2-minimal.yaml)

Proxy-only config: [supramatic-e2-proxy.yaml](supramatic-e2-proxy.yaml)

HTTP monitor-only config: [supramatic-e2-monitor.yaml](supramatic-e2-monitor.yaml)

TTL-side swapped-RX monitor diagnostic: [supramatic-e2-monitor-rx17.yaml](supramatic-e2-monitor-rx17.yaml)

Passive monitor with adapter TX input held idle-high: [supramatic-e2-monitor-idle-tx.yaml](supramatic-e2-monitor-idle-tx.yaml)

`secrets.yaml` needs these keys:

```yaml
api_key_supramatic_e2: "..."
ota_password_supramatic_e2: "..."
proxy_auth_token: "..." # only needed when proxy allow_tx is enabled
```

[secrets.example.yaml](secrets.example.yaml) contains a syntactically valid placeholder for ESPHome validation. Generate a real ESPHome API encryption key before flashing hardware you will actually use.

Use the pinned ESPHome version from [pyproject.toml](pyproject.toml) and [uv.lock](uv.lock) when validating or building:

```bash
cp secrets.example.yaml secrets.yaml
uv run esphome config supramatic-e2.yaml
uv run esphome compile supramatic-e2.yaml
```

## OTA Updates

All YAML variants include ESPHome OTA over Ethernet using `ota_password_supramatic_e2`. The first flash normally still needs USB-C or another serial flashing path; after that, keep Ethernet and PoE connected and update over the network.

Update the currently running main firmware:

```bash
uv run esphome upload supramatic-e2.yaml --device supramatic-e2.local
```

Update the proxy or monitor firmware when that mode is already running:

```bash
uv run esphome upload supramatic-e2-proxy.yaml --device supramatic-e2-proxy.local
uv run esphome upload supramatic-e2-monitor.yaml --device supramatic-e2-monitor.local
```

When switching modes, target the hostname of the firmware currently running on the board, not the hostname in the YAML you are about to flash. For example, if the board is currently running the main firmware and you want to switch to monitor mode:

```bash
uv run esphome upload supramatic-e2-monitor.yaml --device supramatic-e2.local
```

After the reboot, the board will advertise the new hostname from the flashed YAML. Home Assistant entities will also change when switching between main, proxy, and monitor firmware because only the main firmware exposes the garage-door cover.

## Active Test Defaults

The primary YAML is currently configured for physically supervised active testing:

```yaml
uapbridge_esp:
  diagnostic_mode: false
  listen_only: false
  allow_remote_close: true
  allow_remote_impulse: true
  use_unverified_stop_command: true
  require_fresh_broadcast_for_commands: true
  trust_light_feedback: false
  auto_correction: false
```

This enables Home Assistant open, close, stop, venting, light, and generic impulse commands. Use this mode only while physically present at the door during bring-up. Temporarily set `diagnostic_mode: true` only when capturing protocol logs. Even with all commands enabled, the firmware still requires a fresh valid HCP broadcast, a known non-stopped state, and no active error/prewarn before it accepts movement commands. An active close-failure/obstruction latch blocks close, venting, and generic impulse while still allowing open as a recovery command.

After command behavior and state mapping are verified, set `allow_remote_impulse: false` again unless you explicitly need the generic impulse command. If remote closing should remain disabled after testing, set `allow_remote_close: false`.

In the primary E2 YAML, the light entity also uses `restore_mode: RESTORE_DEFAULT_OFF`. This avoids sending a light toggle during boot from a restored optimistic Home Assistant state. If the physical light and Home Assistant state ever get out of sync, toggle the light once from Home Assistant to realign the optimistic state.

Keep `auto_correction: false` for the SupraMatic E2 until the raw E2 state mapping is known. It is retained only as an upstream compatibility option and should not be used as a substitute for understanding an error or prewarn state.

## HCP Bring-Up Notes

Existing HCP1/UAP1 implementations do not send an unsolicited startup packet from the slave. The documented flow is opener-first: after opener power-up, the master sends broadcast status and slave-scan frames; once a UAP1-like slave answers its scan, the master sends status requests and expects status responses.

That means normal emulation can answer scans, but it will not create bus traffic if no bytes physically reach the ESP32. If both monitor mode and normal emulation show no RX at all, focus on the physical bus path, adapter power, adapter TTL pin direction, A/B polarity, BUS socket selection, opener accessory/bus enablement, and testing immediately after opener power-cycle.

Live SupraMatic E2 bring-up showed HCP traffic after an accidental A/B short was removed. This E2 emits one-byte broadcast status frames such as `00 00 01 01 C3` when the monitor includes the sync byte. The emulator accepts this format in addition to the two-byte broadcast shape used by the upstream E3-era mapping. For safety, a one-byte broadcast can confirm open/closed/moving bits in the first status byte, but unavailable second-byte bits such as prewarn remain false until an explicit E2 mapping is captured.

## Time-Based Position

The main YAML enables time-based position estimation so Home Assistant can show and set cover percentages similar to a Shelly 2PM in cover mode:

```yaml
cover:
  - platform: uapbridge
    time_based_position: true
    open_duration: 18s
    close_duration: 18s
    close_obstruction_grace: 5s
```

This is an estimate, not measured position data from the opener. Timing starts when the command is actually sent on the next HCP status response, or when HCP status reports movement. The firmware only reports exact `0%` after the HCP closed bit is decoded; timing alone is clamped above closed so Home Assistant is not told the door is definitely closed when it is not confirmed. If a close reaches the calibrated travel time and the HCP closed bit still does not appear after `close_obstruction_grace`, `Garage Door Obstruction State` turns on and the estimate remains non-closed. Intermediate position targets require a reliable stop path. See [docs/time-based-position.md](docs/time-based-position.md).

Because Home Assistant may hide the position slider for a `device_class: garage` cover, the primary YAML also exposes `Garage Door Target Position` as a number entity. Use that slider for percentage tests. `Garage Door Open Duration` and `Garage Door Close Duration` show the automatically learned runtime calibration values in seconds; clean full open/close runs update and persist them automatically.

The E2 light feedback bit is not available in the observed one-byte broadcasts. The light entity is optimistic for manual toggles, and the primary YAML enables courtesy-light estimation: when HCP state reports opening or closing, Home Assistant is told the light is on and the estimate expires after `courtesy_light_duration`.

## Interactive Test Wizard

Use [tools/garage_test_wizard.py](tools/garage_test_wizard.py) for a guided end-to-end garage test. It starts and stops the ESP persistent protocol logger, takes `/stats` and `/broadcast_status` snapshots, guides full-travel calibration, stop tests, percentage target tests, light checks, and an optional obstruction test, then saves a timestamped bundle under [captures](captures).

The wizard can drive Home Assistant automatically when a long-lived access token is provided:

```bash
HA_TOKEN="..." uv run garage-test-wizard
```

Without `HA_TOKEN`, it still controls ESP recording and pauses with exact manual Home Assistant instructions for each command. Every movement requires a manual confirmation before the next step.

For position calibration, measure the clear opening height in meters instead of guessing the percentage. The wizard asks for the full-open height once, then asks for the measured opening height after each stop/target. It computes actual percent open and error versus target in `measurements.csv` and `summary.md`.

For video-based curve fitting, print the A4 ArUco marker PDFs in [docs/markers](docs/markers). Use four fixed reference markers around the garage opening and one moving marker on the bottom edge of the door. Print at actual size and verify the `100 mm` scale bar before recording.

The first ArUco video analysis is stored in [docs/research/analysis/garage-door-motion-20260527](docs/research/analysis/garage-door-motion-20260527). It fits separate opening and closing curves from the bottom-segment marker path: opening about `10.215 s`, closing about `18.565 s`.

To align those video curves with real HCP endpoint timing, use the HCP-only calibration runner. It starts the ESP persistent protocol logger, sends full-travel commands through the ESPHome native API, downloads the protocol log, then writes an HCP-to-video timing report and overlay plot:

```bash
uv run garage-hcp-timing-calibration
```

The runner uses the API encryption key from `secrets.yaml` and asks for physical-presence confirmation before moving the door unless `--yes` is passed. The offline parser can also be run on any saved persistent log:

```bash
uv run garage-analyze-hcp-timing \
  --persistent-log captures/<bundle>/persistent-log.json \
  --curve-lookup docs/research/analysis/garage-door-motion-20260527/curve_lookup.json
```

For a new phone video capture with synchronized protocol logging, use the fullscreen sync display. Start with the door fully closed, start your phone recording with the MacBook screen visible, then press `Space`. The automatic sequence starts after a visible `15s` countdown and records full-open, full-close, vent-from-closed, open setup, vent-from-open, and final close movements while the screen prioritizes a near full-height QR timecode marker and keeps live HCP feedback in compact side text:

```bash
uv run garage-phone-sync --esp-host <local-ip>
```

Controls: `Space` starts the sequence or cancels a pending countdown, `M` emits a manual marker flash, and `Q`/`Esc` finishes the capture and downloads the ESP persistent log.

The marker is a compact version-1 QR code with a 16-character alphanumeric payload and payload CRC validation. Decode a recorded phone video with:

```bash
uv run garage-decode-phone-sync-video --video /path/to/PHONE_VIDEO.MOV
```

For timing alignment, prefer ESP-side persistent-log command/HCP timestamps over the Mac's command-request time. The QR maps video frames to the Mac display timeline; the persistent log maps command and HCP events on the ESP. Any network/API latency is handled by aligning matching command events from both logs rather than by applying a guessed fixed delay.

To verify the fullscreen visuals before moving the opener, run the same tool in dry-run mode. It does not connect to the ESP, does not start persistent logging, and simulates HCP state feedback locally so the automatic sequence can complete:

```bash
uv run garage-phone-sync --dry-run
```

## Protocol Diagnostics

The primary YAML exposes these diagnostic entities for protocol work:

- `Garage Door Raw HCP Status` and `Garage Door Raw HCP Status Hex`.
- `Garage Door Last HCP Frame`.
- `Garage Door HCP Status Transitions`.
- `Garage Door Unknown Valid HCP Frames`.

`Unknown Valid HCP Frames` should normally stay at zero. If it increments during obstruction, prewarn, venting, or light tests, dump `/recent`, `/stats`, and the persistent log before power-cycling; those frames are the best candidates for additional E2-specific decoding.

## Persistent Protocol Log

The main firmware can capture raw RX, TX, gaps, decoded frame candidates, unknown CRC-valid frames, command events, and status transitions into a dedicated `hcp_logs` SPIFFS filesystem while UAP1 emulation continues to run. Records are staged in RAM as compact binary data with repeat-count compression for identical adjacent records, then flushed to flash periodically. The HTTP dump expands the capture into readable JSON with timestamps, repeat counts, raw hex, CRC status, decoded status bits, and command names.

The default partition reserves 4 MB for captures and caps the active file at 3 MB, which is intended to hold at least five minutes of continuous HCP traffic. Use it only around diagnostic tests because it periodically writes flash while enabled:

```bash
curl http://supramatic-e2.local:8080/persistent_log/clear
curl http://supramatic-e2.local:8080/persistent_log/start
# reproduce the door state, obstruction, or command problem
curl http://supramatic-e2.local:8080/persistent_log/stop
curl --max-time 120 http://supramatic-e2.local:8080/persistent_log > persistent-log.json
```

If `format_required` is true after the partition is first added, initialize the filesystem once with `curl --max-time 120 http://supramatic-e2.local:8080/persistent_log/format`. If mDNS is unreliable, replace `supramatic-e2.local` with the current IP address. See [docs/persistent-protocol-log.md](docs/persistent-protocol-log.md).

## Proxy Mode

Flash [supramatic-e2-proxy.yaml](supramatic-e2-proxy.yaml) when you want the ESP32 to act only as an Ethernet proxy for the RS-485 bus. This mode does not load the UAP1 emulator and does not expose garage entities to Home Assistant.

Proxy mode listens on TCP port `6638` and streams line-based events such as `RX <seq> <micros> <hex>` and `GAP <micros> <gap-us>`. It validates that the UART is configured as `19200 8N1`. It defaults to receive-only and the default proxy YAML does not configure a UART TX pin. Active transmission requires adding `tx_pin`, setting `allow_tx: true`, and configuring `auth_token: !secret proxy_auth_token`; `TXB <hex>` generates the HCP sync break before writing bytes. TX is rejected unless the bus has been idle for the configured gap threshold.

Use the helper client:

```bash
uv run garage-hcp-proxy-client --host supramatic-e2-proxy.local
```

See [docs/proxy-mode.md](docs/proxy-mode.md) for the full TCP protocol. Passive capture is the primary use. Fully live laptop-side UAP1 emulation over TCP may miss the opener's poll response window, so keep timing-critical status response logic on the ESP32 unless measurements prove otherwise.

## HTTP Monitor Mode

Flash [supramatic-e2-monitor.yaml](supramatic-e2-monitor.yaml) when you want a read-only browser/curl-friendly live capture. This mode does not configure a UART TX pin, does not transmit to the bus, and does not load the UAP1 emulator.

If the adapter TTL pins may be crossed, flash [supramatic-e2-monitor-rx17.yaml](supramatic-e2-monitor-rx17.yaml). It is the same read-only monitor with UART RX moved from `GPIO16` to `GPIO17`.

If the RS-485 adapter's TTL input may be floating in pure receive-only mode, flash [supramatic-e2-monitor-idle-tx.yaml](supramatic-e2-monitor-idle-tx.yaml). It keeps `GPIO17` high as an idle TX input for the adapter, but still does not configure UART TX and does not send frames.

The swapped-RX monitor is only for proving a TTL-side wiring mistake. It is not the normal configuration for the verified wiring above.

Open:

```text
http://supramatic-e2-monitor.local:8080/
```

Or stream plain text:

```bash
curl -N http://supramatic-e2-monitor.local:8080/stream
uv run garage-hcp-proxy-client --host supramatic-e2-monitor.local --http-stream
```

The monitor also exposes `/events` as a Server-Sent Events stream, `/recent` for the in-memory capture tail, and `/stats` for JSON counters. See [docs/http-monitor-mode.md](docs/http-monitor-mode.md).

## Test Checklist

1. Flash ESP32 and verify it boots in ESPHome/Home Assistant before connecting to the opener.
2. Connect Ethernet and verify the device comes online via DHCP.
3. Connect PE, DATA-, DATA+ only; power the board via USB-C or PoE for the first bus test.
4. Open ESPHome logs.
5. Optional first capture: set `listen_only: true`, flash, power-cycle the opener, and confirm raw broadcasts before allowing the ESP32 to answer the bus. Then set `listen_only: false` and flash again for control tests.
6. Power-cycle the SupraMatic opener.
7. Confirm broadcast frames are received.
8. Confirm `Valid HCP Broadcast` becomes true.
9. Confirm slave scan and status request frames are detected.
10. Confirm the emulator responds as UAP1 address `0x28`.
11. Test the light command first if the opener supports it. On an E2 with `trust_light_feedback: false`, one Home Assistant state change should produce one `CMD ... queued toggle_light` event followed by one `CMD ... sent toggle_light` event in `http://<device>:8080/recent`.
12. Test the cover open command while physically present at the door. It should produce `CMD ... queued open` and then `CMD ... sent open`.
13. Test stop while the door is already moving and while physically present at the door.
14. Enable `allow_remote_close: true` only after state feedback is reliable and safety devices are verified. Until then, close commands are expected to show `CMD ... blocked close ... reason=close_disabled`.
15. Test open and close while physically present at the door.
16. Enable and test `allow_remote_impulse: true` only if you explicitly need the generic impulse command for protocol diagnosis.
17. Log and verify states for closed, opening, open, closing, stopped halfway, venting/partial-open, light on/off, obstruction/close-failure, and any error/prewarn state.
18. Only after reliable state and safety behavior, expose the cover through Home Assistant's HomeKit Bridge.

## Capturing E2 State Logs

Use:

```bash
uv run esphome logs supramatic-e2.yaml
```

Temporarily enable `diagnostic_mode: true`, or flash the monitor/proxy firmware for passive capture, then collect logs while moving the door through:

- Fully closed.
- Opening.
- Fully open.
- Closing.
- Stopped halfway.
- Venting / partial-open.
- Light on and off.
- Obstruction/close-failure and error/prewarn, if available.

For each state, copy the lines containing:

- `broadcast crc=ok data=...`
- `Decoded status 0x....`
- `State changed`
- `Queued one-shot HCP command`
- `Sending one-shot HCP command`

If E2 state decoding differs from the E3-derived mapping, those raw status frames are the data needed to adjust the bit mapping without changing the hardware.

ESPHome exposes `Garage Door Obstruction State`, but the HomeKit garage-door obstruction characteristic is a Home Assistant HomeKit Bridge setting. In Home Assistant, link `binary_sensor.garage_door_obstruction_state` as `linked_obstruction_sensor` for `cover.garage_door` instead of exposing it as a separate Apple Home sensor. Include the garage cover and optionally the garage light. Exclude venting controls, diagnostics, raw state helpers, and generic error/prewarn sensors from HomeKit unless you explicitly want them as separate sensors. The default YAML does not expose a generic impulse button; add one only for deliberate protocol diagnosis.

## Notes

- The HCP1 UART is `19200 8N1`.
- The drive is the master; the ESP32 behaves as a UAP1 slave at address `0x28`.
- Known protocol values used here include master address `0x80`, broadcast `0x00`, scan command `0x01`, status request `0x20`, status response `0x29`, and CRC-8 polynomial `0x07` with initial value `0xF3`.
- Command responses keep the expected normal bit `0x1000` set and send command bits once on the next status response.
- This is not a Series 4/HCP2 bridge and does not target UAP1-HCP.

## Upstream

The ESPHome component was initially derived from the MIT-licensed `14yannick/hoermann_door` ESPHome UAP1 emulator. The `Tysonpower/hoermann_door` fork was checked as well for current ESPHome compatibility fixes. The HCP2 `HCPBridgeMqtt` family was checked only to confirm it targets different Series 4 / UAP1-HCP use cases and is not the basis for this E2/HCP1 implementation.
