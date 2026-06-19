from __future__ import annotations

from pathlib import Path

import pytest

import tools.hcp2_hil_load as hil_load
from tools.hcp2_hil_load import (
    EmulatedEspHomeCommandSender,
    aggregate_reports,
    build_parser,
    classify_device_health,
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
                    "host_rtt_max_ms": 4.0,
                    "host_rtt_p99_ms": 3.9,
                },
            },
            {
                "verdict": "error-04",
                "simulation": {
                    "polls_sent": 10,
                    "replies": 7,
                    "misses": 3,
                    "max_consecutive_misses": 3,
                    "host_rtt_max_ms": 8.5,
                    "host_rtt_p99_ms": 7.5,
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
    assert report["worst_host_rtt_max_ms"] == 8.5
    assert report["worst_host_rtt_p99_ms"] == 7.5
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


def test_hil_load_native_api_mapping_requires_host_but_allows_plaintext_api(tmp_path: Path, monkeypatch) -> None:
    args = parse_args(
        "--esp-cover-object-id",
        "garage_door_hcp2_tester",
        "--secrets-file",
        str(tmp_path / "missing.yaml"),
    )
    with pytest.raises(ValueError, match="--esp-host"):
        run_session(args, load_commands=[])

    seen: dict[str, object] = {}

    class DummyNativeApiCommandSender:
        def __init__(self, **kwargs):
            seen["api_key"] = kwargs["api_key"]
            self.errors = []

        def start(self) -> None:
            return

        def stop(self) -> None:
            return

        def report(self) -> dict[str, object]:
            return {"mode": "native-api"}

    class DummyTransport:
        def __init__(self, serial: str):
            self.serial = serial

        def close(self) -> None:
            return

    class DummySimulation:
        def as_dict(self) -> dict[str, object]:
            return {"verdict": "ok"}

    class DummySimulator:
        def __init__(self, *args, **kwargs):
            return

        def run(self, *args, **kwargs):
            return DummySimulation()

        def close(self) -> None:
            return

    monkeypatch.setattr(hil_load, "NativeApiCommandSender", DummyNativeApiCommandSender)
    monkeypatch.setattr(hil_load, "SerialTransport", DummyTransport)
    monkeypatch.setattr(hil_load, "SupraMaticSimulator", DummySimulator)

    args = parse_args(
        "--esp-host",
        "192.0.2.10",
        "--esp-cover-object-id",
        "garage_door_hcp2_tester",
        "--secrets-file",
        str(tmp_path / "missing.yaml"),
        "--cycles",
        "1",
    )
    report = run_session(args, load_commands=[])
    assert report["esp_api_encryption"] == "plaintext"
    assert seen["api_key"] is None


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


def health_payload(**checks):
    payload = {
        "verdict": "ok",
        "reasons": [],
        "checks": {
            "lp_mode": True,
            "lp_seen": True,
            "bus_online": True,
            "valid_broadcast": True,
            "last_poll_age_ms": 20,
            "polls_seen": 100,
            "polls_answered": 100,
            "missed_polls": 0,
            "raw_missed_polls": 0,
            "pending_response": False,
            "health_flags": 0,
            "tx_aborts": 0,
            "collisions": 0,
            "loop_overruns": 0,
            "rx_starvations": 0,
            "stuck_de_recoveries": 0,
        },
    }
    payload["checks"].update(checks)
    if payload["checks"]["health_flags"] or payload["checks"]["rx_starvations"]:
        payload["verdict"] = "fail"
        payload["reasons"] = ["lp_health_flags", "rx_starvations"]
    return payload


def test_hil_load_classifies_injected_rx_starvation_as_warning() -> None:
    report = classify_device_health(
        health_payload(health_flags=0x0002, rx_starvations=14),
        fault_injection_expected=True,
    )

    assert report["verdict"] == "warn"
    assert report["continuity_verdict"] == "ok"
    assert report["warnings"] == ["rx_starvations_during_fault_injection:14"]


def test_hil_load_classifies_unexpected_rx_starvation_as_failure() -> None:
    report = classify_device_health(
        health_payload(health_flags=0x0002, rx_starvations=1),
        fault_injection_expected=False,
    )

    assert report["verdict"] == "fail"
    assert report["continuity_verdict"] == "fail"
    assert "rx_starvations:1" in report["blocking_reasons"]


def test_hil_load_accepts_firmware_cleared_sticky_diagnostics() -> None:
    payload = health_payload(health_flags=0x0006, rx_starvations=3)
    payload["verdict"] = "ok"
    payload["safe_for_ota_restart"] = True
    payload["reasons"] = []
    payload["warnings"] = ["lp_health_flags", "rx_starvations"]
    report = classify_device_health(payload, fault_injection_expected=False)

    assert report["verdict"] == "warn"
    assert report["continuity_verdict"] == "ok"
    assert "lp_health_flags_sticky:0x0004" in report["warnings"]
    assert "rx_starvations_sticky:3" in report["warnings"]


def test_hil_load_accepts_legacy_unsafe_ota_verdict_when_continuity_is_clean() -> None:
    payload = health_payload()
    payload["verdict"] = "fail"
    payload["safe_for_ota_restart"] = False
    payload["reasons"] = []
    report = classify_device_health(payload, fault_injection_expected=False)

    assert report["verdict"] == "ok"
    assert report["continuity_verdict"] == "ok"
    assert report["blocking_reasons"] == []


def test_hil_load_treats_raw_pending_response_as_warning() -> None:
    report = classify_device_health(
        health_payload(raw_missed_polls=1, pending_response=True),
        fault_injection_expected=False,
    )

    assert report["verdict"] == "warn"
    assert report["continuity_verdict"] == "ok"
    assert report["warnings"] == ["raw_missed_polls_pending:1"]


def test_hil_load_health_classifier_never_hides_real_continuity_failures() -> None:
    missed = classify_device_health(
        health_payload(health_flags=0x0002, rx_starvations=1, missed_polls=1),
        fault_injection_expected=True,
    )
    non_rx_flag = classify_device_health(
        health_payload(health_flags=0x0006, rx_starvations=1),
        fault_injection_expected=True,
    )

    assert missed["verdict"] == "fail"
    assert "missed_polls:1" in missed["blocking_reasons"]
    assert non_rx_flag["verdict"] == "fail"
    assert "lp_health_flags_non_rx:0x0004" in non_rx_flag["blocking_reasons"]
