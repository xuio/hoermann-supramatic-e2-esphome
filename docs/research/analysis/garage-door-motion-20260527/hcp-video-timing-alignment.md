# HCP To Video Timing Alignment

The existing ArUco-derived opening and closing curves are used as the motion shape.
Each HCP endpoint bit is treated as the fully stopped endpoint, so the residual is a combined command-to-motion-start and endpoint-report offset.

| Direction | Command seq | Endpoint seq | HCP duration s | Video duration s | Combined offset s | Endpoint status |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| opening | 239 | 1398 | 12.825 | 10.215 | 2.610 | 0x0001 |
| closing | 1951 | 3868 | 22.274 | 18.565 | 3.709 | 0x0002 |

## Opening

- Command sent at log time: `8819.542s`
- HCP endpoint after command: `12.825s`
- Estimated video-curve motion start after command: `2.610s`
- Status transitions:
  - `+0.098s` `0x0002` closed `000102CA`
  - `+0.234s` `0x0000` unknown_or_stopped ``
  - `+12.825s` `0x0001` open ``

## Closing

- Command sent at log time: `8838.697s`
- HCP endpoint after command: `22.274s`
- Estimated video-curve motion start after command: `3.709s`
- Status transitions:
  - `+0.102s` `0x0001` open `000101C3`
  - `+0.375s` `0x0000` unknown_or_stopped ``
  - `+22.274s` `0x0002` closed ``

