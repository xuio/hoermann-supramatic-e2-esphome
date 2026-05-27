# Phone Sync AUTO_02 Analysis

Source video: `uncommitted-videos/phone-sync-auto02.mov`

Source capture bundle: `captures/phone-sync-capture-20260527-204730`

This was the second automatic position-target run using `position_targets_no_calibration`.
The video starts at run elapsed `34.9s` and ends at about `184.2s`. The ESP log
continues beyond the video and shows the final `close` command was not sent until
run elapsed `191.4s`, so the video does not contain the final close.

The HCP persistent log has no dropped records. It shows that all commands were
queued and transmitted, but the final `target_25_from_open` close command did not
make the opener leave the HCP `Open` state during the recorded video. The firmware
therefore incorrectly estimated movement that had not actually started. That is
the bug fixed by requiring HCP endpoint departure before starting the estimator
when moving away from a known `Open` or `Closed` endpoint.

## Captured Position Results

The percentages below are video-derived marker progress from the lower door
segment, normalized between closed and open endpoint observations in this same
video. They are best used for relative verification of the estimator, not as a
full 3D reconstruction of clear-opening height.

| Sequence step | Firmware reported | Video-derived progress |
| --- | ---: | ---: |
| target_25_from_closed | 25% | 33% |
| reset_closed_after_25 | Closed | 0% |
| target_50_from_closed | 50% | 60% |
| reset_closed_after_50 | Closed | 0% |
| target_75_from_closed | 75% | 50% |
| reset_closed_after_75 | Closed | 0% |
| setup_open_for_descending_targets | Open | 100% |
| target_75_from_open | 75% | 68% |
| reset_open_after_75 | Open | 100% |
| target_50_from_open | 50% | 42% |
| reset_open_after_50 | Open | 100% |
| target_25_from_open | 25% | not visible in video; door still appeared open at video end |

## Artifacts

- `summary.json`: timing alignment, endpoint windows, and event comparison.
- `auto02_position_timeline.png`: video-derived position timeline with command and HCP event markers.
- `auto02_event_contact_sheet.jpg`: representative frames at sequence milestones.
- `auto02_progress_5fps.csv`: 5 fps marker-progress samples.
- `auto02_event_comparison.csv`: HCP/firmware milestone positions compared against the video.
- `auto02_command_positions.csv`: command timestamps with video-derived positions.
- `qr_decode_stride6.jsonl`: QR timecode samples used for timing alignment.
- `aruco_raw_5fps.json`: raw ArUco detections from the extracted 5 fps analysis stream.
