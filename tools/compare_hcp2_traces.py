from __future__ import annotations

import argparse
import json
import statistics
import sys
from collections import Counter
from pathlib import Path
from typing import Any


TRACE_PREFIX = "HCP2_TRACE "
FRAME_TYPES = ("master_frame", "slave_frame")
REQUIRED_TYPES = ("master_frame", "slave_frame", "reply_latency", "de")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare two HCP2 canonical traces")
    parser.add_argument("--left-trace", type=Path, help="Left trace source: JSON report, JSONL, or HCP2_TRACE log")
    parser.add_argument("--right-trace", type=Path, help="Right trace source: JSON report, JSONL, or HCP2_TRACE log")
    parser.add_argument("--left-label", default="left", help="Label for the left trace in reports")
    parser.add_argument("--right-label", default="right", help="Label for the right trace in reports")
    parser.add_argument("--wokwi-log", type=Path, help="Legacy alias for --left-trace with label 'wokwi'")
    parser.add_argument(
        "--iss-report",
        type=Path,
        help="Legacy alias for --right-trace with label 'iss'",
    )
    parser.add_argument("--latency-tolerance-us", type=float, default=2500.0)
    parser.add_argument("--output", type=Path, help="Write a JSON comparison report")
    args = parser.parse_args(argv)

    if args.left_trace is None and args.wokwi_log is not None:
        args.left_trace = args.wokwi_log
        if args.left_label == "left":
            args.left_label = "wokwi"
    if args.right_trace is None and args.iss_report is not None:
        args.right_trace = args.iss_report
        if args.right_label == "right":
            args.right_label = "iss"
    if args.left_trace is None or args.right_trace is None:
        parser.error("--left-trace and --right-trace are required")
    return args


def load_trace_prefixed_log(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if TRACE_PREFIX not in line:
            continue
        payload = line.split(TRACE_PREFIX, 1)[1]
        events.append(json.loads(payload))
    return events


def load_canonical_trace(path: Path) -> list[dict[str, Any]]:
    text = path.read_text(encoding="utf-8")
    if TRACE_PREFIX in text:
        return load_trace_prefixed_log(path)

    stripped = text.lstrip()
    if stripped.startswith("{"):
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            payload = None
        if payload is not None:
            trace = payload.get("lp_emu", {}).get("trace", payload.get("trace", payload.get("events")))
            if not isinstance(trace, list):
                raise ValueError(f"{path} does not contain lp_emu.trace, trace, or events")
            return trace

    events = []
    for line in text.splitlines():
        if line.strip():
            events.append(json.loads(line))
    return events


def count_by_type(events: list[dict[str, Any]]) -> Counter[str]:
    return Counter(str(event.get("type")) for event in events)


def raw_sequence(events: list[dict[str, Any]], event_type: str) -> list[str]:
    return [str(event.get("raw", "")) for event in events if event.get("type") == event_type]


def latencies(events: list[dict[str, Any]]) -> list[float]:
    values = []
    for event in events:
        if event.get("type") != "reply_latency":
            continue
        value = event.get("latency_us")
        if isinstance(value, (int, float)):
            values.append(float(value))
    return values


def latency_summary(values: list[float]) -> dict[str, float | None]:
    if not values:
        return {"min": None, "mean": None, "max": None}
    return {
        "min": min(values),
        "mean": statistics.fmean(values),
        "max": max(values),
    }


def compare(args: argparse.Namespace) -> tuple[bool, dict[str, Any]]:
    left = load_canonical_trace(args.left_trace)
    right = load_canonical_trace(args.right_trace)
    left_label = str(args.left_label)
    right_label = str(args.right_label)
    left_counts = count_by_type(left)
    right_counts = count_by_type(right)
    failures: list[str] = []

    if not left:
        failures.append(f"{left_label} trace is empty")
    if not right:
        failures.append(f"{right_label} trace is empty")

    for event_type in REQUIRED_TYPES:
        if left_counts[event_type] == 0:
            failures.append(f"{left_label} missing {event_type} events")
        if right_counts[event_type] == 0:
            failures.append(f"{right_label} missing {event_type} events")
        if left_counts[event_type] != right_counts[event_type]:
            failures.append(
                f"{event_type} count differs: {left_label}={left_counts[event_type]} "
                f"{right_label}={right_counts[event_type]}"
            )

    for event_type in FRAME_TYPES:
        left_raw = raw_sequence(left, event_type)
        right_raw = raw_sequence(right, event_type)
        if left_raw != right_raw:
            first_diff = next(
                (i for i, (left_item, right_item) in enumerate(zip(left_raw, right_raw)) if left_item != right_item),
                min(len(left_raw), len(right_raw)),
            )
            failures.append(f"{event_type} raw sequence differs at index {first_diff}")

    left_latency = latency_summary(latencies(left))
    right_latency = latency_summary(latencies(right))
    latency_delta: dict[str, float | None] = {}
    for key in ("min", "mean", "max"):
        left_value = left_latency[key]
        right_value = right_latency[key]
        if left_value is None or right_value is None:
            latency_delta[key] = None
            continue
        delta = abs(left_value - right_value)
        latency_delta[key] = delta
        if delta > args.latency_tolerance_us:
            failures.append(f"reply_latency {key} differs by {delta:.1f} us")

    report: dict[str, Any] = {
        "verdict": "fail" if failures else "ok",
        "failures": failures,
        "left_label": left_label,
        "right_label": right_label,
        "left_counts": dict(sorted(left_counts.items())),
        "right_counts": dict(sorted(right_counts.items())),
        "left_latency_us": left_latency,
        "right_latency_us": right_latency,
        "latency_delta_us": latency_delta,
        "latency_tolerance_us": args.latency_tolerance_us,
    }
    return not failures, report


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        ok, report = compare(args)
    except Exception as exc:
        print(f"garage-compare-hcp2-traces failed: {exc}", file=sys.stderr)
        return 1

    if args.output:
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "verdict={verdict} {left_label}_events={left_events} {right_label}_events={right_events}".format(
            verdict=report["verdict"],
            left_label=report["left_label"],
            right_label=report["right_label"],
            left_events=sum(report["left_counts"].values()),
            right_events=sum(report["right_counts"].values()),
        )
    )
    if report["failures"]:
        for failure in report["failures"]:
            print(f"failure: {failure}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
