"""Run scripted HCP2 HIL closeout checks and emit one JSON verdict."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import copy
import json
import os
import re
import shlex
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from tools.supramatic_sim.__main__ import FAULTS
from tools.supramatic_sim.simulator import DEFAULT_MISSED_POLL_THRESHOLD, DEFAULT_SPEED_FACTOR, SupraMaticSimulator
from tools.supramatic_sim.transport import SerialTransport


DEFAULT_PLAN: dict[str, Any] = {
    "steps": [
        {
            "name": "runtime",
            "cycles": 1000,
        },
        {
            "name": "fault-recovery",
            "cycles": 250,
            "faults": sorted(FAULTS),
        },
    ]
}

LA_PLAN: dict[str, Any] = {
    "steps": [
        {
            "name": "la-runtime",
            "cycles": 160,
            "speed_factor": 1.0,
            "la": {
                "duration": "14s",
                "samplerate": "1MHz",
                "channels": "tx=D1,de=D3,re=D5,rx=D7",
                "min_status_frames": 80,
                "max_de_high_us": 9000,
                "allow_initial_de_high": True,
                "allow_initial_tx_low": True,
                "allow_re_high_during_de": True,
            },
        }
    ]
}

OTA_RESTART_CYCLES = 500
API_RESTART_CYCLES = 350


@dataclass
class CommandRecord:
    name: str
    command: str
    delay_s: float = 0.0
    returncode: int | None = None
    duration_s: float | None = None
    stdout_tail: str = ""
    stderr_tail: str = ""


@dataclass
class RunningCommand:
    record: CommandRecord
    process: subprocess.Popen[str]
    started_s: float = field(default_factory=time.monotonic)
    terminated: bool = False


def _tail(text: str, limit: int = 4000) -> str:
    return text[-limit:] if len(text) > limit else text


def console_command(name: str) -> list[str]:
    sibling = Path(sys.executable).with_name(name)
    if sibling.exists():
        return [str(sibling)]
    return ["uv", "run", name]


def load_api_key(*, api_key: str | None, secrets_file: Path, secret_name: str) -> str:
    if api_key:
        return api_key
    env_key = os.environ.get("ESPHOME_API_KEY")
    if env_key:
        return env_key
    text = secrets_file.read_text(encoding="utf-8")
    match = re.search(rf"^{re.escape(secret_name)}:\s*[\"']?([^\"'\n#]+)", text, re.MULTILINE)
    if not match:
        raise SystemExit(f"Could not find {secret_name!r} in {secrets_file}")
    return match.group(1).strip()


async def api_restart_async(
    *,
    host: str,
    port: int,
    api_key: str,
    expected_name: str,
    restart_object_id: str,
) -> dict[str, Any]:
    try:
        from aioesphomeapi import APIClient  # type: ignore
    except ImportError as err:
        raise RuntimeError("aioesphomeapi is required; run this tool through uv") from err

    client = APIClient(
        host,
        port,
        None,
        noise_psk=api_key,
        expected_name=expected_name,
        client_info="garage-hcp2-closeout",
    )
    await client.connect(login=True)
    try:
        _info, entities, _services = await client.device_info_and_list_entities()
        buttons: list[str] = []
        restart_key: int | None = None
        restart_device_id = 0
        for entity in entities:
            if type(entity).__name__ != "ButtonInfo":
                continue
            object_id = getattr(entity, "object_id", "")
            buttons.append(object_id)
            if object_id == restart_object_id:
                restart_key = int(entity.key)
                restart_device_id = int(getattr(entity, "device_id", 0))
        if restart_key is None:
            raise RuntimeError(f"Could not find restart button {restart_object_id!r}; buttons: {buttons}")
        client.button_command(restart_key, device_id=restart_device_id)
        await asyncio.sleep(0.25)
        return {
            "host": host,
            "port": port,
            "expected_name": expected_name,
            "restart_object_id": restart_object_id,
            "button_key": restart_key,
            "device_id": restart_device_id,
            "verdict": "sent",
        }
    finally:
        with contextlib.suppress(Exception):
            await client.disconnect()


def invoke_api_restart(args: argparse.Namespace) -> int:
    api_key = load_api_key(
        api_key=args.api_key,
        secrets_file=args.secrets_file,
        secret_name=args.api_key_secret,
    )
    result = asyncio.run(
        api_restart_async(
            host=args.esp_host,
            port=args.esp_api_port,
            api_key=api_key,
            expected_name=args.expected_name,
            restart_object_id=args.restart_object_id,
        )
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def run_shell(command: str, *, name: str = "command", delay_s: float = 0.0) -> CommandRecord:
    if delay_s > 0:
        time.sleep(delay_s)
    started = time.monotonic()
    result = subprocess.run(
        ["/bin/sh", "-c", command],
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return CommandRecord(
        name=name,
        command=command,
        delay_s=delay_s,
        returncode=result.returncode,
        duration_s=round(time.monotonic() - started, 3),
        stdout_tail=_tail(result.stdout),
        stderr_tail=_tail(result.stderr),
    )


def start_load(command: str, *, name: str) -> RunningCommand:
    process = subprocess.Popen(
        ["/bin/sh", "-c", command],
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return RunningCommand(CommandRecord(name=name, command=command), process)


def stop_load(load: RunningCommand, *, terminate: bool = True, timeout_s: float = 5.0) -> None:
    if load.process.poll() is None and not terminate:
        try:
            load.process.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            terminate = True
    if load.process.poll() is None and terminate:
        load.terminated = True
        load.process.terminate()
        try:
            load.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            load.process.kill()
            load.process.wait(timeout=5)
    stdout, stderr = load.process.communicate(timeout=1)
    load.record.returncode = load.process.returncode
    load.record.duration_s = round(time.monotonic() - load.started_s, 3)
    load.record.stdout_tail = _tail(stdout or "")
    load.record.stderr_tail = _tail(stderr or "")


def command_record_dict(record: CommandRecord) -> dict[str, Any]:
    return {
        "name": record.name,
        "command": record.command,
        "delay_s": record.delay_s,
        "returncode": record.returncode,
        "duration_s": record.duration_s,
        "stdout_tail": record.stdout_tail,
        "stderr_tail": record.stderr_tail,
    }


def load_record_dict(load: RunningCommand) -> dict[str, Any]:
    payload = command_record_dict(load.record)
    payload["terminated"] = load.terminated
    return payload


def start_la_capture(step: dict[str, Any], output_dir: Path) -> RunningCommand | None:
    la = step.get("la")
    if not la:
        return None

    name = str(step.get("name", "step"))
    raw_output = la.get("output")
    output = Path(raw_output) if raw_output else output_dir / f"{name}-la.csv"
    if raw_output and not output.is_absolute():
        output = output_dir / output
    output.parent.mkdir(parents=True, exist_ok=True)

    command = [
        sys.executable,
        "-m",
        "tools.hcp2_hil_la",
        "capture",
        "--sigrok-cli",
        str(la.get("sigrok_cli", "sigrok-cli")),
        "--driver",
        str(la.get("driver", "fx2lafw")),
        "--samplerate",
        str(la.get("samplerate", "500k")),
        "--duration",
        str(la.get("duration", "10s")),
        "--channels",
        str(la.get("channels", "tx=D1,de=D3,re=D5,rx=D7")),
        "--output",
        str(output),
        "--output-format",
        str(la.get("output_format", "csv")),
    ]
    load = start_load(shlex.join(command), name=f"{name}:la-capture")
    la["resolved_output"] = str(output)
    return load


def run_la_verifier(step: dict[str, Any], output_dir: Path) -> dict[str, Any] | None:
    la = step.get("la")
    if not la or not la.get("verify", True):
        return None

    capture = Path(la["resolved_output"])
    raw_output = la.get("verify_output")
    output = Path(raw_output) if raw_output else output_dir / f"{step.get('name', 'step')}-la-verify.json"
    if raw_output and not output.is_absolute():
        output = output_dir / output
    output.parent.mkdir(parents=True, exist_ok=True)

    command = [
        sys.executable,
        "-m",
        "tools.hcp2_hil_la",
        "verify",
        "--input",
        str(capture),
        "--channels",
        str(la.get("channels", "tx=D1,de=D3,re=D5,rx=D7")),
        "--baud",
        str(la.get("baud", 57600)),
        "--max-de-high-us",
        str(la.get("max_de_high_us", 9000)),
        "--min-status-frames",
        str(la.get("min_status_frames", 0)),
        "--output",
        str(output),
    ]
    if la.get("allow_tx_outside_de", False):
        command.append("--allow-tx-outside-de")
    if la.get("allow_initial_de_high", False):
        command.append("--allow-initial-de-high")
    if la.get("allow_initial_tx_low", False):
        command.append("--allow-initial-tx-low")
    if la.get("allow_re_high", False):
        command.append("--allow-re-high")
    if la.get("allow_re_high_during_de", False):
        command.append("--allow-re-high-during-de")
    if la.get("allow_de_without_tx", False):
        command.append("--allow-de-without-tx")
    if la.get("allow_status_gaps", False):
        command.append("--allow-status-gaps")
    if "ignore_before_us" in la:
        command.extend(["--ignore-before-us", str(la["ignore_before_us"])])

    record = run_shell(shlex.join(command), name=f"{step.get('name', 'step')}:la-verify")
    payload = command_record_dict(record)
    payload["output"] = str(output)
    if output.exists():
        try:
            payload["report"] = json.loads(output.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            payload["report"] = None
    return payload


def run_simulation_step(serial: str, step: dict[str, Any], output_dir: Path) -> dict[str, Any]:
    name = str(step.get("name", "step"))
    cycles = int(step.get("cycles", 1000))
    if cycles <= 0:
        raise ValueError(f"{name}: cycles must be positive")

    report_path = output_dir / f"{name}-sim.json"
    trace_path = output_dir / f"{name}-sim.jsonl" if step.get("trace", True) else None
    faults = set(step.get("faults", []))
    unknown_faults = sorted(faults - FAULTS)
    if unknown_faults:
        raise ValueError(f"{name}: unknown faults: {', '.join(unknown_faults)}")

    payload: dict[str, Any] = {
        "name": name,
        "cycles": cycles,
        "pre_commands": [],
        "during_commands": [],
        "load_commands": [],
        "post_commands": [],
        "la_capture": None,
        "la_verify": None,
        "simulation_report": str(report_path),
        "trace": str(trace_path) if trace_path else None,
        "verdict": "not-run",
    }

    for index, command in enumerate(step.get("pre_commands", [])):
        record = run_shell(str(command), name=f"{name}:pre:{index}")
        payload["pre_commands"].append(command_record_dict(record))
        if record.returncode != 0:
            payload["verdict"] = "pre-command-failed"
            return payload

    loads: list[RunningCommand] = []
    scheduled_records: list[CommandRecord] = []
    threads: list[threading.Thread] = []
    la_capture = start_la_capture(step, output_dir)
    if la_capture is not None:
        loads.append(la_capture)
        payload["la_capture"] = load_record_dict(la_capture)

    for index, command in enumerate(step.get("load_commands", [])):
        loads.append(start_load(str(command), name=f"{name}:load:{index}"))

    def schedule(command_spec: dict[str, Any], index: int) -> None:
        record = run_shell(
            str(command_spec["command"]),
            name=str(command_spec.get("name", f"{name}:during:{index}")),
            delay_s=float(command_spec.get("delay_s", 0.0)),
        )
        scheduled_records.append(record)

    for index, command_spec in enumerate(step.get("during_commands", [])):
        thread = threading.Thread(target=schedule, args=(command_spec, index), daemon=True)
        thread.start()
        threads.append(thread)

    transport = None
    try:
        transport = SerialTransport(serial)
        simulator = SupraMaticSimulator(
            transport,
            speed_factor=float(step.get("speed_factor", DEFAULT_SPEED_FACTOR)),
            missed_poll_threshold=int(step.get("missed_poll_threshold", DEFAULT_MISSED_POLL_THRESHOLD)),
            expected_buttons=set(step.get("expect_buttons", [])),
            trace_path=trace_path,
        )
        simulation = simulator.run(cycles, faults=faults, command=step.get("command"))
        simulation.write(report_path)
        payload["simulation"] = simulation.as_dict()
        payload["verdict"] = str(payload["simulation"]["verdict"])
    finally:
        if "simulator" in locals():
            simulator.close()
        if transport is not None:
            transport.close()
        for thread in threads:
            thread.join(timeout=float(step.get("during_timeout_s", 120.0)))
        for load in loads:
            if load is la_capture:
                stop_load(load, terminate=False, timeout_s=float(step.get("la_wait_timeout_s", 180.0)))
            else:
                stop_load(load)

    payload["during_commands"] = [command_record_dict(record) for record in scheduled_records]
    payload["load_commands"] = [load_record_dict(load) for load in loads if load is not la_capture]
    if la_capture is not None:
        payload["la_capture"] = load_record_dict(la_capture)
        if la_capture.record.returncode == 0:
            payload["la_verify"] = run_la_verifier(step, output_dir)
        elif payload["verdict"] == "ok":
            payload["verdict"] = "la-capture-failed"

    if payload["la_verify"] is not None and payload["la_verify"]["returncode"] != 0 and payload["verdict"] == "ok":
        payload["verdict"] = "la-verify-failed"

    for record in scheduled_records:
        if record.returncode != 0 and payload["verdict"] == "ok":
            payload["verdict"] = "during-command-failed"
    for load in loads:
        if load is la_capture:
            continue
        if load.record.returncode not in (0, -15, 143) and payload["verdict"] == "ok":
            payload["verdict"] = "load-command-failed"

    for index, command in enumerate(step.get("post_commands", [])):
        record = run_shell(str(command), name=f"{name}:post:{index}")
        payload["post_commands"].append(command_record_dict(record))
        if record.returncode != 0 and payload["verdict"] == "ok":
            payload["verdict"] = "post-command-failed"

    return payload


def run_plan(serial: str, plan: dict[str, Any], output_dir: Path, *, fail_fast: bool) -> dict[str, Any]:
    output_dir.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "serial": serial,
        "output_dir": str(output_dir),
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "steps": [],
        "verdict": "ok",
    }
    for step in plan.get("steps", []):
        step_report = run_simulation_step(serial, step, output_dir)
        report["steps"].append(step_report)
        if step_report["verdict"] != "ok":
            report["verdict"] = "fail"
            if fail_fast:
                break
    report["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    return report


def api_restart_command(args: argparse.Namespace) -> str:
    command = [
        sys.executable,
        "-m",
        "tools.hcp2_closeout",
        "--api-restart",
        "--esp-host",
        args.esp_host,
        "--esp-api-port",
        str(args.esp_api_port),
        "--expected-name",
        args.expected_name,
        "--restart-object-id",
        args.restart_object_id,
        "--secrets-file",
        str(args.secrets_file),
        "--api-key-secret",
        args.api_key_secret,
    ]
    return shlex.join(command)


def ota_upload_command(args: argparse.Namespace) -> str:
    command = console_command("esphome") + [
        "upload",
        str(args.esphome_config),
        "--device",
        args.esp_device,
    ]
    return shlex.join(command)


def esphome_compile_command(args: argparse.Namespace) -> str:
    command = console_command("esphome") + [
        "compile",
        str(args.esphome_config),
    ]
    return shlex.join(command)


def build_ota_restart_plan(args: argparse.Namespace) -> dict[str, Any]:
    pre_commands = [] if args.skip_esphome_compile else [esphome_compile_command(args)]
    return {
        "steps": [
            {
                "name": "ota-upload",
                "cycles": OTA_RESTART_CYCLES,
                "speed_factor": 1.0,
                "pre_commands": pre_commands,
                "during_timeout_s": 240,
                "during_commands": [
                    {
                        "name": "esphome-ota-upload",
                        "delay_s": 2.0,
                        "command": ota_upload_command(args),
                    }
                ],
            },
            {
                "name": "api-restart",
                "cycles": API_RESTART_CYCLES,
                "speed_factor": 1.0,
                "during_timeout_s": 90,
                "during_commands": [
                    {
                        "name": "esphome-api-restart",
                        "delay_s": 2.0,
                        "command": api_restart_command(args),
                    }
                ],
            },
        ]
    }


def builtin_plan(args: argparse.Namespace) -> dict[str, Any]:
    if args.plan:
        return json.loads(args.plan.read_text(encoding="utf-8"))
    if args.preset == "basic":
        return copy.deepcopy(DEFAULT_PLAN)
    if args.preset == "la":
        return copy.deepcopy(LA_PLAN)
    if args.preset == "ota-restart":
        return build_ota_restart_plan(args)
    if args.preset == "full":
        plan = copy.deepcopy(DEFAULT_PLAN)
        plan["steps"].extend(copy.deepcopy(LA_PLAN)["steps"])
        plan["steps"].extend(build_ota_restart_plan(args)["steps"])
        return plan
    raise ValueError(f"unknown preset {args.preset!r}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run a scripted HCP2 HIL closeout plan")
    parser.add_argument("--serial", help="RS-485 serial device")
    parser.add_argument("--preset", choices=("basic", "la", "ota-restart", "full"), default="basic")
    parser.add_argument("--plan", type=Path, help="JSON closeout plan; defaults to a basic runtime+fault plan")
    parser.add_argument("--output-dir", type=Path, default=Path("captures/hcp2/closeout"))
    parser.add_argument("--output", type=Path, help="Write the aggregate JSON report")
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--print-template", action="store_true", help="Print a minimal plan template and exit")
    parser.add_argument("--api-restart", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--esp-host", default=os.environ.get("HCP2_ESP_HOST", "supramatic-4-dev.local"))
    parser.add_argument("--esp-device", default=os.environ.get("HCP2_ESP_DEVICE"))
    parser.add_argument("--esp-api-port", type=int, default=int(os.environ.get("ESPHOME_API_PORT", "6053")))
    parser.add_argument("--expected-name", default=os.environ.get("HCP2_ESPHOME_NAME", "supramatic-4-dev"))
    parser.add_argument(
        "--restart-object-id",
        default=os.environ.get("HCP2_RESTART_OBJECT_ID", "garage_door_hcp2_dev_restart"),
    )
    parser.add_argument("--api-key", default=os.environ.get("ESPHOME_API_KEY"))
    parser.add_argument("--api-key-secret", default="api_key_supramatic_4_dev")
    parser.add_argument("--secrets-file", type=Path, default=Path("configs/secrets.yaml"))
    parser.add_argument("--esphome-config", type=Path, default=Path("configs/supramatic-4-dev.yaml"))
    parser.add_argument(
        "--skip-esphome-compile",
        action="store_true",
        help="For ota-restart preset, assume the ESPHome firmware binary already exists on this host",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.esp_device is None:
        args.esp_device = args.esp_host
    if args.api_restart:
        return invoke_api_restart(args)
    if args.print_template:
        print(json.dumps(builtin_plan(args), indent=2, sort_keys=True))
        return 0
    if not args.serial:
        parser.error("--serial is required unless --api-restart is used")

    plan = builtin_plan(args)
    report = run_plan(args.serial, plan, args.output_dir, fail_fast=args.fail_fast)
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(
        "verdict={verdict} steps={steps} output_dir={output_dir}".format(
            verdict=report["verdict"],
            steps=len(report["steps"]),
            output_dir=report["output_dir"],
        )
    )
    return 0 if report["verdict"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
