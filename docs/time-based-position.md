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

## Behavior

- Fully open and fully close still use the native HCP open/close commands.
- Intermediate targets move in the needed direction, estimate travel progress, then send stop at the estimated target.
- New targets in the current travel direction retarget the active estimate. A target in the opposite direction first sends stop and requires a second explicit command after the door stops.
- The estimate is corrected to `0%`, `100%`, or the configured venting percentage when the HCP status decoder sees closed, open, or venting.
- The firmware restores the last estimated cover position after reboot.
- If a full end-to-end travel starts from the opposite end and reaches the expected end state, the firmware logs a learned travel duration for that boot.

## Safety Limits

- Timed position is an estimate, not a measured encoder position.
- The firmware will not publish exact `0%` closed from timing alone. It only publishes `0%` after the HCP closed bit is decoded.
- Downward position moves require `allow_remote_close: true`; the default YAML keeps it `false` until you have verified state decoding and obstruction protection.
- Intermediate target stopping relies on the existing safe stop path. With the default `use_unverified_stop_command: false`, the firmware sends the impulse stop fallback only while the door is decoded as moving. If E2 moving-state decoding is wrong, an intermediate stop may be rejected and the door may continue to an end stop.
- Do not expose position control to HomeKit until open, close, stop, and state correction have been tested while physically present.

## Calibration

1. Keep `allow_remote_close: false`.
2. Start with a conservative `open_duration` and `close_duration` slightly longer than the real travel time.
3. Open from fully closed and watch for a log line like `Learned full open travel duration`.
4. After verifying remote close safety, set `allow_remote_close: true`, close from fully open, and watch for `Learned full close travel duration`.
5. Copy stable learned values into `open_duration` and `close_duration`.
6. Test `75%`, `50%`, and `25%` targets while standing at the door.

The Loxone Hörmann Air adapter is a useful reference point: it integrates through the Hörmann BUS, supports the Garage/Gate block including partially-open input, and documents automatic learning of travel durations. This firmware follows the same practical model, but keeps the timing estimate local and inspectable in ESPHome logs.
