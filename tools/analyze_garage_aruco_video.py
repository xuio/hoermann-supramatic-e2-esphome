#!/usr/bin/env python3
"""Analyze garage-door ArUco marker videos and derive timing curves.

The script is intentionally empirical: it stabilizes marker observations against
fixed reference tags, normalizes moving door tags between closed/open endpoint
positions, and exports lookup tables that can later be used to compensate the
firmware's time-based position estimate.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import cv2
import numpy as np


STATIC_IDS = (0, 1, 2, 3)
MOVING_IDS = (10, 11)
DEFAULT_PERCENT_GRID = tuple(range(0, 101, 5))


@dataclass
class MarkerObservation:
    video: str
    frame: int
    time_s: float
    marker_id: int
    center_x: float
    center_y: float
    stable_x: float
    stable_y: float
    side_px: float


@dataclass
class VideoMetadata:
    name: str
    path: str
    fps: float
    frame_count: int
    width: int
    height: int
    duration_s: float


def get_dictionary(name: str):
    if not hasattr(cv2, "aruco"):
        raise SystemExit("OpenCV must include cv2.aruco. Use opencv-contrib-python-headless.")
    try:
        dictionary_id = getattr(cv2.aruco, name)
    except AttributeError as err:
        raise SystemExit(f"Unknown ArUco dictionary {name}") from err
    return cv2.aruco.getPredefinedDictionary(dictionary_id)


def make_detector(dictionary):
    params = cv2.aruco.DetectorParameters()
    if hasattr(cv2.aruco, "ArucoDetector"):
        return cv2.aruco.ArucoDetector(dictionary, params)
    return None, dictionary, params


def detect(detector, dictionary, params, gray):
    if detector is not None:
        return detector.detectMarkers(gray)
    return cv2.aruco.detectMarkers(gray, dictionary, parameters=params)


def marker_side_px(corners: np.ndarray) -> float:
    pts = corners.reshape(4, 2)
    lengths = [np.linalg.norm(pts[(i + 1) % 4] - pts[i]) for i in range(4)]
    return float(np.mean(lengths))


def marker_center(corners: np.ndarray) -> tuple[float, float]:
    pts = corners.reshape(4, 2)
    center = pts.mean(axis=0)
    return float(center[0]), float(center[1])


def collect_raw_observations(
    video_name: str,
    path: Path,
    dictionary_name: str,
    detection_scale: float,
    sample_stride: int,
) -> tuple[VideoMetadata, list[dict[str, Any]], dict[str, Any]]:
    dictionary = get_dictionary(dictionary_name)
    detector_obj = make_detector(dictionary)
    if isinstance(detector_obj, tuple):
        detector, dictionary, params = detector_obj
    else:
        detector, params = detector_obj, None

    cap = cv2.VideoCapture(str(path))
    if not cap.isOpened():
        raise SystemExit(f"Could not open video: {path}")

    fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0)
    frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 0)
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 0)
    duration_s = frame_count / fps if fps > 0 else 0.0
    metadata = VideoMetadata(video_name, str(path), fps, frame_count, width, height, duration_s)

    observations: list[dict[str, Any]] = []
    marker_counts: dict[int, int] = {}
    annotated_frames: list[tuple[int, np.ndarray, list[int], list[np.ndarray]]] = []
    frame_idx = 0

    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if frame_idx % sample_stride != 0:
            frame_idx += 1
            continue

        time_ms = float(cap.get(cv2.CAP_PROP_POS_MSEC) or 0.0)
        time_s = time_ms / 1000.0 if time_ms > 0 else (frame_idx / fps if fps > 0 else 0.0)
        work = frame
        scale = detection_scale
        if scale != 1.0:
            work = cv2.resize(frame, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
        gray = cv2.cvtColor(work, cv2.COLOR_BGR2GRAY)
        if detector is not None:
            corners, ids, _rejected = detect(detector, dictionary, params, gray)
        else:
            corners, ids, _rejected = detect(None, dictionary, params, gray)

        ids_list: list[int] = [] if ids is None else [int(value) for value in ids.flatten()]
        scaled_corners: list[np.ndarray] = []
        for marker_id, marker_corners in zip(ids_list, corners):
            marker_corners = marker_corners.astype(np.float64)
            if scale != 1.0:
                marker_corners /= scale
            cx, cy = marker_center(marker_corners)
            side_px = marker_side_px(marker_corners)
            observations.append(
                {
                    "video": video_name,
                    "frame": frame_idx,
                    "time_s": time_s,
                    "marker_id": marker_id,
                    "center_x": cx,
                    "center_y": cy,
                    "side_px": side_px,
                }
            )
            marker_counts[marker_id] = marker_counts.get(marker_id, 0) + 1
            scaled_corners.append(marker_corners.astype(np.float32))

        if ids_list and len(annotated_frames) < 6:
            annotated_frames.append((frame_idx, frame.copy(), ids_list, scaled_corners))
        frame_idx += 1

    cap.release()
    summary = {
        "marker_counts": {str(key): value for key, value in sorted(marker_counts.items())},
        "processed_frames": math.ceil(frame_count / sample_stride) if sample_stride > 0 else frame_count,
        "sample_stride": sample_stride,
        "detection_scale": detection_scale,
    }
    return metadata, observations, {"summary": summary, "annotated_frames": annotated_frames}


def stabilize_observations(raw: list[dict[str, Any]]) -> list[MarkerObservation]:
    static_samples: dict[int, list[tuple[float, float]]] = {marker_id: [] for marker_id in STATIC_IDS}
    grouped: dict[tuple[str, int], list[dict[str, Any]]] = {}
    for row in raw:
        grouped.setdefault((row["video"], row["frame"]), []).append(row)
        if row["marker_id"] in static_samples:
            static_samples[row["marker_id"]].append((row["center_x"], row["center_y"]))

    reference = {
        marker_id: np.array(
            [
                statistics.median(point[0] for point in points),
                statistics.median(point[1] for point in points),
            ],
            dtype=np.float64,
        )
        for marker_id, points in static_samples.items()
        if len(points) >= 5
    }

    stable: list[MarkerObservation] = []
    for (video, frame), rows in sorted(grouped.items()):
        src = []
        dst = []
        for row in rows:
            marker_id = row["marker_id"]
            if marker_id in reference:
                src.append([row["center_x"], row["center_y"]])
                dst.append(reference[marker_id].tolist())

        transform = None
        if len(src) >= 2:
            transform, _inliers = cv2.estimateAffinePartial2D(np.array(src), np.array(dst), method=cv2.LMEDS)

        for row in rows:
            point = np.array([row["center_x"], row["center_y"], 1.0], dtype=np.float64)
            if transform is not None:
                sx, sy = (transform @ point).tolist()
            else:
                sx, sy = row["center_x"], row["center_y"]
            stable.append(
                MarkerObservation(
                    video=video,
                    frame=row["frame"],
                    time_s=row["time_s"],
                    marker_id=row["marker_id"],
                    center_x=row["center_x"],
                    center_y=row["center_y"],
                    stable_x=float(sx),
                    stable_y=float(sy),
                    side_px=row["side_px"],
                )
            )
    return stable


def endpoint_center(
    observations: list[MarkerObservation],
    *,
    videos: tuple[str, ...],
    marker_id: int,
    window: str,
    seconds: float,
) -> np.ndarray | None:
    points: list[tuple[float, float]] = []
    for video in videos:
        rows = [row for row in observations if row.video == video and row.marker_id == marker_id]
        if not rows:
            continue
        rows.sort(key=lambda row: row.time_s)
        if window == "start":
            limit = rows[0].time_s + seconds
            selected = [row for row in rows if row.time_s <= limit]
        else:
            limit = rows[-1].time_s - seconds
            selected = [row for row in rows if row.time_s >= limit]
        for row in selected:
            points.append((row.stable_x, row.stable_y))
    if len(points) < 3:
        return None
    return np.array(
        [
            statistics.median(point[0] for point in points),
            statistics.median(point[1] for point in points),
        ],
        dtype=np.float64,
    )


def compute_progress(
    observations: list[MarkerObservation],
    endpoint_seconds: float,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    endpoints: dict[int, dict[str, Any]] = {}
    for marker_id in MOVING_IDS:
        closed = endpoint_center(
            observations,
            videos=("opening",),
            marker_id=marker_id,
            window="start",
            seconds=endpoint_seconds,
        )
        closed_alt = endpoint_center(
            observations,
            videos=("closing",),
            marker_id=marker_id,
            window="end",
            seconds=endpoint_seconds,
        )
        open_ = endpoint_center(
            observations,
            videos=("closing",),
            marker_id=marker_id,
            window="start",
            seconds=endpoint_seconds,
        )
        open_alt = endpoint_center(
            observations,
            videos=("opening",),
            marker_id=marker_id,
            window="end",
            seconds=endpoint_seconds,
        )
        closed_points = [point for point in (closed, closed_alt) if point is not None]
        open_points = [point for point in (open_, open_alt) if point is not None]
        if not closed_points or not open_points:
            continue
        closed_center = np.mean(np.stack(closed_points), axis=0)
        open_center = np.mean(np.stack(open_points), axis=0)
        vector = open_center - closed_center
        denom = float(vector @ vector)
        if denom < 1.0:
            continue
        endpoints[marker_id] = {
            "closed": closed_center,
            "open": open_center,
            "vector": vector,
            "denom": denom,
        }

    per_frame: dict[tuple[str, int], dict[str, Any]] = {}
    for row in observations:
        if row.marker_id not in endpoints:
            continue
        ep = endpoints[row.marker_id]
        point = np.array([row.stable_x, row.stable_y], dtype=np.float64)
        progress = float(((point - ep["closed"]) @ ep["vector"]) / ep["denom"])
        item = per_frame.setdefault(
            (row.video, row.frame),
            {
                "video": row.video,
                "frame": row.frame,
                "time_s": row.time_s,
                "marker_progress": {},
                "marker_centers": {},
            },
        )
        item["marker_progress"][str(row.marker_id)] = progress
        item["marker_centers"][str(row.marker_id)] = [row.stable_x, row.stable_y]

    rows = []
    for item in sorted(per_frame.values(), key=lambda value: (value["video"], value["frame"])):
        values = list(item["marker_progress"].values())
        if not values:
            continue
        item["progress_raw"] = float(statistics.median(values))
        item["visible_moving_markers"] = len(values)
        rows.append(item)

    endpoint_summary = {
        str(marker_id): {
            "closed": endpoints[marker_id]["closed"].tolist(),
            "open": endpoints[marker_id]["open"].tolist(),
        }
        for marker_id in endpoints
    }
    return rows, endpoint_summary


def interpolate_and_smooth(times: np.ndarray, values: np.ndarray, window_s: float) -> np.ndarray:
    order = np.argsort(times)
    times = times[order]
    values = values[order]
    good = np.isfinite(values)
    if good.sum() < 3:
        return values
    interpolated = np.interp(times, times[good], values[good])
    if len(times) < 5:
        return interpolated
    dt = float(np.median(np.diff(times))) if len(times) > 2 else 1.0 / 30.0
    window = max(3, int(round(window_s / max(dt, 1e-3))))
    if window % 2 == 0:
        window += 1
    half = window // 2
    smoothed = np.empty_like(interpolated)
    for idx in range(len(interpolated)):
        start = max(0, idx - half)
        end = min(len(interpolated), idx + half + 1)
        smoothed[idx] = float(np.median(interpolated[start:end]))
    return smoothed


def detect_motion_window(times: np.ndarray, progress: np.ndarray, direction: str) -> tuple[int, int]:
    if direction == "opening":
        start_candidates = np.flatnonzero(progress > 0.02)
        end_candidates = np.flatnonzero(progress > 0.98)
    else:
        start_candidates = np.flatnonzero(progress < 0.98)
        end_candidates = np.flatnonzero(progress < 0.02)
    if len(start_candidates) == 0:
        start_candidates = np.array([0])
    start_idx = int(start_candidates[0])
    end_candidates = end_candidates[end_candidates > start_idx]
    if len(end_candidates) == 0:
        end_idx = len(times) - 1
    else:
        end_idx = int(end_candidates[0])
    if end_idx <= start_idx:
        end_idx = len(times) - 1
    return start_idx, end_idx


def analyze_curve(
    rows: list[dict[str, Any]],
    *,
    video: str,
    direction: str,
    opening_height_m: float,
    percent_grid: tuple[int, ...],
) -> dict[str, Any]:
    selected = [row for row in rows if row["video"] == video]
    selected.sort(key=lambda row: row["time_s"])
    times = np.array([row["time_s"] for row in selected], dtype=np.float64)
    raw = np.array([row["progress_raw"] for row in selected], dtype=np.float64)
    smoothed = interpolate_and_smooth(times, raw, window_s=0.20)
    smoothed = np.clip(smoothed, -0.05, 1.05)
    if direction == "opening":
        monotonic = np.maximum.accumulate(smoothed)
    else:
        monotonic = np.minimum.accumulate(smoothed)
    monotonic = np.clip(monotonic, 0.0, 1.0)

    start_idx, end_idx = detect_motion_window(times, monotonic, direction)
    t_start = float(times[start_idx])
    t_end = float(times[end_idx])
    duration = max(0.0, t_end - t_start)

    curve_rows: list[dict[str, Any]] = []
    for row, value, smooth, mono in zip(selected, raw, smoothed, monotonic):
        motion_t = float(row["time_s"] - t_start)
        curve_rows.append(
            {
                "video": video,
                "frame": row["frame"],
                "time_s": row["time_s"],
                "motion_time_s": motion_t,
                "position_raw": float(value),
                "position_smooth": float(smooth),
                "position_monotonic": float(mono),
                "height_m": float(mono * opening_height_m),
                "visible_moving_markers": row["visible_moving_markers"],
                "marker_progress": row["marker_progress"],
            }
        )

    active_mask = (times >= t_start) & (times <= t_end)
    active_t = times[active_mask] - t_start
    active_p = monotonic[active_mask]
    if direction == "opening":
        xp = active_p
        fp = active_t
        table = [
            {
                "position_percent": percent,
                "height_m": percent / 100.0 * opening_height_m,
                "time_from_closed_s": float(np.interp(percent / 100.0, xp, fp)),
            }
            for percent in percent_grid
        ]
    else:
        close_progress = 1.0 - active_p
        table = [
            {
                "position_percent": percent,
                "height_m": percent / 100.0 * opening_height_m,
                "time_from_open_s": float(np.interp(1.0 - percent / 100.0, close_progress, active_t)),
            }
            for percent in reversed(percent_grid)
        ]

    return {
        "video": video,
        "direction": direction,
        "t_start_s": t_start,
        "t_end_s": t_end,
        "duration_s": duration,
        "rows": curve_rows,
        "lookup": table,
    }


def write_csv(path: Path, rows: list[dict[str, Any]], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def save_annotated_frames(output_dir: Path, video_name: str, annotated_frames: list[tuple[int, np.ndarray, list[int], list[np.ndarray]]]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for frame_idx, frame, ids, corners in annotated_frames[:4]:
        ids_array = np.array(ids, dtype=np.int32).reshape(-1, 1)
        cv2.aruco.drawDetectedMarkers(frame, corners, ids_array)
        path = output_dir / f"{video_name}-frame-{frame_idx:05d}.jpg"
        cv2.imwrite(str(path), frame)


def plot_curves(output_dir: Path, curves: list[dict[str, Any]]) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as err:  # noqa: BLE001
        print(f"Skipping plots because matplotlib is unavailable: {err}")
        return

    for curve in curves:
        rows = curve["rows"]
        t = [row["motion_time_s"] for row in rows]
        raw = [row["position_raw"] * 100.0 for row in rows]
        smooth = [row["position_monotonic"] * 100.0 for row in rows]
        plt.figure(figsize=(10, 6))
        plt.plot(t, raw, ".", markersize=1, alpha=0.25, label="raw")
        plt.plot(t, smooth, "-", linewidth=2, label="monotonic smoothed")
        plt.axvline(0, color="black", linewidth=0.8)
        plt.axvline(curve["duration_s"], color="black", linewidth=0.8)
        plt.xlabel("motion time [s]")
        plt.ylabel("position [% open]")
        plt.ylim(-5, 105)
        plt.title(f"{curve['direction'].title()} curve ({curve['duration_s']:.2f}s)")
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        plt.savefig(output_dir / f"{curve['direction']}-curve.png", dpi=160)
        plt.close()

    plt.figure(figsize=(10, 6))
    for curve in curves:
        rows = [row for row in curve["rows"] if 0 <= row["motion_time_s"] <= curve["duration_s"]]
        tnorm = [row["motion_time_s"] / curve["duration_s"] if curve["duration_s"] else 0 for row in rows]
        pos = [row["position_monotonic"] * 100.0 for row in rows]
        plt.plot(tnorm, pos, linewidth=2, label=curve["direction"])
    plt.xlabel("normalized travel time")
    plt.ylabel("position [% open]")
    plt.ylim(-5, 105)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_dir / "normalized-curves.png", dpi=160)
    plt.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyze ArUco-tagged garage-door videos")
    parser.add_argument("--closing-video", default=str(Path.home() / "Downloads/garage-door-closing.mov"))
    parser.add_argument("--opening-video", default=str(Path.home() / "Downloads/garage-door-opening.mov"))
    parser.add_argument("--output-dir", default="captures/aruco-video-analysis")
    parser.add_argument("--dictionary", default="DICT_4X4_50")
    parser.add_argument("--marker-size-m", type=float, default=0.10)
    parser.add_argument("--floor-marker-distance-m", type=float, default=1.07)
    parser.add_argument("--opening-height-m", type=float, default=2.10)
    parser.add_argument("--detection-scale", type=float, default=0.5)
    parser.add_argument("--sample-stride", type=int, default=1)
    parser.add_argument("--endpoint-seconds", type=float, default=1.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    raw_all: list[dict[str, Any]] = []
    metadata: dict[str, Any] = {}
    detection: dict[str, Any] = {}
    for video_name, video_path in {
        "closing": Path(args.closing_video),
        "opening": Path(args.opening_video),
    }.items():
        meta, rows, info = collect_raw_observations(
            video_name,
            video_path,
            args.dictionary,
            args.detection_scale,
            args.sample_stride,
        )
        raw_all.extend(rows)
        metadata[video_name] = meta.__dict__
        detection[video_name] = info["summary"]
        save_annotated_frames(output_dir / "annotated_frames", video_name, info["annotated_frames"])

    stable = stabilize_observations(raw_all)
    progress_rows, endpoints = compute_progress(stable, args.endpoint_seconds)

    observations_rows = [
        {
            "video": row.video,
            "frame": row.frame,
            "time_s": row.time_s,
            "marker_id": row.marker_id,
            "center_x": row.center_x,
            "center_y": row.center_y,
            "stable_x": row.stable_x,
            "stable_y": row.stable_y,
            "side_px": row.side_px,
        }
        for row in stable
    ]
    write_csv(
        output_dir / "marker_observations.csv",
        observations_rows,
        ["video", "frame", "time_s", "marker_id", "center_x", "center_y", "stable_x", "stable_y", "side_px"],
    )

    curves = [
        analyze_curve(progress_rows, video="opening", direction="opening", opening_height_m=args.opening_height_m, percent_grid=DEFAULT_PERCENT_GRID),
        analyze_curve(progress_rows, video="closing", direction="closing", opening_height_m=args.opening_height_m, percent_grid=DEFAULT_PERCENT_GRID),
    ]

    for curve in curves:
        write_csv(
            output_dir / f"{curve['direction']}_curve.csv",
            curve["rows"],
            [
                "video",
                "frame",
                "time_s",
                "motion_time_s",
                "position_raw",
                "position_smooth",
                "position_monotonic",
                "height_m",
                "visible_moving_markers",
            ],
        )
    lookup = {
        "assumptions": {
            "marker_size_m": args.marker_size_m,
            "floor_marker_distance_m": args.floor_marker_distance_m,
            "opening_height_m": args.opening_height_m,
            "position_model": (
                "sectional-door empirical bottom-segment marker trajectory normalized between "
                "closed/open endpoint marker positions; this captures the visible curved-track path "
                "but is not a calibrated 3D reconstruction of clear-opening height"
            ),
            "direction_model": "opening and closing are fitted independently because the opener uses different speeds",
        },
        "metadata": metadata,
        "detection": detection,
        "endpoints": endpoints,
        "curves": {
            curve["direction"]: {
                "duration_s": curve["duration_s"],
                "t_start_s": curve["t_start_s"],
                "t_end_s": curve["t_end_s"],
                "lookup": curve["lookup"],
            }
            for curve in curves
        },
    }
    (output_dir / "curve_lookup.json").write_text(json.dumps(lookup, indent=2, sort_keys=True) + "\n")
    plot_curves(output_dir, curves)

    print(json.dumps(lookup, indent=2, sort_keys=True))
    print(f"Output written to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
