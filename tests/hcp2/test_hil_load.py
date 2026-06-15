from __future__ import annotations

from pathlib import Path

import pytest

from tools.hcp2_hil_load import (
    EmulatedEspHomeCommandSender,
    aggregate_reports,
    build_parser,
    effective_cycles,
    effective_duration_s,
    effective_load_commands,
    indexed_trace_path,
    parse_button_object_ids,
    resolve_command_mode,
    run_session,
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
    args = parse_args(
        "--repeat",
        "2",
        "--cycles",
        "10",
        "--preset",
        "none",
        "--scenario",
        "goto-position",
        "--fault",
        "corrupt-crc",
        "--fault-every-cycles",
        "5",
        "--fault-cycle",
        "7",
        "--door-travel-cycles",
        "12",
        "--goto-position-raw",
        "90",
    )
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
    assert report["command_mode"] == "emulated"
    assert report["scenario"] == "goto-position"
    assert report["fault_every_cycles"] == 5
    assert report["fault_cycles"] == [7]
    assert report["door_model"]["travel_cycles"] == 12
    assert report["door_model"]["goto_position_raw"] == 90
    assert report["total_polls_sent"] == 20
    assert report["total_replies"] == 17
    assert report["total_misses"] == 3
    assert report["max_consecutive_misses"] == 3
    assert report["worst_latency_max_ms"] == 8.5


def test_hil_load_parses_button_object_id_mapping() -> None:
    assert parse_button_object_ids(
        [
            "half=garage_door_hcp2_tester_half_command",
            "vent=garage_door_hcp2_tester_vent_command",
        ]
    ) == {
        "half": "garage_door_hcp2_tester_half_command",
        "vent": "garage_door_hcp2_tester_vent_command",
    }

    with pytest.raises(ValueError, match="BUTTON=OBJECT_ID"):
        parse_button_object_ids(["half"])
    with pytest.raises(ValueError, match="unsupported"):
        parse_button_object_ids(["foo=bar"])


def test_hil_load_native_api_mapping_requires_host_and_key(tmp_path: Path) -> None:
    args = parse_args(
        "--esp-cover-object-id",
        "garage_door_hcp2_tester",
        "--secrets-file",
        str(tmp_path / "missing.yaml"),
    )
    with pytest.raises(ValueError, match="--esp-host"):
        run_session(args, load_commands=[])

    args = parse_args(
        "--esp-host",
        "192.0.2.10",
        "--esp-cover-object-id",
        "garage_door_hcp2_tester",
        "--secrets-file",
        str(tmp_path / "missing.yaml"),
    )
    with pytest.raises(ValueError, match="--esp-api-key"):
        run_session(args, load_commands=[])


def test_hil_load_resolves_emulated_command_mode_for_scenarios() -> None:
    assert resolve_command_mode(parse_args("--scenario", "steady"), {}) == "none"
    assert resolve_command_mode(parse_args("--scenario", "open-from-closed"), {}) == "emulated"
    assert resolve_command_mode(parse_args("--command", "open"), {}) == "emulated"
    assert resolve_command_mode(parse_args("--command", "light"), {}) == "none"
    assert (
        resolve_command_mode(parse_args("--esp-cover-object-id", "garage_door_hcp2_tester"), {})
        == "native-api"
    )
    assert (
        resolve_command_mode(
            parse_args("--command-mode", "emulated", "--esp-cover-object-id", "garage_door_hcp2_tester"),
            {},
        )
        == "emulated"
    )


def test_hil_load_emulated_command_sender_reports_virtual_coverage() -> None:
    sender = EmulatedEspHomeCommandSender()

    assert sender("open") == "OK emulated-esphome open"
    report = sender.report()

    assert report["mode"] == "emulated"
    assert report["coverage"] == "virtual-door-model-only"
    assert report["commands"] == [{"button": "open", "ok": True, "mode": "emulated"}]
    assert report["errors"] == []


def test_hil_load_emulated_mode_rejects_decoded_button_expectations() -> None:
    args = parse_args(
        "--command-mode",
        "emulated",
        "--scenario",
        "open-from-closed",
        "--expect-button",
        "open",
    )

    with pytest.raises(ValueError, match="--expect-button requires decoded HCP2 output"):
        run_session(args, load_commands=[])
