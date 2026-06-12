"""Closed-loop virtual SupraMatic 4 simulator."""

from __future__ import annotations

import json
import math
import random
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from . import protocol
from .protocol import BroadcastState
from .transport import Transport


STEADY_CYCLE_US = 70_000
BROADCAST_TO_POLL_GAP_US = 25_000
POLL_TO_NEXT_BROADCAST_GAP_US = 26_000
DEFAULT_MISSED_POLL_THRESHOLD = 5
DEFAULT_SPEED_FACTOR = 20.0
DEFAULT_RESPONSE_TIMEOUT_S = 0.03


@dataclass
class SimulationReport:
    polls_sent: int = 0
    replies: int = 0
    misses: int = 0
    consecutive_misses: int = 0
    max_consecutive_misses: int = 0
    scan_ok: bool = False
    command_replies: int = 0
    fault_checks: int = 0
    fault_recoveries: int = 0
    fault_unexpected_responses: int = 0
    ignored_responses: int = 0
    button_observations: dict[str, int] = field(default_factory=dict)
    verdict: str = "ok"
    latencies_ms: list[float] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def as_dict(self) -> dict[str, object]:
        latencies = sorted(self.latencies_ms)
        return {
            "polls_sent": self.polls_sent,
            "replies": self.replies,
            "misses": self.misses,
            "consecutive_misses": self.consecutive_misses,
            "max_consecutive_misses": self.max_consecutive_misses,
            "scan_ok": self.scan_ok,
            "command_replies": self.command_replies,
            "fault_checks": self.fault_checks,
            "fault_recoveries": self.fault_recoveries,
            "fault_unexpected_responses": self.fault_unexpected_responses,
            "ignored_responses": self.ignored_responses,
            "button_observations": self.button_observations,
            "latency_min_ms": latencies[0] if latencies else None,
            "latency_mean_ms": (sum(latencies) / len(latencies)) if latencies else None,
            "latency_p99_ms": percentile(latencies, 99) if latencies else None,
            "latency_max_ms": latencies[-1] if latencies else None,
            "verdict": self.verdict,
            "notes": self.notes,
        }

    def write(self, path: Path) -> None:
        path.write_text(json.dumps(self.as_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")


def percentile(sorted_values: list[float], pct: int) -> float | None:
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return sorted_values[0]
    rank = (pct / 100.0) * (len(sorted_values) - 1)
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return sorted_values[low]
    weight = rank - low
    return sorted_values[low] * (1.0 - weight) + sorted_values[high] * weight


class SupraMaticSimulator:
    def __init__(
        self,
        transport: Transport,
        *,
        slave_id: int = protocol.SLAVE_ID,
        speed_factor: float = DEFAULT_SPEED_FACTOR,
        missed_poll_threshold: int = DEFAULT_MISSED_POLL_THRESHOLD,
        response_timeout_s: float = DEFAULT_RESPONSE_TIMEOUT_S,
        rng_seed: int = 0x5A17,
        expected_buttons: set[str] | None = None,
    ) -> None:
        self.transport = transport
        self.slave_id = slave_id
        self.speed_factor = speed_factor
        self.missed_poll_threshold = missed_poll_threshold
        self.response_timeout_s = response_timeout_s
        self.rng = random.Random(rng_seed)
        self.expected_buttons = expected_buttons or set()
        self.rx_buffer = bytearray()
        self.report = SimulationReport()

    def scaled_sleep(self, gap_us: int, jitter: bool = False) -> None:
        factor = self.rng.uniform(0.25, 1.75) if jitter else 1.0
        delay_s = (gap_us / 1_000_000.0) * factor / max(self.speed_factor, 1.0)
        if delay_s >= 0.0001:
            time.sleep(delay_s)

    def write_frame(self, frame: bytes, *, split: bool = False) -> None:
        if split and len(frame) > 3:
            cut = 1 + self.rng.randrange(len(frame) - 1)
            self.transport.write(frame[:cut])
            self.scaled_sleep(1_500)
            self.transport.write(frame[cut:])
        else:
            self.transport.write(frame)

    def read_response(
        self,
        timeout_s: float | None = None,
        *,
        match: Callable[[bytes], bool] | None = None,
        context: str = "response",
    ) -> bytes | None:
        deadline = time.monotonic() + (self.response_timeout_s if timeout_s is None else timeout_s)
        while time.monotonic() < deadline:
            chunk = self.transport.read_available(max(deadline - time.monotonic(), 0.0))
            if chunk:
                self.rx_buffer.extend(chunk)
            while self.rx_buffer:
                expected = protocol.response_expected_len(bytes(self.rx_buffer))
                if expected is None:
                    break
                if expected < 0:
                    del self.rx_buffer[0]
                    continue
                if len(self.rx_buffer) < expected:
                    break
                frame = bytes(self.rx_buffer[:expected])
                if protocol.crc_ok(frame):
                    del self.rx_buffer[:expected]
                    if match is None or match(frame):
                        return frame
                    self.report.ignored_responses += 1
                    if self.report.ignored_responses <= 8:
                        self.report.notes.append(f"{context} ignored unexpected response {frame.hex()}")
                    continue
                del self.rx_buffer[0]
        return None

    def run(self, cycles: int, faults: set[str] | None = None, command: str | None = None) -> SimulationReport:
        faults = faults or set()
        self.run_bus_scan()
        if not self.report.scan_ok:
            self.report.verdict = "scan-failed"
            return self.report

        for cycle in range(cycles):
            if self.report.consecutive_misses >= self.missed_poll_threshold:
                self.report.verdict = "error-04"
                break
            self.run_cycle(cycle, faults=faults, command=command)

        missing_buttons = sorted(button for button in self.expected_buttons if button not in self.report.button_observations)
        if self.report.verdict == "ok" and missing_buttons:
            self.report.verdict = "button-missing"
            self.report.notes.append(f"missing button observations: {','.join(missing_buttons)}")
        elif self.report.fault_unexpected_responses:
            self.report.verdict = "fault-failed"
        elif self.report.consecutive_misses >= self.missed_poll_threshold:
            self.report.verdict = "error-04"
        elif self.report.verdict == "ok":
            if self.transport.__class__.__name__ == "SerialTransport":
                if getattr(self.transport, "low_latency_enabled", False):
                    self.report.notes.append(
                        "serial HIL uses pyserial 8E1 with Linux low_latency mode requested; "
                        "logic-analyzer electrical timing remains separate"
                    )
                else:
                    low_latency_error = getattr(self.transport, "low_latency_error", None)
                    detail = f" ({low_latency_error})" if low_latency_error else ""
                    self.report.notes.append(
                        "serial HIL uses pyserial 8E1; Linux low_latency mode unavailable"
                        f"{detail}; logic-analyzer electrical timing remains separate"
                    )
            else:
                self.report.notes.append("pty/socketpair simulation does not model parity, baud tolerance, or wire timing")
        return self.report

    def run_bus_scan(self) -> None:
        start = time.monotonic()
        self.write_frame(protocol.bus_scan_request(self.slave_id))
        frame = self.read_response(
            match=lambda response: response == protocol.SCAN_RESPONSE,
            context="bus scan",
        )
        if frame == protocol.SCAN_RESPONSE:
            self.report.scan_ok = True
            self.report.latencies_ms.append((time.monotonic() - start) * 1000.0)
        else:
            self.report.notes.append(f"bus scan failed: {frame.hex() if frame else 'timeout'}")

    def run_cycle(self, cycle: int, *, faults: set[str], command: str | None) -> None:
        state = self.broadcast_for_cycle(cycle, command)
        self.write_frame(protocol.broadcast_status(state))
        self.scaled_sleep(BROADCAST_TO_POLL_GAP_US, jitter="jitter" in faults)

        counter = cycle & 0xFF
        if cycle == 1:
            self.inject_faults(faults, counter)

        if command == "open" and cycle == 2:
            ack = self.transport.command("press open")
            if not ack.startswith("OK"):
                self.report.notes.append(f"press open rejected: {ack}")
        elif command == "close" and cycle == 2:
            ack = self.transport.command("press close")
            if not ack.startswith("OK"):
                self.report.notes.append(f"press close rejected: {ack}")
        elif command == "light" and cycle == 2:
            self.run_light_command(0x02, enabled=True)

        self.send_status_poll(counter, split="split" in faults)
        self.scaled_sleep(POLL_TO_NEXT_BROADCAST_GAP_US, jitter="jitter" in faults)

    def broadcast_for_cycle(self, cycle: int, command: str | None) -> BroadcastState:
        if command == "open":
            position = min(200, cycle * 8)
            state = 0x20 if position >= 200 else 0x01
            return BroadcastState(target=200, current=position, state=state)
        if command == "close":
            position = max(0, 200 - cycle * 8)
            state = 0x40 if position <= 0 else 0x02
            return BroadcastState(target=0, current=position, state=state)
        return protocol.BROADCAST_CLOSED

    def send_status_poll(self, counter: int, *, split: bool = False) -> None:
        frame = protocol.status_poll(counter, self.slave_id)
        self.report.polls_sent += 1
        start = time.monotonic()
        self.write_frame(frame, split=split)
        response = self.read_response(
            match=lambda reply: protocol.decode_response_kind(reply) == "status"
            and protocol.response_counter(reply) == counter,
            context=f"status poll {counter:02x}",
        )
        if response is not None:
            button = protocol.decode_status_button(response)
            if button is not None:
                self.report.button_observations[button] = self.report.button_observations.get(button, 0) + 1
            latency_ms = (time.monotonic() - start) * 1000.0
            self.report.latencies_ms.append(latency_ms)
            self.report.replies += 1
            self.report.consecutive_misses = 0
        else:
            self.report.misses += 1
            self.report.consecutive_misses += 1
            self.report.max_consecutive_misses = max(
                self.report.max_consecutive_misses, self.report.consecutive_misses
            )

    def run_light_command(self, counter: int, *, enabled: bool) -> None:
        self.write_frame(protocol.light_command(counter, enabled, self.slave_id))
        expected = protocol.COMMAND_RESPONSE_BY_COUNTER.get(counter)
        response = self.read_response(
            match=lambda reply: expected is not None and reply == expected,
            context=f"light command {counter:02x}",
        )
        if response == expected:
            self.report.command_replies += 1
        else:
            self.report.notes.append(f"light command failed: {response.hex() if response else 'timeout'}")

    def inject_faults(self, faults: set[str], counter: int) -> None:
        valid = protocol.status_poll(counter, self.slave_id)
        if "garbage" in faults:
            self.transport.write(b"\x55\x00\xff\x10")
        if "corrupt-crc" in faults:
            corrupt = bytearray(valid)
            corrupt[-1] ^= 0x55
            self.expect_no_response(bytes(corrupt), "corrupt-crc")
        if "truncated" in faults:
            self.expect_no_response(valid[:8], "truncated")
        if "duplicate" in faults:
            self.send_status_poll(counter)

    def expect_no_response(self, frame: bytes, name: str) -> None:
        self.report.fault_checks += 1
        self.write_frame(frame)
        response = self.read_response(timeout_s=0.009)
        if response is None:
            self.report.fault_recoveries += 1
        else:
            self.report.fault_unexpected_responses += 1
            self.report.notes.append(f"{name} produced response {response.hex()}")
