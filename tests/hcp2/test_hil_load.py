from __future__ import annotations

from pathlib import Path

import pytest

from tools.hcp2_hil_load import (
    aggregate_reports,
    build_parser,
    effective_cycles,
    effective_duration_s,
    effective_load_commands,
    indexed_trace_path,
)


def parse_args(*extra: str):
    return build_parser().parse_args(["--serial", "/dev/null", *extra])


def test_hil_load_hostile_preset_requires_esp_host() -> None:
    args = parse_args("--preset", "hostile")
    with pytest.raises(ValueError, match="--esp-host"):
        effective_load_commands(args)


def test_hil_load_hostile_preset_adds_ping_and_api_reconnect() -> None:
    args = parse_args("--preset", "hostile", "--esp-host", "192.0.2.10", "--load-command", "stress-test")
    commands = effective_load_commands(args)

    assert commands[0] == "stress-test"
    assert any(command.startswith("ping -i 0.2 192.0.2.10") for command in commands)
    assert any("socket.AF_INET" in command and "6053" in command for command in commands)


def test_hil_load_repeat_trace_paths_are_indexed() -> None:
    trace = Path("captures/hcp2/hil.jsonl")

    assert indexed_trace_path(trace, 1, 1) == trace
    assert indexed_trace_path(trace, 2, 3) == Path("captures/hcp2/hil.run02.jsonl")
    assert indexed_trace_path(Path("captures/hcp2/progress.jsonl"), 3, 4) == Path(
        "captures/hcp2/progress.run03.jsonl"
    )


def test_hil_load_duration_resolves_to_protocol_cycles() -> None:
    args = parse_args("--duration-hours", "0.001", "--speed-factor", "1")

    assert effective_cycles(args) == 52
    assert effective_duration_s(args) == 3.6


def test_hil_load_aggregate_report_summarizes_worst_case() -> None:
    args = parse_args("--repeat", "2", "--cycles", "10", "--preset", "none")
    report = aggregate_reports(
        args,
        [
            {
                "verdict": "ok",
                "simulation": {
                    "polls_sent": 10,
                    "replies": 10,
                    "misses": 0,
                    "max_consecutive_misses": 0,
                    "latency_max_ms": 4.0,
                    "latency_p99_ms": 3.9,
                },
            },
            {
                "verdict": "error-04",
                "simulation": {
                    "polls_sent": 10,
                    "replies": 7,
                    "misses": 3,
                    "max_consecutive_misses": 3,
                    "latency_max_ms": 8.5,
                    "latency_p99_ms": 7.5,
                },
            },
        ],
    )

    assert report["verdict"] == "error-04"
    assert report["total_polls_sent"] == 20
    assert report["total_replies"] == 17
    assert report["total_misses"] == 3
    assert report["max_consecutive_misses"] == 3
    assert report["worst_latency_max_ms"] == 8.5
