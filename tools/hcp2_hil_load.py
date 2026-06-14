"""Run HCP2 HIL simulator scenarios while external load generators are active."""

from __future__ import annotations

import argparse
import copy
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from tools.hcp2_hil_la import load_samples, parse_channel_map, verify_samples
from tools.supramatic_sim.__main__ import FAULTS
from tools.supramatic_sim.simulator import (
    DEFAULT_MISSED_POLL_THRESHOLD,
    DEFAULT_SPEED_FACTOR,
    SupraMaticSimulator,
    cycles_for_duration_hours,
)
from tools.supramatic_sim.transport import SerialTransport


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run HCP2 HIL under optional host/Wi-Fi/API load")
    parser.add_argument("--serial", required=True, help="RS-485 serial device")
    parser.add_argument("--cycles", type=int, default=1000)
    parser.add_argument(
        "--duration-hours",
        type=float,
        help="Run for this approximate wall-clock duration at the selected speed factor; use --speed-factor 1 for real-time HIL",
    )
    parser.add_argument("--repeat", type=int, default=1, help="Run the HIL scenario this many times")
    parser.add_argument("--settle-s", type=float, default=0.0, help="Pause between repeated HIL runs")
    parser.add_argument("--speed-factor", type=float, default=DEFAULT_SPEED_FACTOR)
    parser.add_argument("--missed-poll-threshold", type=int, default=DEFAULT_MISSED_POLL_THRESHOLD)
    parser.add_argument(
        "--preset",
        choices=["none", "hostile"],
        default="none",
        help="Optional built-in load preset. 'hostile' adds ping and API reconnect pressure when --esp-host is set.",
    )
    parser.add_argument("--esp-host", help="ESPHome device host/IP for hostile load commands")
    parser.add_argument("--esp-api-port", type=int, default=6053, help="ESPHome native API port")
    parser.add_argument("--fault", action="append", choices=sorted(FAULTS), default=[])
    parser.add_argument("--command", choices=["open", "close", "light"])
    parser.add_argument("--expect-button", action="append", choices=["open", "close", "stop", "vent", "half", "light"], default=[])
    parser.add_argument("--pre-command", action="append", default=[], help="Shell command to run before the HIL scenario")
    parser.add_argument(
        "--load-command",
        action="append",
        default=[],
        help="Shell command to keep running during the HIL scenario; terminated after the simulator exits",
    )
    parser.add_argument("--post-command", action="append", default=[], help="Shell command to run after the HIL scenario")
    parser.add_argument("--trace", type=Path, help="Write simulator per-poll JSONL trace")
    parser.add_argument("--progress-output", type=Path, help="Write low-volume JSONL progress snapshots")
    parser.add_argument("--progress-interval-s", type=float, default=60.0)
    parser.add_argument("--no-progress-fsync", action="store_true", help="Flush progress JSONL without fsync")
    parser.add_argument("--abort-on-miss", action="store_true", help="Stop immediately after the first missed poll")
    parser.add_argument("--process-nice", type=int, default=0, help="Apply os.nice() before the HIL run")
    parser.add_argument(
        "--cpu-affinity",
        help="Comma-separated CPU list for os.sched_setaffinity() on Linux, e.g. 2 or 2,3",
    )
    parser.add_argument("--la-input", type=Path, help="Analyze this CSV/JSON logic-analyzer capture after the run")
    parser.add_argument("--la-output", type=Path, help="Write the logic-analyzer report separately")
    parser.add_argument("--la-channels", help="Comma-separated logical mapping, e.g. de=D4,re=D3,tx=D5,rx=D2")
    parser.add_argument("--la-ignore-before-us", type=float, default=0.0)
    parser.add_argument("--la-min-status-frames", type=int, default=0)
    parser.add_argument("--la-allow-re-high-during-de", action="store_true")
    parser.add_argument("--output", type=Path, help="Write machine-readable JSON report")
    return parser


def hostile_load_commands(host: str, api_port: int) -> list[str]:
    quoted_host = shlex.quote(host)
    reconnect_script = (
        "import socket,time;"
        f"host={host!r};port={api_port!r};"
        "\nwhile True:\n"
        "    s=socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n"
        "    s.settimeout(0.25)\n"
        "    try:\n"
        "        s.connect((host, port))\n"
        "    except OSError:\n"
        "        pass\n"
        "    finally:\n"
        "        s.close()\n"
        "    time.sleep(0.05)\n"
    )
    return [
        f"ping -i 0.2 {quoted_host}",
        f"{shlex.quote(sys.executable)} -c {shlex.quote(reconnect_script)}",
    ]


def effective_load_commands(args: argparse.Namespace) -> list[str]:
    commands = list(args.load_command)
    if args.preset == "hostile":
        if not args.esp_host:
            raise ValueError("--preset hostile requires --esp-host")
        commands.extend(hostile_load_commands(args.esp_host, args.esp_api_port))
    return commands


def indexed_trace_path(path: Path | None, run_index: int, repeat: int) -> Path | None:
    if path is None or repeat == 1:
        return path
    return path.with_name(f"{path.stem}.run{run_index:02d}{path.suffix}")


def effective_cycles(args: argparse.Namespace) -> int:
    if args.duration_hours is not None:
        return cycles_for_duration_hours(args.duration_hours, args.speed_factor)
    return int(args.cycles)


def effective_duration_s(args: argparse.Namespace) -> float | None:
    if args.duration_hours is None:
        return None
    if args.duration_hours <= 0:
        raise ValueError("duration must be positive")
    return args.duration_hours * 3600.0


def run_shell(command: str) -> dict[str, Any]:
    started = time.time()
    result = subprocess.run(
        ["/bin/sh", "-c", command],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return {
        "command": command,
        "returncode": result.returncode,
        "duration_s": round(time.time() - started, 3),
    }


def start_load(command: str) -> tuple[subprocess.Popen[bytes], dict[str, Any]]:
    proc = subprocess.Popen(
        ["/bin/sh", "-c", command],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return proc, {"command": command, "pid": proc.pid, "returncode": None, "terminated": False}


def stop_load(proc: subprocess.Popen[bytes], record: dict[str, Any]) -> None:
    returncode = proc.poll()
    if returncode is None:
        record["terminated"] = True
        proc.terminate()
        try:
            returncode = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            returncode = proc.wait(timeout=5)
            record["killed"] = True
    record["returncode"] = returncode


def apply_host_tuning(args: argparse.Namespace) -> dict[str, Any]:
    report: dict[str, Any] = {
        "requested_nice": args.process_nice,
        "requested_cpu_affinity": args.cpu_affinity,
        "nice_applied": False,
        "cpu_affinity_applied": False,
        "errors": [],
    }
    if args.process_nice:
        try:
            os.nice(args.process_nice)
            report["nice_applied"] = True
        except OSError as exc:
            report["errors"].append(f"nice: {type(exc).__name__}: {exc}")
    if args.cpu_affinity:
        if not hasattr(os, "sched_setaffinity"):
            report["errors"].append("cpu_affinity: os.sched_setaffinity unavailable")
        else:
            try:
                cpus = {int(item.strip()) for item in args.cpu_affinity.split(",") if item.strip()}
                if not cpus:
                    raise ValueError("empty CPU affinity set")
                os.sched_setaffinity(0, cpus)
                report["cpu_affinity_applied"] = True
                report["cpu_affinity_effective"] = sorted(os.sched_getaffinity(0))
            except (OSError, ValueError) as exc:
                report["errors"].append(f"cpu_affinity: {type(exc).__name__}: {exc}")
    return report


def analyze_logic_capture(args: argparse.Namespace) -> dict[str, Any] | None:
    if args.la_input is None:
        return None
    channels = parse_channel_map(args.la_channels)
    samples = load_samples(args.la_input, channels)
    report = verify_samples(
        samples,
        ignore_before_us=args.la_ignore_before_us,
        allow_re_high_during_de=args.la_allow_re_high_during_de,
        min_status_frames=args.la_min_status_frames,
    )
    if args.la_output:
        args.la_output.parent.mkdir(parents=True, exist_ok=True)
        args.la_output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def run_session(args: argparse.Namespace, *, run_index: int = 1, load_commands: list[str] | None = None) -> dict[str, Any]:
    cycles = effective_cycles(args)
    duration_s = effective_duration_s(args)
    if cycles < 1:
        raise ValueError("--cycles must be positive")
    if args.speed_factor <= 0:
        raise ValueError("--speed-factor must be positive")
    if args.progress_interval_s < 0:
        raise ValueError("--progress-interval-s must not be negative")
    load_commands = effective_load_commands(args) if load_commands is None else load_commands

    report: dict[str, Any] = {
        "serial": args.serial,
        "run_index": run_index,
        "cycles": cycles,
        "duration_hours": args.duration_hours,
        "speed_factor": args.speed_factor,
        "abort_on_miss": args.abort_on_miss,
        "preset": args.preset,
        "esp_host": args.esp_host,
        "esp_api_port": args.esp_api_port,
        "faults": sorted(args.fault),
        "command": args.command,
        "expected_buttons": sorted(args.expect_button),
        "pre_commands": [],
        "load_commands": [],
        "post_commands": [],
        "host_tuning": {},
        "latency_authority": "host_round_trip",
        "verdict": "not-run",
    }
    report["host_tuning"] = apply_host_tuning(args) if run_index == 1 else {"skipped": "already applied"}

    for command in args.pre_command:
        record = run_shell(command)
        report["pre_commands"].append(record)
        if record["returncode"] != 0:
            report["verdict"] = "pre-command-failed"
            return report

    loads: list[tuple[subprocess.Popen[bytes], dict[str, Any]]] = []
    transport = None
    try:
        for command in load_commands:
            proc, record = start_load(command)
            loads.append((proc, record))
            report["load_commands"].append(record)

        transport = SerialTransport(args.serial)
        simulator = SupraMaticSimulator(
            transport,
            speed_factor=args.speed_factor,
            missed_poll_threshold=args.missed_poll_threshold,
            expected_buttons=set(args.expect_button),
            trace_path=args.trace,
            abort_on_first_miss=args.abort_on_miss,
            progress_path=args.progress_output,
            progress_interval_s=args.progress_interval_s,
            progress_fsync=not args.no_progress_fsync,
        )
        simulation = simulator.run(cycles, faults=set(args.fault), command=args.command, duration_s=duration_s).as_dict()
        report["simulation"] = simulation
        report["verdict"] = str(simulation["verdict"])
    finally:
        if "simulator" in locals():
            simulator.close()
        if transport is not None:
            transport.close()
        for proc, record in loads:
            stop_load(proc, record)

    for command in args.post_command:
        record = run_shell(command)
        report["post_commands"].append(record)
        if report["verdict"] == "ok" and record["returncode"] != 0:
            report["verdict"] = "post-command-failed"

    logic_report = analyze_logic_capture(args)
    if logic_report is not None:
        report["logic_analyzer"] = logic_report
        if logic_report.get("latency", {}).get("matched_status_pairs", 0):
            report["latency_authority"] = "logic_analyzer"
        if report["verdict"] == "ok" and logic_report.get("verdict") == "fail":
            report["verdict"] = "logic-analyzer-failed"

    return report


def aggregate_reports(args: argparse.Namespace, runs: list[dict[str, Any]]) -> dict[str, Any]:
    simulations = [run.get("simulation", {}) for run in runs if isinstance(run.get("simulation"), dict)]
    verdict = "ok"
    for run in runs:
        if run.get("verdict") != "ok":
            verdict = str(run.get("verdict", "failed"))
            break

    latency_max_values = [
        value
        for simulation in simulations
        if (value := simulation.get("latency_max_ms")) is not None
    ]
    latency_p99_values = [
        value
        for simulation in simulations
        if (value := simulation.get("latency_p99_ms")) is not None
    ]
    return {
        "serial": args.serial,
        "preset": args.preset,
        "esp_host": args.esp_host,
        "esp_api_port": args.esp_api_port,
        "repeat": args.repeat,
        "completed_runs": len(runs),
        "cycles_per_run": effective_cycles(args),
        "duration_hours": args.duration_hours,
        "speed_factor": args.speed_factor,
        "abort_on_miss": args.abort_on_miss,
        "faults": sorted(args.fault),
        "command": args.command,
        "expected_buttons": sorted(args.expect_button),
        "verdict": verdict,
        "total_polls_sent": sum(int(simulation.get("polls_sent", 0)) for simulation in simulations),
        "total_replies": sum(int(simulation.get("replies", 0)) for simulation in simulations),
        "total_misses": sum(int(simulation.get("misses", 0)) for simulation in simulations),
        "max_consecutive_misses": max(
            [int(simulation.get("max_consecutive_misses", 0)) for simulation in simulations] or [0]
        ),
        "worst_latency_max_ms": max(latency_max_values) if latency_max_values else None,
        "worst_latency_p99_ms": max(latency_p99_values) if latency_p99_values else None,
        "runs": runs,
    }


def run_suite(args: argparse.Namespace) -> dict[str, Any]:
    if args.repeat < 1:
        raise ValueError("--repeat must be positive")
    if args.settle_s < 0:
        raise ValueError("--settle-s must be non-negative")
    load_commands = effective_load_commands(args)
    if args.repeat == 1:
        return run_session(args, load_commands=load_commands)

    runs: list[dict[str, Any]] = []
    for index in range(1, args.repeat + 1):
        if index > 1 and args.settle_s > 0:
            time.sleep(args.settle_s)
        run_args = copy.copy(args)
        run_args.trace = indexed_trace_path(args.trace, index, args.repeat)
        run_args.progress_output = indexed_trace_path(args.progress_output, index, args.repeat)
        report = run_session(run_args, run_index=index, load_commands=load_commands)
        runs.append(report)
        if report["verdict"] != "ok":
            break
    return aggregate_reports(args, runs)


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        report = run_suite(args)
    except Exception as exc:
        print(f"garage-hcp2-hil-load failed: {exc}", file=sys.stderr)
        return 2
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "verdict={verdict} cycles={cycles} repeat={repeat} serial={serial}".format(
            verdict=report["verdict"],
            cycles=report.get("cycles", report.get("cycles_per_run")),
            repeat=report.get("repeat", 1),
            serial=report["serial"],
        )
    )
    return 0 if report["verdict"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
