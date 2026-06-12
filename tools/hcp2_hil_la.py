from __future__ import annotations

import argparse
import csv
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_CHANNELS = {
    "de": "D0",
    "re": "D1",
    "tx": "D2",
    "rx": "D3",
}
DEFAULT_MAX_DE_HIGH_US = 9000.0


@dataclass(frozen=True)
class Sample:
    time_s: float
    values: dict[str, int]


@dataclass(frozen=True)
class Window:
    start_s: float
    end_s: float

    @property
    def duration_us(self) -> float:
        return (self.end_s - self.start_s) * 1_000_000.0


def parse_channel_map(text: str | None) -> dict[str, str]:
    if not text:
        return dict(DEFAULT_CHANNELS)
    result: dict[str, str] = {}
    for item in text.split(","):
        if not item.strip():
            continue
        if "=" not in item:
            raise ValueError(f"channel mapping must be name=source, got {item!r}")
        name, source = item.split("=", 1)
        normalized_name = normalize_channel_name(name)
        if normalized_name not in {"de", "re", "tx", "rx"}:
            raise ValueError(f"unknown logical channel {name!r}")
        result[normalized_name] = source.strip()
    return result


def normalize_channel_name(name: str) -> str:
    normalized = name.strip().lower()
    normalized = normalized.replace("/", "")
    normalized = normalized.replace(" ", "_")
    normalized = normalized.replace("-", "_")
    if normalized in {"time", "time_s", "times", "time_sec", "timesec", "sample_time"}:
        return "time_s"
    return normalized


def parse_logic_value(value: object) -> int:
    if isinstance(value, bool):
        return 1 if value else 0
    if isinstance(value, (int, float)):
        return 1 if int(value) else 0
    text = str(value).strip().lower()
    if text in {"1", "true", "high", "h"}:
        return 1
    if text in {"0", "false", "low", "l"}:
        return 0
    raise ValueError(f"invalid logic value {value!r}")


def parse_time_s(value: object) -> float:
    text = str(value).strip()
    if text.lower().endswith("us"):
        return float(text[:-2]) / 1_000_000.0
    if text.lower().endswith("ms"):
        return float(text[:-2]) / 1000.0
    if text.lower().endswith("s"):
        return float(text[:-1])
    return float(text)


def load_samples_csv(path: Path, channels: dict[str, str]) -> list[Sample]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path} has no CSV header")
        normalized_fields = {normalize_channel_name(field): field for field in reader.fieldnames}
        time_field = normalized_fields.get("time_s")
        if time_field is None:
            raise ValueError(f"{path} must contain a time_s/time column")

        source_by_logic = {}
        for logical, source in channels.items():
            source_field = normalized_fields.get(normalize_channel_name(source))
            if source_field is not None:
                source_by_logic[logical] = source_field
                continue
            logic_field = normalized_fields.get(logical)
            if logic_field is not None:
                source_by_logic[logical] = logic_field

        samples: list[Sample] = []
        for row in reader:
            values: dict[str, int] = {}
            for logical, source_field in source_by_logic.items():
                raw_value = row.get(source_field, "")
                if raw_value != "":
                    values[logical] = parse_logic_value(raw_value)
            samples.append(Sample(parse_time_s(row[time_field]), values))
    return samples


def load_samples_json(path: Path, channels: dict[str, str]) -> list[Sample]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    raw_samples = payload.get("samples") if isinstance(payload, dict) else payload
    if not isinstance(raw_samples, list):
        raise ValueError(f"{path} JSON must be a list or contain a samples list")

    samples: list[Sample] = []
    for item in raw_samples:
        if not isinstance(item, dict):
            raise ValueError("JSON samples must be objects")
        time_value = item.get("time_s", item.get("time", item.get("t")))
        if time_value is None:
            raise ValueError("JSON sample missing time_s")
        values: dict[str, int] = {}
        for logical, source in channels.items():
            raw_value = item.get(source, item.get(logical))
            if raw_value is not None:
                values[logical] = parse_logic_value(raw_value)
        samples.append(Sample(parse_time_s(time_value), values))
    return samples


def load_samples(path: Path, channels: dict[str, str]) -> list[Sample]:
    suffix = path.suffix.lower()
    if suffix == ".json":
        samples = load_samples_json(path, channels)
    else:
        samples = load_samples_csv(path, channels)
    samples.sort(key=lambda sample: sample.time_s)
    if not samples:
        raise ValueError(f"{path} contains no samples")
    return samples


def require_channel(samples: list[Sample], channel: str) -> None:
    if not any(channel in sample.values for sample in samples):
        raise ValueError(f"capture does not contain channel {channel!r}")


def value_at(sample: Sample, channel: str) -> int | None:
    return sample.values.get(channel)


def high_windows(samples: list[Sample], channel: str) -> list[Window]:
    require_channel(samples, channel)
    windows: list[Window] = []
    previous = samples[0]
    level = value_at(previous, channel)
    start_s: float | None = previous.time_s if level == 1 else None

    for sample in samples[1:]:
        current = value_at(sample, channel)
        if current is None:
            previous = sample
            continue
        if level == 0 and current == 1:
            start_s = sample.time_s
        elif level == 1 and current == 0 and start_s is not None:
            windows.append(Window(start_s, sample.time_s))
            start_s = None
        level = current
        previous = sample

    if level == 1 and start_s is not None:
        windows.append(Window(start_s, samples[-1].time_s))
    return windows


def transition_times(samples: list[Sample], channel: str) -> list[float]:
    require_channel(samples, channel)
    transitions: list[float] = []
    last = value_at(samples[0], channel)
    for sample in samples[1:]:
        current = value_at(sample, channel)
        if current is None:
            continue
        if last is not None and current != last:
            transitions.append(sample.time_s)
        last = current
    return transitions


def contains_time(window: Window, time_s: float) -> bool:
    return window.start_s <= time_s <= window.end_s


def analyze_samples(
    samples: list[Sample],
    *,
    max_de_high_us: float = DEFAULT_MAX_DE_HIGH_US,
    require_initial_de_low: bool = True,
    require_initial_tx_high: bool = True,
    require_re_low: bool = True,
    require_tx_during_de: bool = True,
) -> dict[str, Any]:
    failures: list[str] = []
    duration_us = (samples[-1].time_s - samples[0].time_s) * 1_000_000.0
    de_windows = high_windows(samples, "de")
    tx_transitions = transition_times(samples, "tx") if any("tx" in sample.values for sample in samples) else []
    initial_de = value_at(samples[0], "de")
    initial_tx = value_at(samples[0], "tx")
    initial_re = value_at(samples[0], "re")

    if require_initial_de_low and initial_de != 0:
        failures.append("DE is not low at the first sample")
    if require_initial_tx_high and initial_tx is not None and initial_tx != 1:
        failures.append("TX is not idle-high at the first sample")
    if de_windows and value_at(samples[-1], "de") == 1:
        failures.append("DE is still high at the last sample")

    long_de_windows = [window for window in de_windows if window.duration_us > max_de_high_us]
    for window in long_de_windows:
        failures.append(f"DE high window {window.duration_us:.1f} us exceeds {max_de_high_us:.1f} us")

    tx_outside_de = [time_s for time_s in tx_transitions if not any(contains_time(window, time_s) for window in de_windows)]
    if tx_outside_de:
        failures.append(f"{len(tx_outside_de)} TX transitions occurred while DE was low")

    de_without_tx = []
    if require_tx_during_de and tx_transitions:
        for window in de_windows:
            if not any(contains_time(window, time_s) for time_s in tx_transitions):
                de_without_tx.append(window)
        if de_without_tx:
            failures.append(f"{len(de_without_tx)} DE high windows had no TX activity")

    re_high_samples = 0
    if require_re_low and any("re" in sample.values for sample in samples):
        re_high_samples = sum(1 for sample in samples if sample.values.get("re") == 1)
        if re_high_samples:
            failures.append(f"/RE was high in {re_high_samples} samples")

    max_de_high = max((window.duration_us for window in de_windows), default=0.0)
    report: dict[str, Any] = {
        "verdict": "fail" if failures else "ok",
        "failures": failures,
        "samples": len(samples),
        "duration_us": duration_us,
        "initial": {
            "de": initial_de,
            "tx": initial_tx,
            "re": initial_re,
        },
        "de_windows": len(de_windows),
        "max_de_high_us": max_de_high,
        "max_de_high_limit_us": max_de_high_us,
        "tx_transitions": len(tx_transitions),
        "tx_transitions_outside_de": len(tx_outside_de),
        "de_windows_without_tx_activity": len(de_without_tx),
        "re_high_samples": re_high_samples,
        "windows": [
            {
                "start_us": (window.start_s - samples[0].time_s) * 1_000_000.0,
                "end_us": (window.end_s - samples[0].time_s) * 1_000_000.0,
                "duration_us": window.duration_us,
            }
            for window in de_windows
        ],
    }
    return report


def build_sigrok_capture_command(args: argparse.Namespace) -> list[str]:
    channels = parse_channel_map(args.channels)
    command = [
        args.sigrok_cli,
        "--driver",
        args.driver,
        "--config",
        f"samplerate={args.samplerate}",
        "--channels",
        ",".join(channels.values()),
        "--time",
        args.duration,
        "--output-file",
        str(args.output),
    ]
    if args.output_format:
        command.extend(["--output-format", args.output_format])
    return command


def cmd_capture(args: argparse.Namespace) -> int:
    command = build_sigrok_capture_command(args)
    if args.dry_run:
        print(" ".join(command))
        return 0
    if shutil.which(args.sigrok_cli) is None:
        print(f"{args.sigrok_cli!r} not found; install sigrok-cli or use --dry-run", file=sys.stderr)
        return 1
    subprocess.run(command, check=True)
    return 0


def cmd_analyze(args: argparse.Namespace) -> int:
    channels = parse_channel_map(args.channels)
    try:
        samples = load_samples(args.input, channels)
        report = analyze_samples(
            samples,
            max_de_high_us=args.max_de_high_us,
            require_initial_de_low=not args.allow_initial_de_high,
            require_initial_tx_high=not args.allow_initial_tx_low,
            require_re_low=not args.allow_re_high,
            require_tx_during_de=not args.allow_de_without_tx,
        )
    except Exception as exc:
        print(f"garage-hcp2-hil-la analyze failed: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(
        "verdict={verdict} samples={samples} de_windows={de_windows} "
        "max_de_high_us={max_de_high_us:.1f} tx_outside_de={tx_transitions_outside_de}".format(**report)
    )
    for failure in report["failures"]:
        print(f"failure: {failure}", file=sys.stderr)
    return 0 if report["verdict"] == "ok" else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture and analyze HCP2 HIL logic-analyzer traces")
    subparsers = parser.add_subparsers(dest="command", required=True)

    capture = subparsers.add_parser("capture", help="Run sigrok-cli for an HCP2 bench capture")
    capture.add_argument("--sigrok-cli", default="sigrok-cli")
    capture.add_argument("--driver", default="fx2lafw")
    capture.add_argument("--samplerate", default="2m")
    capture.add_argument("--duration", default="10s")
    capture.add_argument("--channels", help="Comma-separated logical mapping, e.g. de=D0,re=D1,tx=D2,rx=D3")
    capture.add_argument("--output", type=Path, required=True)
    capture.add_argument("--output-format", default="srzip")
    capture.add_argument("--dry-run", action="store_true")
    capture.set_defaults(func=cmd_capture)

    analyze = subparsers.add_parser("analyze", help="Analyze a CSV/JSON logic trace")
    analyze.add_argument("--input", type=Path, required=True)
    analyze.add_argument("--channels", help="Comma-separated logical mapping, e.g. de=D0,re=D1,tx=D2,rx=D3")
    analyze.add_argument("--max-de-high-us", type=float, default=DEFAULT_MAX_DE_HIGH_US)
    analyze.add_argument("--allow-initial-de-high", action="store_true")
    analyze.add_argument("--allow-initial-tx-low", action="store_true")
    analyze.add_argument("--allow-re-high", action="store_true")
    analyze.add_argument("--allow-de-without-tx", action="store_true")
    analyze.add_argument("--output", type=Path)
    analyze.set_defaults(func=cmd_analyze)

    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
