# Motion Model Fit Inputs - 2026-05-27

This report extends the AUTO_04 analysis with the earlier timing-aligned full-travel capture only. Standalone non-HCP video timing is intentionally not used as an independent timing source here.

## Sources Used

- Prior timing-aligned full-travel capture: `captures/hcp-timing-calibration-20260527-164141/hcp-video-timing-alignment.json`
- Successful position run: `docs/research/analysis/phone-sync-auto04-20260527/summary.json`
- AUTO_04 video-derived position samples: `docs/research/analysis/phone-sync-auto04-20260527/auto04_progress_5fps.csv`

## Full-Travel Timing

| Source | Direction | Command to HCP endpoint | Visible motion duration | Combined offset |
| --- | --- | ---: | ---: | ---: |
| prior timing-aligned | opening | 12.825s | 10.215s | 2.610s |
| prior timing-aligned | closing | 22.274s | 18.565s | 3.709s |
| AUTO_04 | opening | 20.592s | derived from crossings, not a full visible-duration fit here | n/a |

## AUTO_04 Opening Crossings

These are measured from the HCP open command in AUTO_04 to the first video-derived crossing of each position.

| Position | Time after command |
| ---: | ---: |
| 1% | 1.415s |
| 5% | 3.015s |
| 10% | 4.015s |
| 20% | 5.815s |
| 25% | 6.615s |
| 30% | 7.415s |
| 35% | 8.015s |
| 40% | 8.814s |
| 50% | 10.414s |
| 75% | 14.214s |
| 90% | 16.414s |
| 95% | 17.414s |
| 99% | 19.414s |

## AUTO_04 Close Crossings From Open

AUTO_04 did not include a full 100% to 0% close from open, but the descending target segments provide repeatable close-curve samples.

| Position | Median time after close command | Samples |
| ---: | ---: | ---: |
| 99% | 0.674s | 3 |
| 90% | 2.284s | 3 |
| 75% | 5.074s | 3 |
| 50% | 9.279s | 2 |
| 25% | 13.674s | 1 |

## Target Results From AUTO_04

| Step | Target | Measured | Error |
| --- | ---: | ---: | ---: |
| target_25_from_closed | 25% | 37.1% | +12.1 pp |
| target_50_from_closed | 50% | 37.1% | -12.9 pp |
| target_75_from_closed | 75% | 51.0% | -24.0 pp |
| target_75_from_open | 75% | 66.6% | -8.4 pp |
| target_50_from_open | 50% | 42.2% | -7.8 pp |
| target_25_from_open | 25% | 18.4% | -6.6 pp |

## Interpretation

The close model is consistent across the timing-aligned full-close capture and AUTO_04 close-from-open samples. The prior timing-aligned close duration of about 18.6s remains a good full-close visible-duration constraint; AUTO_04 mainly shows that the command-to-motion start delay should be shorter than the current YAML value.

The opening data is not consistent as a single fixed duration. The earlier timing-aligned full-open capture reports a 12.825s command-to-HCP-open interval and a 10.215s visible motion duration. AUTO_04 reports a 20.592s command-to-HCP-open interval for a full open. That difference is too large to solve by tweaking a stop lead or smoothing curve.

Firmware implication: do not average these opening durations. The next firmware change should either use the latest timing-aligned AUTO_04 opening behavior for position control, or add an explicit adaptive calibration gate that validates the current full-open duration before using percentage targets.

![Motion model timing comparison](motion_model_timing_comparison.png)
