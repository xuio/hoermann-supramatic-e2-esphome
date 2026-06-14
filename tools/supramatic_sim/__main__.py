from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .simulator import (
    DEFAULT_MISSED_POLL_THRESHOLD,
    DEFAULT_SPEED_FACTOR,
    SupraMaticSimulator,
    cycles_for_duration_hours,
)
from .transport import PtyHostTransport, SerialTransport, SocketPairHostTransport


FAULTS = {"corrupt-crc", "truncated", "duplicate", "jitter", "garbage", "split"}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the virtual SupraMatic 4 HCP2 master")
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument("--pty", action="store_true", help="Run against the host responder through a PTY")
    transport.add_argument("--socketpair", action="store_true", help="Run against the host responder through socketpair")
    transport.add_argument("--serial", help="Run against a real serial device for HIL")
    parser.add_argument("--cycles", type=int, default=1000)
    parser.add_argument(
        "--duration-hours",
        type=float,
        help="Run for this approximate wall-clock duration at the selected speed factor; use --speed-factor 1 for real-time HIL",
    )
    parser.add_argument("--speed-factor", type=float, default=DEFAULT_SPEED_FACTOR)
    parser.add_argument("--dut-response-delay-us", type=int, default=4200)
    parser.add_argument("--missed-poll-threshold", type=int, default=DEFAULT_MISSED_POLL_THRESHOLD)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--trace", type=Path, help="Write per-poll JSONL trace for correlation with LA captures")
    parser.add_argument("--progress", type=Path, help="Write low-volume JSONL progress snapshots")
    parser.add_argument("--progress-interval-s", type=float, default=60.0)
    parser.add_argument("--no-progress-fsync", action="store_true", help="Flush progress JSONL without fsync")
    parser.add_argument("--abort-on-miss", action="store_true", help="Stop immediately after the first missed poll")
    parser.add_argument("--fault", action="append", choices=sorted(FAULTS), default=[])
    parser.add_argument("--command", choices=["open", "close", "light"])
    parser.add_argument("--expect-button", action="append", choices=["open", "close", "stop", "vent", "half", "light"], default=[])
    parser.add_argument("--selftest", action="store_true")
    return parser


def run_once(args: argparse.Namespace) -> dict[str, object]:
    duration_hours = getattr(args, "duration_hours", None)
    cycles = (
        cycles_for_duration_hours(duration_hours, args.speed_factor)
        if duration_hours is not None
        else args.cycles
    )
    duration_s = duration_hours * 3600.0 if duration_hours is not None else None
    if args.serial:
        transport = SerialTransport(args.serial)
    elif args.socketpair:
        transport = SocketPairHostTransport(response_delay_us=args.dut_response_delay_us)
    else:
        transport = PtyHostTransport(response_delay_us=args.dut_response_delay_us)

    try:
        simulator = SupraMaticSimulator(
            transport,
            speed_factor=args.speed_factor,
            missed_poll_threshold=args.missed_poll_threshold,
            expected_buttons=set(args.expect_button),
            trace_path=args.trace,
            abort_on_first_miss=getattr(args, "abort_on_miss", False),
            progress_path=getattr(args, "progress", None),
            progress_interval_s=getattr(args, "progress_interval_s", 60.0),
            progress_fsync=not getattr(args, "no_progress_fsync", False),
        )
        report = simulator.run(cycles, faults=set(args.fault), command=args.command, duration_s=duration_s)
        if args.report:
            report.write(args.report)
        return report.as_dict()
    finally:
        if "simulator" in locals():
            simulator.close()
        transport.close()


def selftest() -> int:
    scenarios = [
        argparse.Namespace(
            pty=True,
            socketpair=False,
            serial=None,
            cycles=50,
            duration_hours=None,
            speed_factor=50.0,
            dut_response_delay_us=4200,
            missed_poll_threshold=DEFAULT_MISSED_POLL_THRESHOLD,
            report=None,
            trace=None,
            progress=None,
            progress_interval_s=60.0,
            no_progress_fsync=False,
            abort_on_miss=False,
            fault=[],
            command=None,
            expect_button=[],
        ),
        argparse.Namespace(
            pty=False,
            socketpair=True,
            serial=None,
            cycles=50,
            duration_hours=None,
            speed_factor=50.0,
            dut_response_delay_us=4200,
            missed_poll_threshold=DEFAULT_MISSED_POLL_THRESHOLD,
            report=None,
            trace=None,
            progress=None,
            progress_interval_s=60.0,
            no_progress_fsync=False,
            abort_on_miss=False,
            fault=["corrupt-crc", "truncated", "duplicate", "jitter", "garbage", "split"],
            command="open",
            expect_button=["open"],
        ),
    ]
    for scenario in scenarios:
        result = run_once(scenario)
        if result["verdict"] != "ok":
            print(f"selftest failed: {result}", file=sys.stderr)
            return 1
    print("garage-supramatic-sim selftest ok")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.pty and not args.socketpair and not args.serial:
        args.pty = True
    if args.duration_hours is not None:
        try:
            args.cycles = cycles_for_duration_hours(args.duration_hours, args.speed_factor)
        except ValueError as exc:
            parser.error(str(exc))
    if args.cycles < 1:
        parser.error("--cycles must be positive")
    if args.speed_factor <= 0:
        parser.error("--speed-factor must be positive")
    if args.dut_response_delay_us < 0:
        parser.error("--dut-response-delay-us must not be negative")
    if args.progress_interval_s < 0:
        parser.error("--progress-interval-s must not be negative")
    result = run_once(args)
    print(
        "verdict={verdict} polls={polls_sent} replies={replies} misses={misses} "
        "latency_p99_ms={latency_p99_ms}".format(**result)
    )
    return 0 if result["verdict"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
