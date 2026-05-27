# Garage Door Motion Curve Analysis - 2026-05-27

Input videos:

- `uncommitted-videos/opening-sample.mov`
- `uncommitted-videos/closing-sample.mov`

Assumptions:

- Door type: sectional door (`Sektionaltor`).
- Opening height from front: `2.10 m`.
- ArUco marker size: `100 mm`.
- Floor marker distance: approximately `1.07 m`.
- Marker dictionary: OpenCV `DICT_4X4_50`.

The analysis fits opening and closing separately because the drive uses different speeds in each direction.

## Results

- Opening movement duration: `10.215 s`.
- Closing movement duration: `18.565 s`.
- Opening starts at video time `3.033 s` and reaches the fitted open endpoint at `13.248 s`.
- Closing starts at video time `2.250 s` and reaches the fitted closed endpoint at `20.815 s`.

## Artifacts

- [curve_lookup.json](docs/research/analysis/garage-door-motion-20260527/curve_lookup.json): lookup table at 5% intervals for firmware calibration.
- [opening_curve.csv](docs/research/analysis/garage-door-motion-20260527/opening_curve.csv): per-frame opening curve.
- [closing_curve.csv](docs/research/analysis/garage-door-motion-20260527/closing_curve.csv): per-frame closing curve.
- [opening-curve.png](docs/research/analysis/garage-door-motion-20260527/opening-curve.png): opening plot.
- [closing-curve.png](docs/research/analysis/garage-door-motion-20260527/closing-curve.png): closing plot.
- [normalized-curves.png](docs/research/analysis/garage-door-motion-20260527/normalized-curves.png): opening and closing over normalized travel time.

## Sectional-Door Caveat

This is an empirical bottom-segment marker trajectory, normalized between closed and open endpoint marker positions. That means the curved top track of the sectional door is included in the observed marker path. The result is useful for timing compensation, especially soft start and soft stop, but it is not a calibrated 3D reconstruction of front-view clear-opening height at every frame.

For firmware use, treat this as a direction-specific timing curve. Validate the final percentage behavior with the interactive test wizard and measured opening height before exposing percentage control broadly.

## Selected Lookup Values

Opening, from closed:

| Position | Height | Time |
| ---: | ---: | ---: |
| 0% | 0.000 m | 0.000 s |
| 25% | 0.525 m | 3.000 s |
| 50% | 1.050 m | 5.413 s |
| 75% | 1.575 m | 7.670 s |
| 100% | 2.100 m | 10.215 s |

Closing, from open:

| Position | Height | Time |
| ---: | ---: | ---: |
| 100% | 2.100 m | 0.000 s |
| 75% | 1.575 m | 4.507 s |
| 50% | 1.050 m | 8.676 s |
| 25% | 0.525 m | 13.101 s |
| 0% | 0.000 m | 18.565 s |

Regenerate with:

```bash
uv run --with opencv-contrib-python-headless --with numpy --with matplotlib \
  tools/analyze_garage_aruco_video.py \
  --output-dir captures/aruco-video-analysis-20260527-4k60-fixed-endpoints
```
