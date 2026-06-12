from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from tools.supramatic_sim.simulator import DEFAULT_MISSED_POLL_THRESHOLD, SupraMaticSimulator

from .emulator import LPEmuError, LPEmuTransport, rvc_smoke, write_report
from .dual_iss import run_mailbox_suite


FAULTS = {"corrupt-crc", "truncated", "duplicate", "jitter", "garbage", "split"}
DEFAULT_LP_BLOB = Path(__file__).resolve().parents[2] / "firmware" / "hcp2-lp" / "build" / "hcp2_lp.bin"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the HCP2 LP-core blob in a Unicorn ISS harness")
    parser.add_argument("--blob", type=Path, default=DEFAULT_LP_BLOB, help="Path to hcp2_lp.bin or hcp2_lp.elf")
    parser.add_argument("--cycles", type=int, default=1000, help="Steady-state SupraMatic cycles to run")
    parser.add_argument("--speed-factor", type=float, default=1_000_000.0)
    parser.add_argument("--missed-poll-threshold", type=int, default=DEFAULT_MISSED_POLL_THRESHOLD)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--trace", type=Path, help="Write canonical ISS trace JSONL")
    parser.add_argument("--fault", action="append", choices=sorted(FAULTS), default=[])
    parser.add_argument("--command", choices=["open", "close", "light"])
    parser.add_argument("--rvc-smoke", action="store_true", help="Run a compressed-instruction smoke test and exit")
    parser.add_argument("--dual", action="store_true", help="Run the Phase 0d dual-ISS harness")
    parser.add_argument("--interleave", type=int, default=64, help="Instruction slice for each dual-ISS engine")
    parser.add_argument("--suite", choices=["mailbox"], default="mailbox", help="Dual-ISS suite to run")
    return parser


def run_closed_loop(args: argparse.Namespace) -> dict[str, object]:
    transport = LPEmuTransport(args.blob)
    try:
        simulator = SupraMaticSimulator(
            transport,
            speed_factor=args.speed_factor,
            missed_poll_threshold=args.missed_poll_threshold,
        )
        sim_report = simulator.run(args.cycles, faults=set(args.fault), command=args.command)
        payload = {
            "verdict": sim_report.verdict,
            "simulator": sim_report.as_dict(),
            "lp_emu": transport.emulator.report(),
        }
        if args.report:
            write_report(args.report, payload)
        if args.trace:
            args.trace.write_text(
                "".join(json.dumps(event, sort_keys=True) + "\n" for event in transport.emulator.trace_events),
                encoding="utf-8",
            )
        return payload
    finally:
        transport.close()


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.rvc_smoke:
        try:
            ok = rvc_smoke()
        except Exception as exc:
            print(f"rvc smoke failed: {exc}", file=sys.stderr)
            return 1
        print("rvc smoke ok" if ok else "rvc smoke failed")
        return 0 if ok else 1

    if args.dual:
        if args.interleave < 1:
            parser.error("--interleave must be positive")
        if args.trace:
            parser.error("--trace is only valid for closed-loop LP emulation")
        if args.fault:
            parser.error("--fault is only valid for closed-loop LP emulation")
        if args.command:
            parser.error("--command is only valid for closed-loop LP emulation")
        try:
            result = run_mailbox_suite(args.blob, interleave=args.interleave)
        except Exception as exc:
            print(f"garage-hcp2-lp-emu dual ISS failed: {exc}", file=sys.stderr)
            return 1
        if args.report:
            write_report(args.report, result)
        print(
            "dual_iss={verdict} suite={suite} interleave={interleave} checks={checks} "
            "unmodeled_mmio={unmodeled}".format(
                verdict=result["verdict"],
                suite=result["suite"],
                interleave=result["interleave"],
                checks=",".join(result["checks"]),
                unmodeled=len(result["lp_emu"]["unmodeled_mmio"]),
            )
        )
        return 0

    if args.cycles < 1:
        parser.error("--cycles must be positive")
    if args.speed_factor <= 0:
        parser.error("--speed-factor must be positive")

    try:
        result = run_closed_loop(args)
    except Exception as exc:
        print(f"garage-hcp2-lp-emu failed: {exc}", file=sys.stderr)
        return 1

    sim = result["simulator"]
    lp = result["lp_emu"]
    print(
        "verdict={verdict} polls={polls_sent} replies={replies} misses={misses} "
        "lp_tx_high_water={tx_fifo_high_water} unmodeled_mmio={unmodeled}".format(
            verdict=result["verdict"],
            polls_sent=sim["polls_sent"],
            replies=sim["replies"],
            misses=sim["misses"],
            tx_fifo_high_water=lp["tx_fifo_high_water"],
            unmodeled=len(lp["unmodeled_mmio"]),
        )
    )
    if result["verdict"] != "ok":
        return 1
    if lp["unmodeled_mmio"] or lp["tx_overflows"] or lp["tx_when_de_low"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
