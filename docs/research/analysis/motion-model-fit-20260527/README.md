# Motion Model Fit Inputs - 2026-05-27

This report uses the earlier timing-aligned full-travel capture as the only full-open/full-close calibration source. AUTO_04 is treated only as an interrupted-position-target run. Its reset/end commands are not used as full-travel calibration observations.

## Sources Used

- Full-travel timing-aligned capture: `captures/hcp-timing-calibration-20260527-164141/hcp-video-timing-alignment.json`
- Interrupted target run: `docs/research/analysis/phone-sync-auto04-20260527/summary.json`
- AUTO_04 video-derived position samples: `docs/research/analysis/phone-sync-auto04-20260527/auto04_progress_5fps.csv`

## Full-Travel Calibration

| Direction | Command to HCP endpoint | Visible motion duration | Combined offset |
| --- | ---: | ---: | ---: |
| opening | 12.825s | 10.215s | 2.610s |
| closing | 22.274s | 18.565s | 3.709s |

## AUTO_04 Interrupted Stop Observations

These rows describe the behavior we need for percentage targets: where the door was when the firmware sent stop, and where it finally settled.

| Step | Target | Stop delay | Position at stop | Settled position | Error | Settle after stop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| target_25_from_closed | 25% | 5.749s | 35.4% | 37.1% | +12.1 pp | 0.315s |
| target_50_from_closed | 50% | 8.211s | 36.3% | 37.1% | -12.9 pp | 0.274s |
| target_75_from_closed | 75% | 10.402s | 49.9% | 51.0% | -24.0 pp | 0.253s |
| target_75_from_open | 75% | 6.257s | 67.3% | 66.6% | -8.4 pp | 0.254s |
| target_50_from_open | 50% | 10.443s | 42.8% | 42.2% | -7.8 pp | 0.247s |
| target_25_from_open | 25% | 14.880s | 18.7% | 18.4% | -6.6 pp | 0.113s |

## Model Implication

The door needs two models, not one:

1. Planned endpoint profile for full open/full close. This uses the timing-aligned capture and remains corrected by HCP open/closed endpoint bits.
2. Interrupted target profile for percentage positions. This must predict the final settled position after an abrupt stop command. It should not use the planned endpoint S-curve directly, because the motor does not get to execute its planned deceleration profile.

A practical firmware implementation is to compute target stops from `predicted_settled_position`, not from instantaneous estimated position:

```text
predicted_settled_position = interrupted_profile(position_at_stop, velocity_at_stop, direction)
stop when predicted_settled_position reaches requested target
```

For the first implementation, the interrupted profile can be a small empirical table per direction/start endpoint derived from AUTO_04 stop observations. Later captures can add more points without changing the protocol layer.

![Motion model timing comparison](motion_model_timing_comparison.png)
