from __future__ import annotations

from argparse import Namespace
from pathlib import Path

import pytest

from tools import hcp2_closeout


def closeout_args(tmp_path: Path, **overrides: object) -> Namespace:
    defaults = {
        "plan": None,
        "preset": "basic",
        "esp_host": "192.0.2.10",
        "esp_device": "192.0.2.10",
        "esp_api_port": 6053,
        "expected_name": "supramatic-4-dev",
        "restart_object_id": "garage_door_hcp2_dev_restart",
        "api_key_secret": "api_key_supramatic_4_dev",
        "secrets_file": tmp_path / "secrets.yaml",
        "esphome_config": Path("configs/supramatic-4-dev.yaml"),
        "skip_esphome_compile": False,
        "sigrok_cli": None,
    }
    defaults.update(overrides)
    return Namespace(**defaults)


def test_closeout_la_preset_contains_logic_analyzer_verdict_step(tmp_path: Path) -> None:
    plan = hcp2_closeout.prepare_plan(closeout_args(tmp_path, preset="la"))

    assert [step["name"] for step in plan["steps"]] == ["la-runtime"]
    la = plan["steps"][0]["la"]
    assert la["channels"] == "tx=D1,de=D3,re=D5,rx=D7"
    assert la["allow_re_high_during_de"] is True
    assert la["min_status_frames"] > 0
    assert la["sigrok_cli"] == "sigrok-cli"


def test_closeout_la_preset_accepts_sigrok_cli_override(tmp_path: Path) -> None:
    plan = hcp2_closeout.prepare_plan(closeout_args(tmp_path, preset="la", sigrok_cli="/tmp/sigrok-cli"))

    assert plan["steps"][0]["la"]["sigrok_cli"] == "/tmp/sigrok-cli"


def test_closeout_preflight_reports_missing_sigrok_cli(tmp_path: Path) -> None:
    plan = hcp2_closeout.prepare_plan(
        closeout_args(tmp_path, preset="la", sigrok_cli="/definitely/missing/sigrok-cli")
    )

    with pytest.raises(SystemExit, match="sigrok-cli preflight failed"):
        hcp2_closeout.preflight_plan(plan)


def test_closeout_ota_restart_preset_records_commands_without_plaintext_api_key(tmp_path: Path) -> None:
    plan = hcp2_closeout.builtin_plan(closeout_args(tmp_path, preset="ota-restart"))

    assert [step["name"] for step in plan["steps"]] == ["ota-upload", "api-restart"]
    assert "esphome compile" in plan["steps"][0]["pre_commands"][0]
    ota_command = plan["steps"][0]["during_commands"][0]["command"]
    restart_command = plan["steps"][1]["during_commands"][0]["command"]

    assert "esphome upload" in ota_command
    assert "--device 192.0.2.10" in ota_command
    assert "--api-restart" in restart_command
    assert "--api-key-secret api_key_supramatic_4_dev" in restart_command
    assert "ESPHOME_API_KEY" not in restart_command


def test_closeout_full_preset_runs_basic_la_and_restart_steps(tmp_path: Path) -> None:
    plan = hcp2_closeout.builtin_plan(closeout_args(tmp_path, preset="full"))

    assert [step["name"] for step in plan["steps"]] == [
        "runtime",
        "fault-recovery",
        "la-runtime",
        "ota-upload",
        "api-restart",
    ]


def test_closeout_ota_restart_preset_can_skip_compile_for_preseeded_bench(tmp_path: Path) -> None:
    plan = hcp2_closeout.builtin_plan(closeout_args(tmp_path, preset="ota-restart", skip_esphome_compile=True))

    assert plan["steps"][0]["pre_commands"] == []
    assert "esphome upload" in plan["steps"][0]["during_commands"][0]["command"]


def test_load_api_key_from_named_secret(tmp_path: Path) -> None:
    secrets = tmp_path / "secrets.yaml"
    secrets.write_text(
        'api_key_supramatic_4_dev: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="\n',
        encoding="utf-8",
    )

    assert (
        hcp2_closeout.load_api_key(
            api_key=None,
            secrets_file=secrets,
            secret_name="api_key_supramatic_4_dev",
        )
        == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
    )
