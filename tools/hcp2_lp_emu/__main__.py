from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from tools.supramatic_sim.simulator import DEFAULT_MISSED_POLL_THRESHOLD, SupraMaticSimulator

from .emulator import LPEmuError, LPEmuTransport, rvc_smoke, write_report


FAULTS = {"corrupt-crc", "truncated", "duplicate", "jitter", "garbage", "split"}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the HCP2 LP-core blob in a Unicorn ISS harness")
    parser.add_argument("--blob", type=Path, help="Path to hcp2_lp.bin or hcp2_lp.elf")
    parser.add_argument("--cycles", type=int, default=1000, help="Steady-state SupraMatic cycles to run")
    parser.add_argument("--speed-factor", type=float, default=1_000_000.0)
    parser.add_argument("--missed-poll-threshold", type=int, default=DEFAULT_MISSED_POLL_THRESHOLD)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--trace", type=Path, help="Write canonical ISS trace JSONL")
    parser.add_argument("--fault", action="append", choices=sorted(FAULTS), default=[])
    parser.add_argument("--command", choices=["open", "close", "light"])
    parser.add_argument("--rvc-smoke", action="store_true", help="Run a compressed-instruction smoke test and exit")
    return parser


def run_closed_loop(args: argparse.Namespace) -> dict[str, object]:
    if args.blob is None:
        raise LPEmuError("--blob is required unless --rvc-smoke is used")
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
