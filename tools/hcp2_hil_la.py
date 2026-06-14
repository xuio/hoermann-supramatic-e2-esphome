from __future__ import annotations

import argparse
from bisect import bisect_right
import csv
import gzip
from itertools import chain
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from tools.supramatic_sim import protocol


DEFAULT_CHANNELS = {
    "de": "D0",
    "re": "D1",
    "tx": "D2",
    "rx": "D3",
}
DEFAULT_MAX_DE_HIGH_US = 9000.0
DEFAULT_HCP2_BAUD = 57600
MIN_UART_SAMPLES_PER_BIT = 8.0


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


def parse_sample_rate_hz(text: str) -> float:
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*([kmg]?hz)\s*", text, re.IGNORECASE)
    if not match:
        raise ValueError(f"invalid sigrok samplerate {text!r}")
    value = float(match.group(1))
    unit = match.group(2).lower()
    multiplier = {
        "hz": 1.0,
        "khz": 1_000.0,
        "mhz": 1_000_000.0,
        "ghz": 1_000_000_000.0,
    }[unit]
    return value * multiplier


def sigrok_metadata(comments: list[str]) -> tuple[list[str], float] | None:
    raw_channels: list[str] | None = None
    sample_rate_hz: float | None = None
    for line in comments:
        text = line.strip()
        normalized = text.lower()
        if normalized.startswith("; channels"):
            _, payload = text.split(":", 1)
            raw_channels = [item.strip() for item in payload.split(",") if item.strip()]
        elif normalized.startswith("; samplerate"):
            _, payload = text.split(":", 1)
            sample_rate_hz = parse_sample_rate_hz(payload)
    if raw_channels is None and sample_rate_hz is None:
        return None
    if not raw_channels:
        raise ValueError("sigrok CSV is missing channel metadata")
    if sample_rate_hz is None or sample_rate_hz <= 0:
        raise ValueError("sigrok CSV is missing samplerate metadata")
    return raw_channels, sample_rate_hz


def load_sigrok_csv_samples(
    first_line: str,
    handle: Any,
    comments: list[str],
    channels: dict[str, str],
) -> list[Sample]:
    metadata = sigrok_metadata(comments)
    if metadata is None:
        raise ValueError("CSV has no time column and no sigrok samplerate metadata")
    raw_channels, sample_rate_hz = metadata
    channel_index = {normalize_channel_name(name): index for index, name in enumerate(raw_channels)}
    source_index_by_logic = {
        logical: channel_index[normalize_channel_name(source)]
        for logical, source in channels.items()
        if normalize_channel_name(source) in channel_index
    }
    if not source_index_by_logic:
        raise ValueError("sigrok CSV channel metadata does not match requested channel map")

    reader = csv.reader(chain([first_line], handle))
    samples: list[Sample] = []
    sample_index = 0
    for row in reader:
        if not row:
            continue
        if len(row) == 1 and row[0].strip().upper() == "FRAME-END":
            continue
        if all(cell.strip().lower() == "logic" for cell in row):
            continue
        values: dict[str, int] = {}
        for logical, source_index in source_index_by_logic.items():
            if source_index >= len(row):
                raise ValueError("sigrok CSV row has fewer columns than channel metadata")
            values[logical] = parse_logic_value(row[source_index])
        samples.append(Sample(sample_index / sample_rate_hz, values))
        sample_index += 1
    return samples


def open_samples_text(path: Path):
    if path.suffix.lower() == ".gz":
        return gzip.open(path, "rt", newline="", encoding="utf-8")
    return path.open(newline="", encoding="utf-8")


def load_samples_csv(path: Path, channels: dict[str, str]) -> list[Sample]:
    with open_samples_text(path) as handle:
        comments: list[str] = []
        first_line = ""
        for line in handle:
            if line.startswith(";"):
                comments.append(line.strip())
                continue
            if not line.strip():
                continue
            first_line = line
            break
        if not first_line:
            raise ValueError(f"{path} has no CSV header")

        reader = csv.DictReader(chain([first_line], handle))
        if reader.fieldnames is None:
            raise ValueError(f"{path} has no CSV header")
        normalized_fields = {normalize_channel_name(field): field for field in reader.fieldnames}
        time_field = normalized_fields.get("time_s")
        if time_field is None:
            return load_sigrok_csv_samples(first_line, handle, comments, channels)

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
    with open_samples_text(path) as handle:
        payload = json.load(handle)
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
    suffixes = [suffix.lower() for suffix in path.suffixes]
    suffix = suffixes[-2] if suffixes[-1:] == [".gz"] and len(suffixes) >= 2 else path.suffix.lower()
    if suffix == ".json":
        samples = load_samples_json(path, channels)
    else:
        samples = load_samples_csv(path, channels)
    samples.sort(key=lambda sample: sample.time_s)
    if not samples:
        raise ValueError(f"{path} contains no samples")
    return samples


def estimate_uniform_sample_rate_hz(samples: list[Sample], *, max_deltas: int = 4096) -> float | None:
    deltas: list[float] = []
    previous = samples[0].time_s
    for sample in samples[1:]:
        delta = sample.time_s - previous
        previous = sample.time_s
        if delta <= 0.0:
            continue
        deltas.append(delta)
        if len(deltas) >= max_deltas:
            break
    if not deltas:
        return None

    nominal = min(deltas)
    tolerance = max(nominal * 0.05, 1e-12)
    uniform = sum(1 for delta in deltas if abs(delta - nominal) <= tolerance)
    if uniform / len(deltas) < 0.80:
        return None
    return 1.0 / nominal


def uart_sampling_report(samples: list[Sample], baud: int) -> dict[str, Any]:
    sample_rate_hz = estimate_uniform_sample_rate_hz(samples)
    samples_per_bit = sample_rate_hz / float(baud) if sample_rate_hz is not None else None
    failures: list[str] = []
    if samples_per_bit is not None and samples_per_bit < MIN_UART_SAMPLES_PER_BIT:
        failures.append(
            f"uniform capture sample rate is too low for UART decode: {samples_per_bit:.2f} "
            f"samples/bit at {baud} baud, need at least {MIN_UART_SAMPLES_PER_BIT:.1f}"
        )
    return {
        "sample_rate_hz": sample_rate_hz,
        "samples_per_bit": samples_per_bit,
        "min_samples_per_bit": MIN_UART_SAMPLES_PER_BIT,
        "failures": failures,
    }


def apply_uart_sampling_check(report: dict[str, Any], samples: list[Sample], baud: int) -> None:
    sampling = uart_sampling_report(samples, baud)
    report["sampling"] = sampling
    if sampling["failures"]:
        report["failures"] = sampling["failures"] + list(report["failures"])
        report["verdict"] = "fail"


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


def high_sample_counts(samples: list[Sample], channel: str, allowed_windows: list[Window]) -> tuple[int, int]:
    starts = [window.start_s for window in allowed_windows]
    high_count = 0
    outside_count = 0
    for sample in samples:
        if sample.values.get(channel) != 1:
            continue
        high_count += 1
        index = bisect_right(starts, sample.time_s) - 1
        if index < 0 or sample.time_s > allowed_windows[index].end_s:
            outside_count += 1
    return high_count, outside_count


def crop_samples(samples: list[Sample], ignore_before_us: float) -> list[Sample]:
    if ignore_before_us < 0:
        raise ValueError("ignore_before_us must be non-negative")
    if not ignore_before_us:
        return samples
    start_time_s = samples[0].time_s + (ignore_before_us / 1_000_000.0)
    cropped = [sample for sample in samples if sample.time_s >= start_time_s]
    if not cropped:
        raise ValueError("ignore_before_us removed every sample")
    return cropped


def level_at_time(samples: list[Sample], times: list[float], channel: str, time_s: float) -> int | None:
    index = bisect_right(times, time_s) - 1
    while index >= 0:
        value = value_at(samples[index], channel)
        if value is not None:
            return value
        index -= 1
    return None


def transition_edges(samples: list[Sample], channel: str) -> list[tuple[float, int, int]]:
    require_channel(samples, channel)
    edges: list[tuple[float, int, int]] = []
    last = value_at(samples[0], channel)
    for sample in samples[1:]:
        current = value_at(sample, channel)
        if current is None:
            continue
        if last is not None and current != last:
            edges.append((sample.time_s, last, current))
        last = current
    return edges


def decode_uart_windows(
    samples: list[Sample],
    *,
    signal: str = "tx",
    gate: str = "de",
    baud: int = DEFAULT_HCP2_BAUD,
    ignore_before_us: float = 0.0,
) -> list[dict[str, Any]]:
    samples = crop_samples(samples, ignore_before_us)
    times = [sample.time_s for sample in samples]
    bit_s = 1.0 / float(baud)
    frame_bits = 1 + 8 + 1 + 1
    windows = high_windows(samples, gate)
    tx_edges = transition_edges(samples, signal)
    decoded_windows: list[dict[str, Any]] = []

    for window in windows:
        bytes_out: list[int] = []
        byte_reports: list[dict[str, Any]] = []
        cursor_s = window.start_s

        while True:
            start_edge_s: float | None = None
            for edge_s, before, after in tx_edges:
                if edge_s < cursor_s:
                    continue
                if edge_s > window.end_s:
                    break
                if before == 1 and after == 0:
                    start_edge_s = edge_s
                    break
            if start_edge_s is None:
                break

            errors: list[str] = []
            if level_at_time(samples, times, signal, start_edge_s + 0.5 * bit_s) != 0:
                errors.append("start")

            value = 0
            ones = 0
            for bit in range(8):
                bit_value = level_at_time(samples, times, signal, start_edge_s + (1.5 + bit) * bit_s)
                if bit_value not in {0, 1}:
                    errors.append(f"data{bit}")
                    bit_value = 0
                if bit_value:
                    value |= 1 << bit
                    ones += 1

            parity_value = level_at_time(samples, times, signal, start_edge_s + 9.5 * bit_s)
            if parity_value not in {0, 1}:
                errors.append("parity-missing")
                parity_value = 0
            elif (ones + parity_value) % 2 != 0:
                errors.append("parity")

            stop_value = level_at_time(samples, times, signal, start_edge_s + 10.5 * bit_s)
            if stop_value != 1:
                errors.append("stop")

            bytes_out.append(value)
            byte_reports.append(
                {
                    "start_us": (start_edge_s - samples[0].time_s) * 1_000_000.0,
                    "byte": value,
                    "errors": errors,
                }
            )
            cursor_s = start_edge_s + (frame_bits - 0.25) * bit_s

        frame = bytes(bytes_out)
        kind = protocol.decode_response_kind(frame) if frame else "none"
        first_byte_start_us = byte_reports[0]["start_us"] if byte_reports else None
        frame_end_us = None
        if byte_reports:
            frame_end_us = byte_reports[-1]["start_us"] + frame_bits * bit_s * 1_000_000.0
        decoded_windows.append(
            {
                "start_us": (window.start_s - samples[0].time_s) * 1_000_000.0,
                "end_us": (window.end_s - samples[0].time_s) * 1_000_000.0,
                "duration_us": window.duration_us,
                "first_byte_start_us": first_byte_start_us,
                "frame_end_us": frame_end_us,
                "byte_count": len(frame),
                "frame": frame.hex(),
                "crc_ok": protocol.crc_ok(frame) if frame else False,
                "kind": kind,
                "counter": protocol.response_counter(frame),
                "bytes": byte_reports,
                "errors": [error for byte in byte_reports for error in byte["errors"]],
            }
        )
    return decoded_windows


def master_expected_len(buffer: bytes) -> int | None:
    if len(buffer) < 2:
        return None
    function = buffer[1]
    if function == protocol.FC_WRITE_MULTIPLE_REGISTERS:
        if len(buffer) < 7:
            return None
        byte_count = buffer[6]
        expected = 7 + byte_count + 2
    elif function == protocol.FC_READ_WRITE_MULTIPLE_REGISTERS:
        if len(buffer) < 11:
            return None
        byte_count = buffer[10]
        expected = 11 + byte_count + 2
    else:
        return -1
    if expected < 5 or expected > 32:
        return -1
    return expected


def decode_master_kind(frame: bytes) -> str:
    if len(frame) < 2:
        return "unknown"
    if frame[0] == 0 and frame[1] == protocol.FC_WRITE_MULTIPLE_REGISTERS:
        return "broadcast"
    if frame[1] != protocol.FC_READ_WRITE_MULTIPLE_REGISTERS or len(frame) < 13:
        return "unknown"
    if frame[2:4] != bytes([0x9C, 0xB9]) or frame[6:8] != bytes([0x9C, 0x41]):
        return "unknown"
    read_qty = (frame[4] << 8) | frame[5]
    write_qty = (frame[8] << 8) | frame[9]
    byte_count = frame[10]
    if read_qty == 8 and write_qty == 2 and byte_count == 4 and len(frame) == 17 and frame[12] == 0x03:
        return "status_poll"
    if read_qty == 5 and write_qty == 3 and byte_count == 6 and len(frame) == 19:
        return "bus_scan"
    if read_qty == 2 and write_qty == 2 and byte_count == 4 and len(frame) == 17:
        return "command"
    return "unknown"


def master_counter(frame: bytes) -> int | None:
    kind = decode_master_kind(frame)
    if kind in {"status_poll", "command"} and len(frame) > 11:
        return frame[11]
    return None


def decode_uart_byte(
    samples: list[Sample],
    times: list[float],
    *,
    signal: str,
    start_edge_s: float,
    baud: int,
) -> tuple[int, dict[str, Any]]:
    bit_s = 1.0 / float(baud)
    errors: list[str] = []
    if level_at_time(samples, times, signal, start_edge_s + 0.5 * bit_s) != 0:
        errors.append("start")

    value = 0
    ones = 0
    for bit in range(8):
        bit_value = level_at_time(samples, times, signal, start_edge_s + (1.5 + bit) * bit_s)
        if bit_value not in {0, 1}:
            errors.append(f"data{bit}")
            bit_value = 0
        if bit_value:
            value |= 1 << bit
            ones += 1

    parity_value = level_at_time(samples, times, signal, start_edge_s + 9.5 * bit_s)
    if parity_value not in {0, 1}:
        errors.append("parity-missing")
        parity_value = 0
    elif (ones + parity_value) % 2 != 0:
        errors.append("parity")

    stop_value = level_at_time(samples, times, signal, start_edge_s + 10.5 * bit_s)
    if stop_value != 1:
        errors.append("stop")

    return value, {
        "start_us": (start_edge_s - samples[0].time_s) * 1_000_000.0,
        "byte": value,
        "errors": errors,
    }


def decode_uart_stream(
    samples: list[Sample],
    *,
    signal: str = "rx",
    baud: int = DEFAULT_HCP2_BAUD,
    ignore_before_us: float = 0.0,
) -> list[dict[str, Any]]:
    samples = crop_samples(samples, ignore_before_us)
    if not any(signal in sample.values for sample in samples):
        return []
    times = [sample.time_s for sample in samples]
    bit_s = 1.0 / float(baud)
    frame_bits = 1 + 8 + 1 + 1
    edges = transition_edges(samples, signal)
    buffer = bytearray()
    byte_reports: list[dict[str, Any]] = []
    decoded: list[dict[str, Any]] = []
    cursor_s = samples[0].time_s

    for edge_s, before, after in edges:
        if edge_s < cursor_s:
            continue
        if before != 1 or after != 0:
            continue
        value, byte_report = decode_uart_byte(samples, times, signal=signal, start_edge_s=edge_s, baud=baud)
        cursor_s = edge_s + (frame_bits - 0.25) * bit_s
        buffer.append(value)
        byte_reports.append(byte_report)
        while buffer:
            expected = master_expected_len(bytes(buffer))
            if expected is None:
                break
            if expected < 0:
                del buffer[0]
                del byte_reports[0]
                continue
            if len(buffer) < expected:
                break
            frame_bytes = bytes(buffer[:expected])
            frame_reports = byte_reports[:expected]
            decoded.append(
                {
                    "start_us": frame_reports[0]["start_us"],
                    "end_us": frame_reports[-1]["start_us"] + frame_bits * bit_s * 1_000_000.0,
                    "byte_count": len(frame_bytes),
                    "frame": frame_bytes.hex(),
                    "crc_ok": protocol.crc_ok(frame_bytes),
                    "kind": decode_master_kind(frame_bytes),
                    "counter": master_counter(frame_bytes),
                    "bytes": frame_reports,
                    "errors": [error for byte in frame_reports for error in byte["errors"]],
                }
            )
            del buffer[:expected]
            del byte_reports[:expected]
    return decoded


def status_counter_gaps(counters: list[int]) -> list[dict[str, Any]]:
    gaps: list[dict[str, Any]] = []
    for index, (previous, current) in enumerate(zip(counters, counters[1:]), start=1):
        expected = (previous + 1) & 0xFF
        if current == expected:
            continue
        missing: list[int] = []
        value = expected
        while value != current and len(missing) < 256:
            missing.append(value)
            value = (value + 1) & 0xFF
        gaps.append(
            {
                "index": index,
                "previous": previous,
                "current": current,
                "expected": expected,
                "missing": missing,
            }
        )
    return gaps


def percentile(sorted_values: list[float], pct: int) -> float | None:
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return sorted_values[0]
    rank = (pct / 100.0) * (len(sorted_values) - 1)
    low = int(rank)
    high = min(len(sorted_values) - 1, low + (0 if rank == low else 1))
    if low == high:
        return sorted_values[low]
    weight = rank - low
    return sorted_values[low] * (1.0 - weight) + sorted_values[high] * weight


def summarize_decoded_uart(
    windows: list[dict[str, Any]],
    *,
    ignore_before_us: float,
    baud: int,
    allow_status_gaps: bool = False,
    min_status_frames: int = 0,
) -> dict[str, Any]:
    decoded = [window for window in windows if window["byte_count"]]
    crc_ok = [window for window in decoded if window["crc_ok"]]
    status = [window for window in crc_ok if window["kind"] == "status"]
    counters = [window["counter"] for window in status if window["counter"] is not None]
    gaps = status_counter_gaps(counters)
    byte_error_windows = [window for window in decoded if window["errors"]]
    failures: list[str] = []
    if len(crc_ok) != len(decoded):
        failures.append(f"{len(decoded) - len(crc_ok)} decoded UART frame(s) failed Modbus CRC")
    if byte_error_windows:
        failures.append(f"{len(byte_error_windows)} decoded UART frame(s) had 8E1 parity/framing errors")
    if gaps and not allow_status_gaps:
        failures.append(f"{len(gaps)} status counter gap(s) detected")
    if len(status) < min_status_frames:
        failures.append(f"only {len(status)} status frame(s) decoded, expected at least {min_status_frames}")
    return {
        "verdict": "fail" if failures else "ok",
        "failures": failures,
        "ignored_before_us": ignore_before_us,
        "baud": baud,
        "windows": len(windows),
        "decoded_windows": len(decoded),
        "crc_ok_frames": len(crc_ok),
        "status_frames": len(status),
        "status_counters": counters,
        "status_counter_gaps": gaps,
        "status_counter_gap_count": len(gaps),
        "windows_detail": windows,
}


def metric_summary(values: list[float]) -> dict[str, float | int | None]:
    sorted_values = sorted(values)
    return {
        "count": len(values),
        "min_us": sorted_values[0] if sorted_values else None,
        "mean_us": (sum(sorted_values) / len(sorted_values)) if sorted_values else None,
        "p99_us": percentile(sorted_values, 99) if sorted_values else None,
        "max_us": sorted_values[-1] if sorted_values else None,
    }


def summarize_response_latencies(
    samples: list[Sample],
    *,
    baud: int = DEFAULT_HCP2_BAUD,
    ignore_before_us: float = 0.0,
    response_signal: str = "tx",
    response_gate: str = "de",
    request_signal: str = "rx",
) -> dict[str, Any]:
    cropped_samples = crop_samples(samples, ignore_before_us)
    if not any(request_signal in sample.values for sample in cropped_samples):
        return {
            "verdict": "not-run",
            "reason": f"capture does not contain request channel {request_signal!r}",
            "matched_status_pairs": 0,
        }

    request_frames = decode_uart_stream(
        samples,
        signal=request_signal,
        baud=baud,
        ignore_before_us=ignore_before_us,
    )
    response_windows = decode_uart_windows(
        samples,
        signal=response_signal,
        gate=response_gate,
        baud=baud,
        ignore_before_us=ignore_before_us,
    )
    status_polls = [
        frame
        for frame in request_frames
        if frame["crc_ok"] and not frame["errors"] and frame["kind"] == "status_poll" and frame["counter"] is not None
    ]
    status_responses = [
        frame
        for frame in response_windows
        if frame["crc_ok"] and not frame["errors"] and frame["kind"] == "status" and frame["counter"] is not None
    ]
    pending_by_counter: dict[int, list[dict[str, Any]]] = {}
    for poll in status_polls:
        pending_by_counter.setdefault(int(poll["counter"]), []).append(poll)

    pairs: list[dict[str, Any]] = []
    unmatched_responses = 0
    for response in status_responses:
        counter = int(response["counter"])
        candidates = pending_by_counter.get(counter, [])
        eligible_matches = [
            (index, poll)
            for index, poll in enumerate(candidates)
            if poll["end_us"] <= response["start_us"]
        ]
        match = max(
            eligible_matches,
            key=lambda item: item[1]["end_us"],
            default=None,
        )
        if match is None:
            unmatched_responses += 1
            continue
        match_index, _ = match
        poll = candidates.pop(match_index)
        first_byte_start_us = response.get("first_byte_start_us")
        frame_end_us = response.get("frame_end_us")
        if first_byte_start_us is None or frame_end_us is None:
            unmatched_responses += 1
            continue
        pairs.append(
            {
                "counter": counter,
                "poll_start_us": poll["start_us"],
                "poll_end_us": poll["end_us"],
                "de_assert_us": response["start_us"],
                "response_first_byte_us": first_byte_start_us,
                "response_end_us": frame_end_us,
                "de_release_us": response["end_us"],
                "poll_end_to_de_assert_us": response["start_us"] - poll["end_us"],
                "poll_end_to_first_byte_us": first_byte_start_us - poll["end_us"],
                "poll_end_to_response_end_us": frame_end_us - poll["end_us"],
                "de_assert_to_first_byte_us": first_byte_start_us - response["start_us"],
                "first_byte_to_response_end_us": frame_end_us - first_byte_start_us,
                "de_hold_us": response["duration_us"],
            }
        )

    unmatched_polls = sum(len(polls) for polls in pending_by_counter.values())
    failures: list[str] = []
    if status_polls and not pairs:
        failures.append("no status poll/response latency pairs could be matched")
    return {
        "verdict": "fail" if failures else "ok",
        "failures": failures,
        "ignored_before_us": ignore_before_us,
        "baud": baud,
        "request_signal": request_signal,
        "response_signal": response_signal,
        "response_gate": response_gate,
        "request_frames": len(request_frames),
        "status_polls": len(status_polls),
        "status_responses": len(status_responses),
        "matched_status_pairs": len(pairs),
        "unmatched_status_polls": unmatched_polls,
        "unmatched_status_responses": unmatched_responses,
        "poll_end_to_de_assert": metric_summary([pair["poll_end_to_de_assert_us"] for pair in pairs]),
        "poll_end_to_first_byte": metric_summary([pair["poll_end_to_first_byte_us"] for pair in pairs]),
        "poll_end_to_response_end": metric_summary([pair["poll_end_to_response_end_us"] for pair in pairs]),
        "de_assert_to_first_byte": metric_summary([pair["de_assert_to_first_byte_us"] for pair in pairs]),
        "first_byte_to_response_end": metric_summary([pair["first_byte_to_response_end_us"] for pair in pairs]),
        "de_hold": metric_summary([pair["de_hold_us"] for pair in pairs]),
        "pairs_detail": pairs[:200],
    }


def analyze_samples(
    samples: list[Sample],
    *,
    max_de_high_us: float = DEFAULT_MAX_DE_HIGH_US,
    ignore_before_us: float = 0.0,
    require_initial_de_low: bool = True,
    require_initial_tx_high: bool = True,
    require_re_low: bool = True,
    allow_re_high_during_de: bool = False,
    require_tx_only_during_de: bool = True,
    require_tx_during_de: bool = True,
) -> dict[str, Any]:
    failures: list[str] = []
    samples = crop_samples(samples, ignore_before_us)

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
    if require_tx_only_during_de and tx_outside_de:
        failures.append(f"{len(tx_outside_de)} TX transitions occurred while DE was low")

    de_without_tx = []
    initial_boundary_de_without_tx = []
    if require_tx_during_de and tx_transitions:
        for window in de_windows:
            if not any(contains_time(window, time_s) for time_s in tx_transitions):
                if not require_initial_de_low and abs(window.start_s - samples[0].time_s) <= 1e-12:
                    initial_boundary_de_without_tx.append(window)
                    continue
                de_without_tx.append(window)
        if de_without_tx:
            failures.append(f"{len(de_without_tx)} DE high windows had no TX activity")

    re_high_samples = 0
    re_high_outside_de_samples = 0
    if require_re_low and any("re" in sample.values for sample in samples):
        re_high_samples, re_high_outside_de_samples = high_sample_counts(samples, "re", de_windows)
        if allow_re_high_during_de and re_high_outside_de_samples:
            failures.append(f"/RE was high outside DE in {re_high_outside_de_samples} samples")
        elif not allow_re_high_during_de and re_high_samples:
            failures.append(f"/RE was high in {re_high_samples} samples")

    max_de_high = max((window.duration_us for window in de_windows), default=0.0)
    report: dict[str, Any] = {
        "verdict": "fail" if failures else "ok",
        "failures": failures,
        "samples": len(samples),
        "duration_us": duration_us,
        "ignored_before_us": ignore_before_us,
        "analysis_start_time_s": samples[0].time_s,
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
        "initial_boundary_de_windows_without_tx_activity": len(initial_boundary_de_without_tx),
        "re_high_samples": re_high_samples,
        "re_high_outside_de_samples": re_high_outside_de_samples,
        "allow_re_high_during_de": allow_re_high_during_de,
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


def verify_samples(
    samples: list[Sample],
    *,
    max_de_high_us: float = DEFAULT_MAX_DE_HIGH_US,
    ignore_before_us: float = 0.0,
    require_initial_de_low: bool = True,
    require_initial_tx_high: bool = True,
    require_re_low: bool = True,
    allow_re_high_during_de: bool = False,
    require_tx_only_during_de: bool = True,
    require_tx_during_de: bool = True,
    signal: str = "tx",
    gate: str = "de",
    baud: int = DEFAULT_HCP2_BAUD,
    allow_status_gaps: bool = False,
    min_status_frames: int = 0,
) -> dict[str, Any]:
    cropped_samples = crop_samples(samples, ignore_before_us)
    electrical = analyze_samples(
        samples,
        max_de_high_us=max_de_high_us,
        ignore_before_us=ignore_before_us,
        require_initial_de_low=require_initial_de_low,
        require_initial_tx_high=require_initial_tx_high,
        require_re_low=require_re_low,
        allow_re_high_during_de=allow_re_high_during_de,
        require_tx_only_during_de=require_tx_only_during_de,
        require_tx_during_de=require_tx_during_de,
    )
    uart = summarize_decoded_uart(
        decode_uart_windows(samples, signal=signal, gate=gate, baud=baud, ignore_before_us=ignore_before_us),
        ignore_before_us=ignore_before_us,
        baud=baud,
        allow_status_gaps=allow_status_gaps,
        min_status_frames=min_status_frames,
    )
    apply_uart_sampling_check(uart, cropped_samples, baud)
    latency = summarize_response_latencies(
        samples,
        baud=baud,
        ignore_before_us=ignore_before_us,
        response_signal=signal,
        response_gate=gate,
    )
    failures: list[str] = []
    failures.extend(f"electrical: {failure}" for failure in electrical["failures"])
    failures.extend(f"uart: {failure}" for failure in uart["failures"])
    if latency["verdict"] == "fail":
        failures.extend(f"latency: {failure}" for failure in latency["failures"])
    return {
        "verdict": "fail" if failures else "ok",
        "failures": failures,
        "electrical": electrical,
        "uart": uart,
        "latency": latency,
    }


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
            ignore_before_us=args.ignore_before_us,
            require_initial_de_low=not args.allow_initial_de_high,
            require_initial_tx_high=not args.allow_initial_tx_low,
            require_re_low=not args.allow_re_high,
            allow_re_high_during_de=args.allow_re_high_during_de,
            require_tx_only_during_de=not args.allow_tx_outside_de,
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


def cmd_decode_uart(args: argparse.Namespace) -> int:
    channels = parse_channel_map(args.channels)
    try:
        samples = load_samples(args.input, channels)
        cropped_samples = crop_samples(samples, args.ignore_before_us)
        windows = decode_uart_windows(
            samples,
            signal=args.signal,
            gate=args.gate,
            baud=args.baud,
            ignore_before_us=args.ignore_before_us,
        )
        report = summarize_decoded_uart(
            windows,
            ignore_before_us=args.ignore_before_us,
            baud=args.baud,
            allow_status_gaps=args.allow_status_gaps,
            min_status_frames=args.min_status_frames,
        )
        apply_uart_sampling_check(report, cropped_samples, args.baud)
    except Exception as exc:
        print(f"garage-hcp2-hil-la decode-uart failed: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(
        "verdict={verdict} windows={windows} decoded={decoded_windows} "
        "crc_ok={crc_ok_frames} status={status_frames} gaps={status_counter_gap_count}".format(**report)
    )
    for failure in report["failures"]:
        print(f"failure: {failure}", file=sys.stderr)
    return 0 if report["verdict"] == "ok" else 1


def cmd_verify(args: argparse.Namespace) -> int:
    channels = parse_channel_map(args.channels)
    try:
        samples = load_samples(args.input, channels)
        report = verify_samples(
            samples,
            max_de_high_us=args.max_de_high_us,
            ignore_before_us=args.ignore_before_us,
            require_initial_de_low=not args.allow_initial_de_high,
            require_initial_tx_high=not args.allow_initial_tx_low,
            require_re_low=not args.allow_re_high,
            allow_re_high_during_de=args.allow_re_high_during_de,
            require_tx_only_during_de=not args.allow_tx_outside_de,
            require_tx_during_de=not args.allow_de_without_tx,
            signal=args.signal,
            gate=args.gate,
            baud=args.baud,
            allow_status_gaps=args.allow_status_gaps,
            min_status_frames=args.min_status_frames,
        )
    except Exception as exc:
        print(f"garage-hcp2-hil-la verify failed: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(
        "verdict={verdict} electrical={electrical_verdict} uart={uart_verdict} "
        "status={status_frames} gaps={status_counter_gap_count} latency_pairs={latency_pairs}".format(
            verdict=report["verdict"],
            electrical_verdict=report["electrical"]["verdict"],
            uart_verdict=report["uart"]["verdict"],
            status_frames=report["uart"]["status_frames"],
            status_counter_gap_count=report["uart"]["status_counter_gap_count"],
            latency_pairs=report["latency"].get("matched_status_pairs", 0),
        )
    )
    for failure in report["failures"]:
        print(f"failure: {failure}", file=sys.stderr)
    return 0 if report["verdict"] == "ok" else 1


def cmd_latency(args: argparse.Namespace) -> int:
    channels = parse_channel_map(args.channels)
    try:
        samples = load_samples(args.input, channels)
        report = summarize_response_latencies(
            samples,
            baud=args.baud,
            ignore_before_us=args.ignore_before_us,
            response_signal=args.signal,
            response_gate=args.gate,
            request_signal=args.request_signal,
        )
    except Exception as exc:
        print(f"garage-hcp2-hil-la latency failed: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    first_byte = report.get("poll_end_to_first_byte", {})
    print(
        "verdict={verdict} pairs={pairs} poll_end_to_first_byte_max_us={max_us}".format(
            verdict=report["verdict"],
            pairs=report.get("matched_status_pairs", 0),
            max_us=first_byte.get("max_us") if isinstance(first_byte, dict) else None,
        )
    )
    for failure in report.get("failures", []):
        print(f"failure: {failure}", file=sys.stderr)
    return 0 if report["verdict"] in {"ok", "not-run"} else 1


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
    analyze.add_argument(
        "--ignore-before-us",
        type=float,
        default=0.0,
        help="Ignore samples before this offset from capture start; default keeps full reset-safety analysis",
    )
    analyze.add_argument("--allow-initial-de-high", action="store_true")
    analyze.add_argument("--allow-initial-tx-low", action="store_true")
    analyze.add_argument("--allow-re-high", action="store_true")
    analyze.add_argument("--allow-re-high-during-de", action="store_true")
    analyze.add_argument("--allow-tx-outside-de", action="store_true")
    analyze.add_argument("--allow-de-without-tx", action="store_true")
    analyze.add_argument("--output", type=Path)
    analyze.set_defaults(func=cmd_analyze)

    decode = subparsers.add_parser("decode-uart", help="Decode HCP2 UART bytes from TX edges gated by DE")
    decode.add_argument("--input", type=Path, required=True)
    decode.add_argument("--channels", help="Comma-separated logical mapping, e.g. de=D0,re=D1,tx=D2,rx=D3")
    decode.add_argument("--baud", type=int, default=DEFAULT_HCP2_BAUD)
    decode.add_argument("--signal", default="tx")
    decode.add_argument("--gate", default="de")
    decode.add_argument("--ignore-before-us", type=float, default=0.0)
    decode.add_argument("--allow-status-gaps", action="store_true")
    decode.add_argument("--min-status-frames", type=int, default=0)
    decode.add_argument("--output", type=Path)
    decode.set_defaults(func=cmd_decode_uart)

    verify = subparsers.add_parser("verify", help="Run electrical and UART continuity checks as one HIL verdict")
    verify.add_argument("--input", type=Path, required=True)
    verify.add_argument("--channels", help="Comma-separated logical mapping, e.g. de=D0,re=D1,tx=D2,rx=D3")
    verify.add_argument("--max-de-high-us", type=float, default=DEFAULT_MAX_DE_HIGH_US)
    verify.add_argument(
        "--ignore-before-us",
        type=float,
        default=0.0,
        help="Ignore samples before this offset from capture start; default keeps full reset-safety analysis",
    )
    verify.add_argument("--allow-initial-de-high", action="store_true")
    verify.add_argument("--allow-initial-tx-low", action="store_true")
    verify.add_argument("--allow-re-high", action="store_true")
    verify.add_argument("--allow-re-high-during-de", action="store_true")
    verify.add_argument("--allow-tx-outside-de", action="store_true")
    verify.add_argument("--allow-de-without-tx", action="store_true")
    verify.add_argument("--baud", type=int, default=DEFAULT_HCP2_BAUD)
    verify.add_argument("--signal", default="tx")
    verify.add_argument("--gate", default="de")
    verify.add_argument("--allow-status-gaps", action="store_true")
    verify.add_argument("--min-status-frames", type=int, default=0)
    verify.add_argument("--output", type=Path)
    verify.set_defaults(func=cmd_verify)

    latency = subparsers.add_parser("latency", help="Correlate RX status polls to TX status responses")
    latency.add_argument("--input", type=Path, required=True)
    latency.add_argument("--channels", help="Comma-separated logical mapping, e.g. de=D0,re=D1,tx=D2,rx=D3")
    latency.add_argument("--baud", type=int, default=DEFAULT_HCP2_BAUD)
    latency.add_argument("--signal", default="tx")
    latency.add_argument("--gate", default="de")
    latency.add_argument("--request-signal", default="rx")
    latency.add_argument("--ignore-before-us", type=float, default=0.0)
    latency.add_argument("--output", type=Path)
    latency.set_defaults(func=cmd_latency)

    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
