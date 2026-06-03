# Phone Sync Auto 01 Analysis

Source video: `uncommitted-videos/phone-sync-auto01.mov`

Source capture bundle: `uncommitted-capture-bundles/phone-sync-auto01`

This was the first automatic phone-sync run. The hardware behaved normally, but the old runner waited for decoded HCP state `Venting` after the vent command. The SupraMatic E2 instead reported the partly-open state on the operator display as `H` and the firmware/HCP state stayed `Stopped`, so the runner eventually timed out.

## Extracted Data

- `qr_decode_2fps.jsonl`: decoded MacBook QR timecode from the video, sampled at 2 FPS.
- `aruco_observations_2fps.csv`: detected ArUco marker centers, sampled at 2 FPS.
- `progress.csv`: stabilized moving-marker progress normalized between closed and open endpoints from this same video.
- `summary.json`: event window and endpoint summary.
- `auto01_position_estimate.png`: quick plot of estimated door position over the trimmed video.

The original video is not committed because it is large.

## Alignment

QR decoding succeeded for 162 of 181 downsampled frames. The median offset from video time to the runner clock was:

```text
run_elapsed_s = video_time_s + 89.9
```

Important aligned events:

| Event | Video time | Runner time | Note |
| --- | ---: | ---: | --- |
| Open command | 9.5 s | 99.4 s | Full-open step |
| Open reached | 23.0 s | 112.9 s | HCP reported `Open` at runner time 112.711 s |
| Close command | 33.0 s | 122.9 s | Full-close step |
| Closed reached | 56.5 s | 146.4 s | HCP reported `Closed` at runner time 145.558 s |
| Vent command | 66.0 s | 155.9 s | Partial-open command from closed |

## Motion Summary

The ArUco extraction detected both moving door markers in all 181 downsampled samples. Using the video-local closed and open endpoint positions:

| Movement | Visual start | Visual end | Approx duration |
| --- | ---: | ---: | ---: |
| Open | 12.0 s | 22.0 s | 10.0 s |
| Close | 34.5 s | 52.0 s | 17.5 s |
| Vent from closed | 68.5 s | 68.5 s | Below 2 FPS resolution |

The vent position stabilized at approximately `3.57%` open. With the measured `2.10 m` opening height, that is about `0.075 m` or `7.5 cm` of opening.

## Protocol Implication

After the vent command, the physical opener entered its normal partly-open state, but the decoded HCP state did not become `Venting`:

- HCP text state changed to `Stopped` at runner time `156.134 s`.
- Raw HCP status changed to `0x0000` at runner time `156.741 s`.
- No distinct decoded venting bit appeared in the current status mapping.

This supports the runner fix that treats venting as a timed observation window unless the firmware later learns a reliable E2-specific partial-open indication.
