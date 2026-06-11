from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path

from tools.supramatic_sim.__main__ import run_once, selftest
from tools.supramatic_sim.simulator import DEFAULT_MISSED_POLL_THRESHOLD


def scenario(**kwargs: object) -> Namespace:
    defaults = {
        "pty": True,
        "socketpair": False,
        "serial": None,
        "cycles": 100,
        "speed_factor": 50.0,
        "dut_response_delay_us": 4500,
        "missed_poll_threshold": DEFAULT_MISSED_POLL_THRESHOLD,
        "report": None,
        "fault": [],
        "command": None,
    }
    defaults.update(kwargs)
    return Namespace(**defaults)


def test_simulator_selftest() -> None:
    assert selftest() == 0


def test_pty_functional_report(tmp_path: Path) -> None:
    report_path = tmp_path / "report.json"
    result = run_once(scenario(cycles=120, report=report_path))
    assert result["verdict"] == "ok"
    assert result["scan_ok"] is True
    assert result["polls_sent"] == 120
    assert result["replies"] == 120
    assert result["misses"] == 0
    assert result["latency_p99_ms"] is not None

    report = json.loads(report_path.read_text())
    assert report["verdict"] == "ok"
    assert report["polls_sent"] == 120


def test_socketpair_fault_recovery_and_open_command() -> None:
    result = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=100,
            fault=["corrupt-crc", "truncated", "duplicate", "jitter", "garbage", "split"],
            command="open",
        )
    )
    assert result["verdict"] == "ok"
    assert result["fault_checks"] == 2
    assert result["fault_recoveries"] == 2
    assert result["fault_unexpected_responses"] == 0
    assert result["misses"] == 0


def test_socketpair_light_command() -> None:
    result = run_once(scenario(pty=False, socketpair=True, cycles=40, command="light"))
    assert result["verdict"] == "ok"
    assert result["command_replies"] == 1
