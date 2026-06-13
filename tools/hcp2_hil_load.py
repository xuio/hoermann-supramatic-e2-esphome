"""Run HCP2 HIL simulator scenarios while external load generators are active."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from tools.supramatic_sim.__main__ import FAULTS
from tools.supramatic_sim.simulator import DEFAULT_MISSED_POLL_THRESHOLD, DEFAULT_SPEED_FACTOR, SupraMaticSimulator
from tools.supramatic_sim.transport import SerialTransport


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run HCP2 HIL under optional host/Wi-Fi/API load")
    parser.add_argument("--serial", required=True, help="RS-485 serial device")
    parser.add_argument("--cycles", type=int, default=1000)
    parser.add_argument("--speed-factor", type=float, default=DEFAULT_SPEED_FACTOR)
    parser.add_argument("--missed-poll-threshold", type=int, default=DEFAULT_MISSED_POLL_THRESHOLD)
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
    parser.add_argument("--output", type=Path, help="Write machine-readable JSON report")
    return parser


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


def run_session(args: argparse.Namespace) -> dict[str, Any]:
    if args.cycles < 1:
        raise ValueError("--cycles must be positive")
    if args.speed_factor <= 0:
        raise ValueError("--speed-factor must be positive")

    report: dict[str, Any] = {
        "serial": args.serial,
        "cycles": args.cycles,
        "speed_factor": args.speed_factor,
        "faults": sorted(args.fault),
        "command": args.command,
        "expected_buttons": sorted(args.expect_button),
        "pre_commands": [],
        "load_commands": [],
        "post_commands": [],
        "verdict": "not-run",
    }

    for command in args.pre_command:
        record = run_shell(command)
        report["pre_commands"].append(record)
        if record["returncode"] != 0:
            report["verdict"] = "pre-command-failed"
            return report

    loads: list[tuple[subprocess.Popen[bytes], dict[str, Any]]] = []
    transport = None
    try:
        for command in args.load_command:
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
        )
        simulation = simulator.run(args.cycles, faults=set(args.fault), command=args.command).as_dict()
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

    return report


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        report = run_session(args)
    except Exception as exc:
        print(f"garage-hcp2-hil-load failed: {exc}", file=sys.stderr)
        return 2
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "verdict={verdict} cycles={cycles} serial={serial}".format(
            verdict=report["verdict"],
            cycles=report["cycles"],
            serial=report["serial"],
        )
    )
    return 0 if report["verdict"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
