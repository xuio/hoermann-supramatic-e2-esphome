# ArUco Garage Tracking Markers

These PDFs contain OpenCV ArUco `DICT_4X4_50` markers at exact print scale on A4 pages:

- [aruco_dict_4x4_50_100mm_static_reference_tags.pdf](docs/markers/aruco_dict_4x4_50_100mm_static_reference_tags.pdf): IDs `0` to `3` for fixed reference points around the garage opening.
- [aruco_dict_4x4_50_100mm_moving_door_tags.pdf](docs/markers/aruco_dict_4x4_50_100mm_moving_door_tags.pdf): ID `10` for the door bottom edge and ID `11` as a spare moving marker.
- [aruco_dict_4x4_50_100mm_all_garage_tracking_tags.pdf](docs/markers/aruco_dict_4x4_50_100mm_all_garage_tracking_tags.pdf): all six pages in one file.

Print at `100%` / `Actual size`. Disable `fit to page`, `shrink oversized pages`, and borderless scaling. After printing, verify that the printed scale bar measures exactly `100 mm`, and that the black marker square is exactly `100 mm`.

Recommended placement:

```text
ID 0 static                     ID 1 static

             garage opening

        ID 10 moving on bottom door edge

ID 2 static                     ID 3 static
```

Keep the phone or camera fixed, avoid wide-angle mode, and record full open and full close with all static markers and the moving marker visible. A 60 fps or 120 fps video is preferred.

Regenerate the PDFs with:

```bash
uv run garage-generate-aruco-markers
```
