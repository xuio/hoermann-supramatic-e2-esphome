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
    position_publish_interval: 1s
    position_deadband: 2%
    venting_position: 20%
    learn_travel_durations: true
```

Home Assistant sees a normal cover position where `0%` is closed and `100%` is open. This matches the convention used by Home Assistant cover entities and Shelly cover mode.

The garage door cover advertises `supports_position: true`, but some Home Assistant garage-door cards do not expose a percentage slider for `device_class: garage`. The main YAML therefore also exposes a separate `Garage Door Target Position` number entity. Use that number slider to send percentage targets directly while keeping the main entity as a garage-door cover for HomeKit Bridge.

The main YAML also exposes two configuration numbers:

- `Garage Door Open Duration`, in seconds.
- `Garage Door Close Duration`, in seconds.

These values are restored by ESPHome and applied to the position estimator on boot, so you can tune travel timing from Home Assistant without reflashing after every calibration run.

## Behavior

- Fully open and fully close still use the native HCP open/close commands.
- Intermediate targets move in the needed direction, estimate travel progress, then send stop at the estimated target.
- The movement timer is armed by the Home Assistant command, but it does not start counting from the request timestamp. It starts after the one-shot HCP command is actually sent in a status response, or when decoded HCP status reports opening/closing.
- During the short start/prewarn window after a command is sent, old opposite end-state broadcasts are ignored so an intermediate target is not discarded before movement is reported.
- New targets in the current travel direction retarget the active estimate. A target in the opposite direction first sends stop and requires a second explicit command after the door stops.
- The estimate is corrected to `0%`, `100%`, or the configured venting percentage when the HCP status decoder sees closed, open, or venting.
- The firmware restores the last estimated cover position after reboot, but a restored exact closed value is clamped above `0%` until the HCP closed bit is decoded again.
- Full open/close timing completion keeps the cover operation as opening/closing until the corresponding HCP end-state bit confirms the final state.
- If a full end-to-end travel starts from the opposite end and reaches the expected end state, the firmware logs a learned travel duration for that boot.

## Safety Limits

- Timed position is an estimate, not a measured encoder position.
- The firmware will not publish exact `0%` closed from timing alone. It only publishes `0%` after the HCP closed bit is decoded.
- `position_deadband` is limited to `20%` or less at ESPHome config validation time so a broad deadband cannot hide large position errors.
- Downward position moves require `allow_remote_close: true`; the default YAML keeps it `false` until you have verified state decoding and obstruction protection.
- Intermediate target stopping relies on the existing safe stop path. With the default `use_unverified_stop_command: false`, the firmware sends the impulse stop fallback only after a recent decoded moving broadcast. If E2 moving-state decoding is wrong or stale, an intermediate stop may be rejected and the door may continue to an end stop.
- Do not expose position control to HomeKit until open, close, stop, and state correction have been tested while physically present.

## Calibration

1. Keep `allow_remote_close: false`.
2. Start with a conservative `open_duration` and `close_duration` slightly longer than the real travel time.
3. Open from fully closed and watch for a log line like `Learned full open travel duration`.
4. After verifying remote close safety, set `allow_remote_close: true`, close from fully open, and watch for `Learned full close travel duration`.
5. Enter stable measured values into `Garage Door Open Duration` and `Garage Door Close Duration`.
6. Test `75%`, `50%`, and `25%` with the `Garage Door Target Position` number while standing at the door.
7. Once the values are stable, optionally copy them back into `open_duration` and `close_duration` in YAML so the defaults match the calibrated values after a clean flash.

The Loxone Hörmann Air adapter is a useful reference point: it integrates through the Hörmann BUS, supports the Garage/Gate block including partially-open input, and documents automatic learning of travel durations. This firmware follows the same practical model, but keeps the timing estimate local and inspectable in ESPHome logs.
