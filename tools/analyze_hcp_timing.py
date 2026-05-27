#!/usr/bin/env python3
"""Align ESP persistent HCP timing with previously extracted video curves."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


TARGET_BITS = {
    "open": "open",
    "close": "closed",
}

TYPE_NAMES = {
    1: "rx",
    2: "tx",
    3: "gap",
    4: "frame",
    5: "command",
    6: "status",
    7: "control",
}

SOURCE_NAMES = {
    0: "raw",
    10: "broadcast",
    11: "broadcast-len1",
    12: "scan",
    13: "status-request",
    20: "scan-invalid",
    21: "broadcast-invalid",
    22: "broadcast-len1-invalid",
    23: "status-request-invalid",
    24: "unknown-valid",
    255: "other",
}

PHASE_NAMES = {
    0: "none",
    1: "start",
    2: "stop",
    3: "clear",
    10: "queued",
    11: "sent",
    12: "blocked",
    13: "expired",
    14: "dropped",
    15: "cancelled",
    16: "skipped",
    255: "other",
}

REASON_NAMES = {
    0: "none",
    1: "listen_only",
    2: "close_disabled",
    3: "impulse_disabled",
    4: "raw_stop_disabled",
    5: "stale_broadcast",
    6: "unsafe_state",
    7: "error_or_prewarn",
    8: "previous_command_pending",
    9: "state_already_matches",
    10: "status_request_timeout",
    11: "bus_state_stale",
    12: "stop_before_fetch",
    13: "venting_from_open_close_disabled",
    14: "obstruction_active",
    255: "other",
}

ACTION_NAMES = {
    0x10FF: "stop",
    0x1001: "open",
    0x1002: "close",
    0x1004: "impulse",
    0x1008: "toggle_light",
    0x1010: "venting",
    0x1000: "none",
    0x0000: "none",
}

STATUS_BITS = {
    "open": 0x0001,
    "closed": 0x0002,
    "relay": 0x0004,
    "light": 0x0008,
    "error": 0x0010,
    "direction_closing": 0x0020,
    "moving": 0x0040,
    "venting": 0x0080,
    "prewarn": 0x0100,
}


def record_time_s(record: dict[str, Any]) -> float:
    return float(record.get("first_ms", 0)) / 1000.0


def load_persistent_log(path: Path) -> dict[str, Any]:
    if path.suffix == ".bin":
        return load_binary_persistent_log(path)
    if path.stat().st_size == 0:
        bin_path = path.with_suffix(".bin")
        if bin_path.exists():
            return load_binary_persistent_log(bin_path)
    with path.open() as handle:
        return json.load(handle)


def load_json(path: Path) -> dict[str, Any]:
    with path.open() as handle:
        return json.load(handle)


def read_u16(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


def read_u32(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24)


def status_bits(status: int) -> dict[str, bool]:
    return {name: bool(status & mask) for name, mask in STATUS_BITS.items()}


def hex_word(value: int) -> str:
    return f"0x{value:04X}"


def load_binary_persistent_log(path: Path) -> dict[str, Any]:
    blob = path.read_bytes()
    records: list[dict[str, Any]] = []
    offset = 0
    dropped_decode_records = 0
    while offset < len(blob):
        total_len = blob[offset]
        if total_len < 35 or offset + total_len > len(blob):
            dropped_decode_records += 1
            break
        record = blob[offset : offset + total_len]
        payload = record[1:]
        data_len = payload[5]
        if data_len + 35 != total_len:
            dropped_decode_records += 1
            offset += max(total_len, 1)
            continue
        record_type = payload[0]
        flags = payload[1]
        source = payload[2]
        phase = payload[3]
        reason = payload[4]
        status = read_u16(payload, 26)
        action = read_u16(payload, 28)
        repeat = read_u16(payload, 32) or 1
        data = payload[34 : 34 + data_len]
        records.append(
            {
                "seq": read_u32(payload, 6),
                "type": TYPE_NAMES.get(record_type, "unknown"),
                "source": SOURCE_NAMES.get(source, "unknown"),
                "phase": PHASE_NAMES.get(phase, "unknown"),
                "reason": REASON_NAMES.get(reason, "unknown"),
                "flags": flags,
                "crc": "ok" if flags & 0x01 else "unknown_or_bad",
                "first_ms": read_u32(payload, 10),
                "last_ms": read_u32(payload, 14),
                "first_micros": read_u32(payload, 18),
                "last_micros": read_u32(payload, 22),
                "repeat": repeat,
                "status": status,
                "status_hex": hex_word(status),
                "action": ACTION_NAMES.get(action, "unknown"),
                "action_hex": hex_word(action),
                "state": read_u16(payload, 30),
                "bits": status_bits(status),
                "hex": data.hex().upper(),
            }
        )
        offset += total_len
    return {
        "storage": "binary_file",
        "format_version": None,
        "dropped_records": dropped_decode_records,
        "dropped_bytes": max(len(blob) - offset, 0),
        "records": records,
    }


def load_curve_csv(path: Path) -> list[dict[str, float]]:
    rows: list[dict[str, float]] = []
    if not path.exists():
        return rows
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            rows.append(
                {
                    "motion_time_s": float(row["motion_time_s"]),
                    "position_percent": float(row["position_monotonic"]) * 100.0,
                }
            )
    return rows


def command_records(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        record
        for record in records
        if record.get("type") == "command"
        and record.get("phase") == "sent"
        and record.get("action") in TARGET_BITS
    ]


def status_records(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        record
        for record in records
        if record.get("type") in {"frame", "status"}
        and record.get("source") in {"broadcast", "broadcast-len1"}
        and (record.get("crc") == "ok" or record.get("type") == "status")
    ]


def state_name(record: dict[str, Any]) -> str:
    bits = record.get("bits") or {}
    if bits.get("open"):
        return "open"
    if bits.get("closed"):
        return "closed"
    if bits.get("moving") and bits.get("direction_closing"):
        return "closing"
    if bits.get("moving"):
        return "opening"
    if bits.get("venting"):
        return "venting"
    return "unknown_or_stopped"


def transitions_between(statuses: list[dict[str, Any]], start_s: float, end_s: float) -> list[dict[str, Any]]:
    transitions: list[dict[str, Any]] = []
    last_status: str | None = None
    for record in statuses:
        t_s = record_time_s(record)
        if t_s < start_s or t_s > end_s:
            continue
        status_hex = record.get("status_hex")
        if status_hex == last_status:
            continue
        last_status = status_hex
        transitions.append(
            {
                "seq": record.get("seq"),
                "time_s": t_s,
                "relative_s": t_s - start_s,
                "status_hex": status_hex,
                "state": state_name(record),
                "source": record.get("source"),
                "hex": record.get("hex"),
            }
        )
    return transitions


def find_endpoint(
    statuses: list[dict[str, Any]],
    *,
    command_s: float,
    target_bit: str,
    ignore_old_end_state_s: float,
) -> dict[str, Any] | None:
    min_time = command_s + ignore_old_end_state_s
    for record in statuses:
        if record_time_s(record) < min_time:
            continue
        bits = record.get("bits") or {}
        if bits.get(target_bit):
            return record
    return None


def analyze_alignment(
    persistent_log: dict[str, Any],
    curve_lookup: dict[str, Any],
    *,
    ignore_old_end_state_s: float,
) -> dict[str, Any]:
    records = persistent_log.get("records") or []
    commands = command_records(records)
    statuses = status_records(records)
    first_record_s = record_time_s(records[0]) if records else 0.0

    cycles: list[dict[str, Any]] = []
    for command in commands:
        action = command["action"]
        direction = "opening" if action == "open" else "closing"
        command_s = record_time_s(command)
        target_bit = TARGET_BITS[action]
        endpoint = find_endpoint(
            statuses,
            command_s=command_s,
            target_bit=target_bit,
            ignore_old_end_state_s=ignore_old_end_state_s,
        )
        curve = (curve_lookup.get("curves") or {}).get(direction) or {}
        video_duration_s = curve.get("duration_s")
        cycle: dict[str, Any] = {
            "direction": direction,
            "action": action,
            "command_seq": command.get("seq"),
            "command_log_time_s": command_s,
            "command_relative_to_log_start_s": command_s - first_record_s,
            "command_hex": command.get("action_hex"),
            "video_duration_s": video_duration_s,
        }
        if endpoint is None:
            cycle["error"] = f"no HCP {target_bit} endpoint after command"
            cycles.append(cycle)
            continue

        endpoint_s = record_time_s(endpoint)
        hcp_duration_s = endpoint_s - command_s
        cycle.update(
            {
                "endpoint_seq": endpoint.get("seq"),
                "endpoint_log_time_s": endpoint_s,
                "endpoint_relative_to_command_s": hcp_duration_s,
                "endpoint_status_hex": endpoint.get("status_hex"),
                "endpoint_frame_hex": endpoint.get("hex"),
                "endpoint_state": state_name(endpoint),
                "transitions": transitions_between(statuses, command_s, endpoint_s + 2.0),
            }
        )
        if isinstance(video_duration_s, (int, float)):
            aligned_start_s = endpoint_s - float(video_duration_s)
            cycle.update(
                {
                    "aligned_video_motion_start_log_time_s": aligned_start_s,
                    "aligned_video_motion_start_relative_to_command_s": aligned_start_s - command_s,
                    "hcp_minus_video_duration_s": hcp_duration_s - float(video_duration_s),
                    "alignment_assumption": (
                        "HCP endpoint bit is treated as the full-stop endpoint; "
                        "without simultaneous video this combines motor start delay and endpoint report delay"
                    ),
                }
            )
        cycles.append(cycle)

    return {
        "persistent_log": {
            "format_version": persistent_log.get("format_version"),
            "record_count": len(records),
            "dropped_records": persistent_log.get("dropped_records"),
            "dropped_bytes": persistent_log.get("dropped_bytes"),
            "first_record_time_s": first_record_s,
        },
        "curve_assumptions": curve_lookup.get("assumptions"),
        "cycles": cycles,
    }


def write_summary(path: Path, report: dict[str, Any]) -> None:
    lines = [
        "# HCP To Video Timing Alignment",
        "",
        "The existing ArUco-derived opening and closing curves are used as the motion shape.",
        "Each HCP endpoint bit is treated as the fully stopped endpoint, so the residual is a combined command-to-motion-start and endpoint-report offset.",
        "",
        "| Direction | Command seq | Endpoint seq | HCP duration s | Video duration s | Combined offset s | Endpoint status |",
        "| --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for cycle in report["cycles"]:
        lines.append(
            "| {direction} | {command_seq} | {endpoint_seq} | {hcp_duration} | {video_duration} | {offset} | {status} |".format(
                direction=cycle.get("direction", ""),
                command_seq=cycle.get("command_seq", ""),
                endpoint_seq=cycle.get("endpoint_seq", ""),
                hcp_duration=format_float(cycle.get("endpoint_relative_to_command_s")),
                video_duration=format_float(cycle.get("video_duration_s")),
                offset=format_float(cycle.get("hcp_minus_video_duration_s")),
                status=cycle.get("endpoint_status_hex", cycle.get("error", "")),
            )
        )
    lines.append("")
    for cycle in report["cycles"]:
        lines.append(f"## {cycle.get('direction', 'unknown').title()}")
        lines.append("")
        if "error" in cycle:
            lines.append(f"- Error: `{cycle['error']}`")
            lines.append("")
            continue
        lines.append(f"- Command sent at log time: `{cycle['command_log_time_s']:.3f}s`")
        lines.append(f"- HCP endpoint after command: `{cycle['endpoint_relative_to_command_s']:.3f}s`")
        if "aligned_video_motion_start_relative_to_command_s" in cycle:
            lines.append(
                "- Estimated video-curve motion start after command: "
                f"`{cycle['aligned_video_motion_start_relative_to_command_s']:.3f}s`"
            )
        lines.append("- Status transitions:")
        for transition in cycle.get("transitions", []):
            lines.append(
                f"  - `+{transition['relative_s']:.3f}s` `{transition['status_hex']}` "
                f"{transition['state']} `{transition['hex']}`"
            )
        lines.append("")
    path.write_text("\n".join(lines) + "\n")


def format_float(value: Any) -> str:
    if isinstance(value, (int, float)):
        return f"{float(value):.3f}"
    return ""


def plot_alignment(output_dir: Path, report: dict[str, Any], curve_dir: Path) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as err:  # noqa: BLE001
        print(f"Skipping HCP alignment plot because matplotlib is unavailable: {err}")
        return

    cycles = [cycle for cycle in report["cycles"] if "error" not in cycle]
    if not cycles:
        return
    fig, axes = plt.subplots(len(cycles), 1, figsize=(11, 4.5 * len(cycles)), squeeze=False)
    for axis, cycle in zip(axes[:, 0], cycles, strict=False):
        direction = cycle["direction"]
        curve_rows = load_curve_csv(curve_dir / f"{direction}_curve.csv")
        start_offset = float(cycle.get("aligned_video_motion_start_relative_to_command_s", 0.0))
        if curve_rows:
            axis.plot(
                [row["motion_time_s"] + start_offset for row in curve_rows],
                [row["position_percent"] for row in curve_rows],
                linewidth=2,
                label=f"{direction} video curve aligned to HCP endpoint",
            )
        axis.axvline(0.0, color="black", linewidth=1, label="HCP command sent")
        axis.axvline(
            float(cycle["endpoint_relative_to_command_s"]),
            color="tab:red",
            linewidth=1.5,
            label="HCP endpoint bit",
        )
        for transition in cycle.get("transitions", []):
            axis.axvline(float(transition["relative_s"]), color="tab:gray", alpha=0.25, linewidth=0.8)
        axis.set_title(f"{direction.title()} HCP timing alignment")
        axis.set_xlabel("seconds since HCP command was sent")
        axis.set_ylabel("position [% open]")
        axis.set_ylim(-5, 105)
        axis.grid(True, alpha=0.3)
        axis.legend(loc="best")
    fig.tight_layout()
    fig.savefig(output_dir / "hcp-video-timing-alignment.png", dpi=160)
    plt.close(fig)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Align persistent HCP logs with existing garage-door video curves")
    parser.add_argument("--persistent-log", required=True, type=Path)
    parser.add_argument("--curve-lookup", default="docs/research/analysis/garage-door-motion-20260527/curve_lookup.json", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--ignore-old-end-state", type=float, default=1.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    persistent_log = load_persistent_log(args.persistent_log)
    curve_lookup = load_json(args.curve_lookup)
    output_dir = args.output_dir or args.persistent_log.parent
    output_dir.mkdir(parents=True, exist_ok=True)

    report = analyze_alignment(
        persistent_log,
        curve_lookup,
        ignore_old_end_state_s=args.ignore_old_end_state,
    )
    (output_dir / "hcp-video-timing-alignment.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    write_summary(output_dir / "hcp-video-timing-alignment.md", report)
    plot_alignment(output_dir, report, args.curve_lookup.parent)

    print(json.dumps(report, indent=2, sort_keys=True))
    print(f"Output written to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
