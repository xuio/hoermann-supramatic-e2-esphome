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
    parser = argparse.ArgumentParser(description="Compare Wokwi and ISS HCP2 canonical traces")
    parser.add_argument("--wokwi-log", type=Path, required=True, help="Wokwi console log containing HCP2_TRACE lines")
    parser.add_argument(
        "--iss-report",
        type=Path,
        required=True,
        help="ISS JSON report containing lp_emu.trace, or a JSONL trace file",
    )
    parser.add_argument("--latency-tolerance-us", type=float, default=2500.0)
    parser.add_argument("--output", type=Path, help="Write a JSON comparison report")
    return parser.parse_args(argv)


def load_wokwi_trace(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if TRACE_PREFIX not in line:
            continue
        payload = line.split(TRACE_PREFIX, 1)[1]
        events.append(json.loads(payload))
    return events


def load_iss_trace(path: Path) -> list[dict[str, Any]]:
    text = path.read_text(encoding="utf-8")
    stripped = text.lstrip()
    if stripped.startswith("{"):
        payload = json.loads(text)
        trace = payload.get("lp_emu", {}).get("trace", payload.get("trace"))
        if not isinstance(trace, list):
            raise ValueError(f"{path} does not contain lp_emu.trace")
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
    wokwi = load_wokwi_trace(args.wokwi_log)
    iss = load_iss_trace(args.iss_report)
    wokwi_counts = count_by_type(wokwi)
    iss_counts = count_by_type(iss)
    failures: list[str] = []

    if not wokwi:
        failures.append("wokwi trace is empty")
    if not iss:
        failures.append("iss trace is empty")

    for event_type in REQUIRED_TYPES:
        if wokwi_counts[event_type] == 0:
            failures.append(f"wokwi missing {event_type} events")
        if iss_counts[event_type] == 0:
            failures.append(f"iss missing {event_type} events")
        if wokwi_counts[event_type] != iss_counts[event_type]:
            failures.append(
                f"{event_type} count differs: wokwi={wokwi_counts[event_type]} iss={iss_counts[event_type]}"
            )

    for event_type in FRAME_TYPES:
        wokwi_raw = raw_sequence(wokwi, event_type)
        iss_raw = raw_sequence(iss, event_type)
        if wokwi_raw != iss_raw:
            first_diff = next(
                (i for i, (left, right) in enumerate(zip(wokwi_raw, iss_raw)) if left != right),
                min(len(wokwi_raw), len(iss_raw)),
            )
            failures.append(f"{event_type} raw sequence differs at index {first_diff}")

    wokwi_latency = latency_summary(latencies(wokwi))
    iss_latency = latency_summary(latencies(iss))
    latency_delta: dict[str, float | None] = {}
    for key in ("min", "mean", "max"):
        left = wokwi_latency[key]
        right = iss_latency[key]
        if left is None or right is None:
            latency_delta[key] = None
            continue
        delta = abs(left - right)
        latency_delta[key] = delta
        if delta > args.latency_tolerance_us:
            failures.append(f"reply_latency {key} differs by {delta:.1f} us")

    report: dict[str, Any] = {
        "verdict": "fail" if failures else "ok",
        "failures": failures,
        "wokwi_counts": dict(sorted(wokwi_counts.items())),
        "iss_counts": dict(sorted(iss_counts.items())),
        "wokwi_latency_us": wokwi_latency,
        "iss_latency_us": iss_latency,
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
        "verdict={verdict} wokwi_events={wokwi} iss_events={iss}".format(
            verdict=report["verdict"],
            wokwi=sum(report["wokwi_counts"].values()),
            iss=sum(report["iss_counts"].values()),
        )
    )
    if report["failures"]:
        for failure in report["failures"]:
            print(f"failure: {failure}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
