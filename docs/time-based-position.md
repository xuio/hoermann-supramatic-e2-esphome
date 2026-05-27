# Time-Based Position Estimation

The SupraMatic E2/HCP1 status frames may not expose a trustworthy continuous door position. The main firmware can therefore expose a Shelly-style position-capable Home Assistant cover by estimating position from travel time.

This is enabled in [supramatic-e2.yaml](supramatic-e2.yaml):

```yaml
cover:
  - platform: uapbridge
    name: "${friendly_name}"
    device_class: garage
    time_based_position: true
    open_duration: 10215ms
    close_duration: 18565ms
    open_start_delay: 2500ms
    close_start_delay: 1500ms
    open_report_delay: 600ms
    close_report_delay: 2600ms
    close_obstruction_grace: 5s
    position_publish_interval: 1s
    position_deadband: 2%
    venting_position: 3.6%
    learn_travel_durations: false
    use_motion_curve: true
```

Home Assistant sees a normal cover position where `0%` is closed and `100%` is open. This matches the convention used by Home Assistant cover entities and Shelly cover mode.

The garage door cover advertises `supports_position: true`, but some Home Assistant garage-door cards do not expose a percentage slider for `device_class: garage`. The main YAML therefore also exposes a separate `Garage Door Target Position` number entity. Use that number slider to send percentage targets directly while keeping the main entity as a garage-door cover for HomeKit Bridge.

The SupraMatic E2 defaults above are calibrated from the 2026-05-27 ArUco marker videos and the first synchronized phone/HCP run:

- `open_duration` / `close_duration`: visible door motion only, not command-to-HCP-end time.
- `open_start_delay` / `close_start_delay`: delay between the HCP command being transmitted and visible door motion beginning.
- `open_report_delay` / `close_report_delay`: delay between visible end of travel and the HCP end-state report.
- `use_motion_curve`: interpolate through the measured soft-start/soft-stop curve instead of assuming a linear position over time.
- `venting_position`: used only if a future HCP status frame decodes as native venting; current position calibration does not depend on the vent command.

The main E2 YAML keeps automatic travel-duration learning disabled so the video-derived timing values remain authoritative. The component still exposes configuration numbers:

- `Garage Door Open Duration`, in seconds.
- `Garage Door Close Duration`, in seconds.
- `Garage Door Open Start Delay` and `Garage Door Close Start Delay`, in seconds.
- `Garage Door Open Report Delay` and `Garage Door Close Report Delay`, in seconds.

These values show the active calibration from the cover component. You can still override them manually from Home Assistant; manual changes are persisted by the cover component as well.

## Behavior

- Fully open and fully close still use the native HCP open/close commands.
- Intermediate targets move in the needed direction, estimate travel progress, then send stop at the estimated target.
- The movement timer is armed by the Home Assistant command, but it does not start counting from the request timestamp. It starts after the one-shot HCP command is actually sent in a status response. When moving away from a known `Open` or `Closed` end state, the estimator also waits until the HCP status leaves that old endpoint; this prevents timed stop/follow-up commands if the opener ignores a close/open command.
- The visible position remains at the start value during the configured start delay, then advances through the configured empirical motion curve over the visible motion duration.
- During the short start/prewarn window after a command is sent, old opposite end-state broadcasts are ignored so an intermediate target is not discarded before movement is reported.
- If HCP stays at the old endpoint for several seconds after an endpoint-departure command, the firmware cancels the estimate and leaves the position at the known endpoint instead of pretending movement happened.
- Ambiguous E2 `stopped` / `0x0000` status is ignored while an estimated movement is active. Captures showed the E2 can emit that value during a close attempt, so treating it as a real stop breaks calibration and percentage control.
- New targets in the current travel direction retarget the active estimate. A target in the opposite direction first sends stop and requires a second explicit command after the door stops.
- After an estimated intermediate stop, follow-up open/close commands are allowed from the cover's own estimated intermediate position even if the HCP broadcast still decodes as `0x0000`; this is required for reset moves between percentage tests.
- The estimate is corrected to `0%` or `100%` when the HCP status decoder sees closed or open. Native venting is intentionally not part of the current calibration path; use normal percentage targets for partial-open testing.
- The firmware restores the last estimated cover position after reboot, but a restored exact closed value is clamped above `0%` until the HCP closed bit is decoded again.
- Full open/close timing completion keeps the cover operation as opening/closing until the corresponding HCP end-state bit confirms the final state.
- For a full close, if the visible close duration plus `close_report_delay` plus `close_obstruction_grace` elapses without the HCP closed bit, the firmware latches `Garage Door Obstruction State`, stops the estimate at a conservative non-closed value, and blocks further close/venting/impulse commands. An explicit open command is still allowed as recovery and clears the latch when accepted, or when HCP already reports open.
- If a full end-to-end travel starts from the opposite end and reaches the expected end state, the firmware subtracts the configured start/report delays and stores the learned visible travel duration in flash.

## Safety Limits

- Timed position is an estimate, not a measured encoder position.
- The firmware will not publish exact `0%` closed from timing alone. It only publishes `0%` after the HCP closed bit is decoded.
- The obstruction latch is inferred from a failed timed close, not from a confirmed E2 error-code field. Keep protocol logging available during further obstruction tests so a future explicit E2 error bit can replace or refine the inference if one is found.
- `position_deadband` is limited to `20%` or less at ESPHome config validation time so a broad deadband cannot hide large position errors.
- Downward position moves require `allow_remote_close: true`; the default YAML keeps it `false` until you have verified state decoding and obstruction protection.
- Intermediate target stopping relies on the stop command path. On this E2, movement broadcasts can decode as `0x0000` / stopped while the door is physically moving, so percentage control requires `use_unverified_stop_command: true`; that allows the raw HCP stop command even when the moving bit is not present, while still requiring a fresh valid bus broadcast.
- Do not expose position control to HomeKit until open, close, stop, and state correction have been tested while physically present.

## Calibration

1. Start fully closed.
2. Open fully and wait for the HCP open end state.
3. Close fully and wait for the HCP closed end state.
4. Check `Garage Door Open Duration` and `Garage Door Close Duration` in Home Assistant; they should reflect visible motion time, not command-to-end-state time.
5. Leave the start/report delays at the measured E2 defaults unless a synchronized video shows a consistent offset.
6. Test `75%`, `50%`, and `25%` with the `Garage Door Target Position` number while standing at the door.
7. If a value is obviously wrong, enter a corrected duration manually after analyzing synchronized video/HCP data.

For repeatable position tests, use [tools/garage_test_wizard.py](tools/garage_test_wizard.py). It records full-open clear height in meters and asks for measured clear opening height after each target. The saved CSV gives actual position and target error without guessing percentages by eye.

The Loxone Hörmann Air adapter is a useful reference point: it integrates through the Hörmann BUS, supports the Garage/Gate block including partially-open input, and documents automatic learning of travel durations. This firmware follows the same practical model, but keeps the timing estimate local and inspectable in ESPHome logs.

## HCP / Video Timing Alignment

Use [tools/run_hcp_timing_calibration.py](tools/run_hcp_timing_calibration.py) when the video curve has already been extracted and you only need a normal HCP run. It commands a full open/close sequence through the ESPHome native API, captures the ESP persistent protocol log, and then calls [tools/analyze_hcp_timing.py](tools/analyze_hcp_timing.py).

The analyzer treats the first HCP open or closed endpoint bit after a command as the fully stopped endpoint, then shifts the ArUco curve so its end aligns to that HCP endpoint. The reported offset is therefore a combined command-to-motion-start and endpoint-report offset; splitting those requires simultaneous video and HCP capture.

For a simultaneous phone video run, start with the door fully closed and use [tools/run_phone_video_sync_capture.py](tools/run_phone_video_sync_capture.py). It opens a fullscreen Tk display on the MacBook, starts the ESP persistent HCP logger, records a near full-height compact QR timecode timeline, and runs the default `position_targets` sequence after `Space`: full open, full close, `25%` from closed, full close, `50%` from closed, full close, full open, `75%` from open, full open, `50%` from open, and final close. Each command is separated by a timed delay and settle period.

For a second position-only run, use `--sequence position_targets_no_calibration`. It assumes the door starts fully closed and skips the initial full-open/full-close calibration pair, then tests `25%`, `50%`, and `75%` from closed plus `75%`, `50%`, and `25%` from open with endpoint resets between targets. The runner uses a `1s` settle delay between phases by default.

Use `--dry-run` first if you only want to check the fullscreen layout and QR code. Dry run skips all ESPHome API and HTTP calls, sends no opener commands, and simulates HCP state changes locally so the sequence advances without hardware.

If you press `Q`/`Esc` while the cover is still moving, the runner sends a stop command and waits briefly before it stops the persistent log and downloads files. This avoids immediately loading the ESP with HTTP transfers while the SupraMatic still expects live UAP1 responses. The normal capture downloads the compact binary persistent log only; use `--download-json-log` only for offline debugging when the bus is idle.

The old native-venting capture preset is still available with `--sequence full_and_vent`, but it is not used for the current percentage-control calibration. If position control is good enough, partial-open behavior should be tested as a normal cover target.

The capture display keeps the QR code and keepout area clear, with rotated status text only in the left and right gutters. The far-left vertical bar shows current-step progress, and the far-right vertical bar shows total automation progress. The QR payload is a 10-character alphanumeric base36 code in a version-1 symbol with a standard quiet zone, strong error correction, and CRC validation. Use `--hide-side-status` for a QR-only screen, or `--show-overlay-text` only for local debugging when you are not recording calibration video.

The QR timecode can be decoded from the phone video with `uv run garage-decode-phone-sync-video --video /path/to/PHONE_VIDEO.MOV`. Frames are accepted only when OpenCV decodes a valid garage-sync payload and the payload CRC matches. For timing alignment, do not apply a guessed fixed network-latency correction; align matching command events from the Mac visual timeline and the ESP persistent log.
