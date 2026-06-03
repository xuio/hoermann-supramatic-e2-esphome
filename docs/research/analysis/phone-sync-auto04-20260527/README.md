# Phone Sync AUTO_04 Analysis

Source video: `uncommitted-videos/phone-sync-auto04.mov`

Source capture bundle: `uncommitted-capture-bundles/phone-sync-auto04`

This was a successful `position_targets_no_calibration` run. The QR fit maps video time to ESP run time as:
`run_elapsed_s = 0.99997635 * video_time_s + 17.862`.

## Position Results

| Sequence step | Firmware target/state | Video-derived position | Error |
| --- | ---: | ---: | ---: |
| target_25_from_closed | 25% | 37.1% | +12.1 pp |
| reached_closed | Closed | 0.1% |  |
| target_50_from_closed | 50% | 37.1% | -12.9 pp |
| reached_closed | Closed | 0.1% |  |
| target_75_from_closed | 75% | 51.0% | -24.0 pp |
| reached_closed | Closed | 0.1% |  |
| reached_open | Open | 100.0% |  |
| target_75_from_open | 75% | 66.6% | -8.4 pp |
| reached_open | Open | 100.0% |  |
| target_50_from_open | 50% | 42.2% | -7.8 pp |
| reached_open | Open | 100.0% |  |
| target_25_from_open | 25% | 18.4% | -6.6 pp |
| reached_closed | Closed | 0.1% |  |

## Protocol Log

- Records decoded: `11748`.
- Decode drops: `0` records, `0` bytes.
- Unknown CRC-valid frames: `0`.
- Firmware log storage for this run was PSRAM-only (`flash_writes:false` in the start/stop summaries).

## Artifacts

- `summary.json`: complete alignment, endpoint windows, event comparison, and protocol summary.
- `auto04_position_timeline.png`: video-derived position and firmware estimate timeline.
- `auto04_event_contact_sheet.jpg`: representative frames at sequence milestones.
- `auto04_progress_5fps.csv`: 5 fps marker-progress samples.
- `auto04_event_comparison.csv`: sequence milestones compared against video-derived position.
- `auto04_command_positions.csv`: command send timestamps compared against video-derived position.
- `qr_decode_stride6.jsonl`: accepted QR timecode samples.
- `aruco_raw_5fps.json`: raw ArUco detections from the 5 fps working video.
