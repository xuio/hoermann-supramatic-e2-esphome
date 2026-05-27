#!/usr/bin/env python3
"""Decode the fullscreen phone-sync QR timecode from video."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import cv2
import numpy as np

from tools.run_phone_video_sync_capture import (
    EVENT_NAMES,
    QR_QUIET_ZONE_MODULES,
    create_phone_sync_qr_encoder,
    parse_phone_sync_qr_payload,
    phone_sync_qr_payload,
)


@dataclass
class DecodeResult:
    frame: int
    time_s: float
    valid: bool
    seq: int | None
    event_code: int | None
    event_name: str | None
    elapsed_tenths: int | None
    payload: str
    reason: str
    points: list[list[float]]


def parse_decoded_payload(payload: str) -> tuple[dict[str, int] | None, str]:
    if not payload:
        return None, "qr_not_decoded"
    try:
        parsed = parse_phone_sync_qr_payload(payload)
    except ValueError as err:
        return None, str(err)
    return parsed, "ok"


def decode_frame(detector: cv2.QRCodeDetector, frame_image: np.ndarray, frame: int = 0, time_s: float = 0.0) -> DecodeResult:
    payload, points, _straight_qrcode = detector.detectAndDecode(frame_image)
    parsed, reason = parse_decoded_payload(payload)
    points_list: list[list[float]] = []
    if points is not None:
        points_list = np.asarray(points).reshape(-1, 2).astype(float).round(2).tolist()
    if parsed is None:
        return DecodeResult(
            frame=frame,
            time_s=time_s,
            valid=False,
            seq=None,
            event_code=None,
            event_name=None,
            elapsed_tenths=None,
            payload=payload,
            reason=reason,
            points=points_list,
        )
    event_code = parsed["event_code"]
    return DecodeResult(
        frame=frame,
        time_s=time_s,
        valid=True,
        seq=parsed["seq"],
        event_code=event_code,
        event_name=EVENT_NAMES.get(event_code, f"unknown_{event_code}"),
        elapsed_tenths=parsed["elapsed_tenths"],
        payload=payload,
        reason="ok",
        points=points_list,
    )


def render_synthetic_qr(seq: int, event_code: int, elapsed_tenths: int, modules_px: int = 40) -> np.ndarray:
    payload = phone_sync_qr_payload(seq, event_code, elapsed_tenths)
    matrix = create_phone_sync_qr_encoder().encode(payload)
    if QR_QUIET_ZONE_MODULES > 0:
        matrix = np.pad(matrix, QR_QUIET_ZONE_MODULES, mode="constant", constant_values=255)
    image = cv2.resize(matrix, (matrix.shape[1] * modules_px, matrix.shape[0] * modules_px), interpolation=cv2.INTER_NEAREST)
    return cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)


def run_self_test() -> None:
    detector = cv2.QRCodeDetector()
    image = render_synthetic_qr(seq=0x12AB34, event_code=7, elapsed_tenths=4321)
    result = decode_frame(detector, image)
    if not result.valid or result.seq != 0x12AB34 or result.event_code != 7 or result.elapsed_tenths != 4321:
        raise SystemExit(f"Straight QR self-test failed: {json.dumps(asdict(result), sort_keys=True)}")

    h, w = image.shape[:2]
    canvas = np.zeros((1200, 1800, 3), dtype=np.uint8)
    src = np.float32([[0, 0], [w - 1, 0], [w - 1, h - 1], [0, h - 1]])
    dst = np.float32([[260, 120], [1510, 60], [1580, 1020], [220, 920]])
    matrix = cv2.getPerspectiveTransform(src, dst)
    warped = cv2.warpPerspective(image, matrix, (canvas.shape[1], canvas.shape[0]))
    mask = cv2.warpPerspective(np.full(image.shape[:2], 255, dtype=np.uint8), matrix, (canvas.shape[1], canvas.shape[0]))
    canvas[mask > 0] = warped[mask > 0]
    result = decode_frame(detector, canvas)
    if not result.valid or result.seq != 0x12AB34 or result.event_code != 7 or result.elapsed_tenths != 4321:
        raise SystemExit(f"Perspective QR self-test failed: {json.dumps(asdict(result), sort_keys=True)}")
    print("QR timecode decoder self-test ok")


def decode_video(args: argparse.Namespace) -> int:
    cap = cv2.VideoCapture(str(args.video))
    if not cap.isOpened():
        raise SystemExit(f"Could not open video: {args.video}")
    fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0)
    output = args.output or args.video.with_suffix(".phone-sync-decode.jsonl")
    output.parent.mkdir(parents=True, exist_ok=True)
    detector = cv2.QRCodeDetector()

    frame_idx = 0
    checked = 0
    valid = 0
    with output.open("w", buffering=1) as handle:
        while True:
            ok, frame_image = cap.read()
            if not ok:
                break
            if args.max_frames is not None and frame_idx >= args.max_frames:
                break
            if frame_idx % args.sample_stride == 0:
                time_s = frame_idx / fps if fps > 0 else 0.0
                result = decode_frame(detector, frame_image, frame_idx, time_s)
                checked += 1
                if result.valid:
                    valid += 1
                if args.include_invalid or result.valid:
                    handle.write(json.dumps(asdict(result), sort_keys=True) + "\n")
            frame_idx += 1

    print(f"Checked {checked} frames from {args.video}")
    print(f"Valid QR timecode decodes: {valid}")
    print(f"Saved decode log: {output}")
    return 0 if valid > 0 else 2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Decode garage-phone-sync QR timecodes from a video")
    parser.add_argument("--video", type=Path, help="Phone video containing the MacBook fullscreen QR")
    parser.add_argument("--output", type=Path, help="JSONL output path, default is next to the video")
    parser.add_argument("--sample-stride", type=int, default=1, help="Decode every Nth frame")
    parser.add_argument("--max-frames", type=int, help="Stop after reading this many frames")
    parser.add_argument("--include-invalid", action="store_true", help="Write failed frame decodes as well as valid ones")
    parser.add_argument("--self-test", action="store_true", help="Run synthetic straight and perspective QR tests")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        run_self_test()
        return 0
    if args.video is None:
        raise SystemExit("--video is required unless --self-test is used")
    return decode_video(args)


if __name__ == "__main__":
    raise SystemExit(main())
