# Time-Based Position Estimation

The SupraMatic E2/HCP1 status frames may not expose a trustworthy continuous door position. The main firmware can therefore expose a Shelly-style position-capable Home Assistant cover by estimating position from travel time.

This is enabled in [supramatic-e2.yaml](supramatic-e2.yaml):

```yaml
cover:
  - platform: uapbridge
    name: "${friendly_name}"
    device_class: garage
    time_based_position: true
    open_duration: 18s
    close_duration: 18s
    close_obstruction_grace: 5s
    position_publish_interval: 1s
    position_deadband: 2%
    venting_position: 20%
    learn_travel_durations: true
```

Home Assistant sees a normal cover position where `0%` is closed and `100%` is open. This matches the convention used by Home Assistant cover entities and Shelly cover mode.

The garage door cover advertises `supports_position: true`, but some Home Assistant garage-door cards do not expose a percentage slider for `device_class: garage`. The main YAML therefore also exposes a separate `Garage Door Target Position` number entity. Use that number slider to send percentage targets directly while keeping the main entity as a garage-door cover for HomeKit Bridge.

The cover component automatically learns and persists travel durations when it sees a complete full travel from one end state to the other. The main YAML also exposes two configuration numbers:

- `Garage Door Open Duration`, in seconds.
- `Garage Door Close Duration`, in seconds.

These values show the active learned durations from the cover component. You can still override them manually from Home Assistant; manual changes are persisted by the cover component as well.

## Behavior

- Fully open and fully close still use the native HCP open/close commands.
- Intermediate targets move in the needed direction, estimate travel progress, then send stop at the estimated target.
- The movement timer is armed by the Home Assistant command, but it does not start counting from the request timestamp. It starts after the one-shot HCP command is actually sent in a status response, or when decoded HCP status reports opening/closing.
- During the short start/prewarn window after a command is sent, old opposite end-state broadcasts are ignored so an intermediate target is not discarded before movement is reported.
- Ambiguous E2 `stopped` / `0x0000` status is ignored while an estimated movement is active. Captures showed the E2 can emit that value during a close attempt, so treating it as a real stop breaks calibration and percentage control.
- New targets in the current travel direction retarget the active estimate. A target in the opposite direction first sends stop and requires a second explicit command after the door stops.
- The estimate is corrected to `0%`, `100%`, or the configured venting percentage when the HCP status decoder sees closed, open, or venting.
- The firmware restores the last estimated cover position after reboot, but a restored exact closed value is clamped above `0%` until the HCP closed bit is decoded again.
- Full open/close timing completion keeps the cover operation as opening/closing until the corresponding HCP end-state bit confirms the final state.
- For a full close, if the close duration plus `close_obstruction_grace` elapses without the HCP closed bit, the firmware latches `Garage Door Obstruction State`, stops the estimate at a conservative non-closed value, and blocks further close/venting/impulse commands. An explicit open command is still allowed as recovery and clears the latch when accepted, or when HCP already reports open.
- If a full end-to-end travel starts from the opposite end and reaches the expected end state, the firmware automatically stores the learned travel duration in flash.

## Safety Limits

- Timed position is an estimate, not a measured encoder position.
- The firmware will not publish exact `0%` closed from timing alone. It only publishes `0%` after the HCP closed bit is decoded.
- The obstruction latch is inferred from a failed timed close, not from a confirmed E2 error-code field. Keep protocol logging available during further obstruction tests so a future explicit E2 error bit can replace or refine the inference if one is found.
- `position_deadband` is limited to `20%` or less at ESPHome config validation time so a broad deadband cannot hide large position errors.
- Downward position moves require `allow_remote_close: true`; the default YAML keeps it `false` until you have verified state decoding and obstruction protection.
- Intermediate target stopping relies on the existing safe stop path. With the default `use_unverified_stop_command: false`, the firmware sends the impulse stop fallback only after a recent decoded moving broadcast. If E2 moving-state decoding is wrong or stale, an intermediate stop may be rejected and the door may continue to an end stop.
- Do not expose position control to HomeKit until open, close, stop, and state correction have been tested while physically present.

## Calibration

1. Start fully closed.
2. Open fully and wait for the HCP open end state. The firmware stores the measured open duration automatically.
3. Close fully and wait for the HCP closed end state. The firmware stores the measured close duration automatically.
4. Check `Garage Door Open Duration` and `Garage Door Close Duration` in Home Assistant; they should reflect the learned values.
5. Test `75%`, `50%`, and `25%` with the `Garage Door Target Position` number while standing at the door.
6. If a value is obviously wrong because the run was interrupted, enter a corrected duration manually or repeat a clean full run.

For repeatable position tests, use [tools/garage_test_wizard.py](tools/garage_test_wizard.py). It records full-open clear height in meters and asks for measured clear opening height after each target. The saved CSV gives actual position and target error without guessing percentages by eye.

The Loxone Hörmann Air adapter is a useful reference point: it integrates through the Hörmann BUS, supports the Garage/Gate block including partially-open input, and documents automatic learning of travel durations. This firmware follows the same practical model, but keeps the timing estimate local and inspectable in ESPHome logs.

## HCP / Video Timing Alignment

Use [tools/run_hcp_timing_calibration.py](tools/run_hcp_timing_calibration.py) when the video curve has already been extracted and you only need a normal HCP run. It commands a full open/close sequence through the ESPHome native API, captures the ESP persistent protocol log, and then calls [tools/analyze_hcp_timing.py](tools/analyze_hcp_timing.py).

The analyzer treats the first HCP open or closed endpoint bit after a command as the fully stopped endpoint, then shifts the ArUco curve so its end aligns to that HCP endpoint. The reported offset is therefore a combined command-to-motion-start and endpoint-report offset; splitting those requires simultaneous video and HCP capture.

For a simultaneous phone video run, start with the door fully closed and use [tools/run_phone_video_sync_capture.py](tools/run_phone_video_sync_capture.py). It opens a fullscreen Tk display on the MacBook, starts the ESP persistent HCP logger, records a visual timecode timeline, shows live HCP feedback, and runs an automatic sequence after `Space`: full open, full close, venting from closed, open setup, venting from open, and final close. Each command is separated by visible countdown and settle periods.

Use `--dry-run` first if you only want to check the fullscreen layout and visual code. Dry run skips all ESPHome API and HTTP calls, sends no opener commands, and simulates HCP state changes locally so the sequence advances without hardware.
