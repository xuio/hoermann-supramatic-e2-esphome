"""Closed-loop virtual SupraMatic 4 simulator."""

from __future__ import annotations

import json
import math
import os
import random
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, TextIO

from . import protocol
from .door_model import (
    DoorModel,
    POSITION_OPEN,
)
from .protocol import BroadcastState
from .transport import Transport


STEADY_CYCLE_US = 70_000
BROADCAST_TO_POLL_GAP_US = 25_000
POLL_TO_NEXT_BROADCAST_GAP_US = 26_000
DEFAULT_MISSED_POLL_THRESHOLD = 5
DEFAULT_SPEED_FACTOR = 20.0
DEFAULT_RESPONSE_TIMEOUT_S = 0.03
DEFAULT_DOOR_TRAVEL_CYCLES = 260


SCENARIOS = {
    "steady",
    "open-from-closed",
    "close-from-open",
    "stop-mid-opening",
    "reverse-open-to-close",
    "reverse-close-to-open",
    "half-position",
    "vent-position",
    "light-toggle",
    "closing-obstruction",
    "goto-position",
}


COMMAND_TO_SCENARIO = {
    "open": "open-from-closed",
    "close": "close-from-open",
}


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
    button_phase_observations: dict[str, int] = field(default_factory=dict)
    emulated_button_observations: dict[str, int] = field(default_factory=dict)
    emulated_button_phase_observations: dict[str, int] = field(default_factory=dict)
    verdict: str = "ok"
    latencies_ms: list[float] = field(default_factory=list)
    latency_sum_ms: float = 0.0
    latency_min_seen_ms: float | None = None
    latency_max_seen_ms: float | None = None
    elapsed_s: float | None = None
    notes: list[str] = field(default_factory=list)
    scenario: str = "steady"
    door: dict[str, object] = field(default_factory=dict)

    def record_latency(self, latency_ms: float) -> None:
        self.latencies_ms.append(latency_ms)
        self.latency_sum_ms += latency_ms
        if self.latency_min_seen_ms is None or latency_ms < self.latency_min_seen_ms:
            self.latency_min_seen_ms = latency_ms
        if self.latency_max_seen_ms is None or latency_ms > self.latency_max_seen_ms:
            self.latency_max_seen_ms = latency_ms

    def progress_dict(self) -> dict[str, object]:
        samples = len(self.latencies_ms)
        mean_ms = (self.latency_sum_ms / samples) if samples else None
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
            "button_phase_observations": self.button_phase_observations,
            "emulated_button_observations": self.emulated_button_observations,
            "emulated_button_phase_observations": self.emulated_button_phase_observations,
            "host_rtt_samples": samples,
            "host_rtt_min_ms": self.latency_min_seen_ms,
            "host_rtt_mean_ms": mean_ms,
            "host_rtt_max_ms": self.latency_max_seen_ms,
            "latency_samples": samples,
            "latency_min_ms": self.latency_min_seen_ms,
            "latency_mean_ms": mean_ms,
            "latency_max_ms": self.latency_max_seen_ms,
            "elapsed_s": self.elapsed_s,
            "verdict": self.verdict,
            "notes": self.notes,
        }

    def as_dict(self) -> dict[str, object]:
        latencies = sorted(self.latencies_ms)
        mean_ms = (self.latency_sum_ms / len(latencies)) if latencies else None
        p99_ms = percentile(latencies, 99) if latencies else None
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
            "button_phase_observations": self.button_phase_observations,
            "emulated_button_observations": self.emulated_button_observations,
            "emulated_button_phase_observations": self.emulated_button_phase_observations,
            "host_rtt_min_ms": self.latency_min_seen_ms,
            "host_rtt_mean_ms": mean_ms,
            "host_rtt_p99_ms": p99_ms,
            "host_rtt_max_ms": self.latency_max_seen_ms,
            "latency_min_ms": self.latency_min_seen_ms,
            "latency_mean_ms": mean_ms,
            "latency_p99_ms": p99_ms,
            "latency_max_ms": self.latency_max_seen_ms,
            "elapsed_s": self.elapsed_s,
            "verdict": self.verdict,
            "notes": self.notes,
            "scenario": self.scenario,
            "door": self.door,
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


def cycles_for_duration_hours(duration_hours: float, speed_factor: float) -> int:
    if duration_hours <= 0:
        raise ValueError("duration must be positive")
    if speed_factor <= 0:
        raise ValueError("speed factor must be positive")
    cycle_s = STEADY_CYCLE_US / 1_000_000.0
    return max(1, math.ceil((duration_hours * 3600.0 / cycle_s) * speed_factor))


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
        trace_path: Path | None = None,
        abort_on_first_miss: bool = False,
        progress_path: Path | None = None,
        progress_interval_s: float = 60.0,
        progress_fsync: bool = True,
        scenario: str = "steady",
        door_model: DoorModel | None = None,
        command_sender: Callable[[str], str] | None = None,
        fault_every_cycles: int = 0,
        fault_cycles: set[int] | None = None,
        goto_position: int = 80,
        strict_scenario_verdict: bool = True,
        emulate_commands: bool = False,
    ) -> None:
        self.transport = transport
        self.slave_id = slave_id
        self.speed_factor = speed_factor
        self.missed_poll_threshold = missed_poll_threshold
        self.response_timeout_s = response_timeout_s
        self.abort_on_first_miss = abort_on_first_miss
        self.progress_interval_s = progress_interval_s
        self.progress_fsync = progress_fsync
        self.rng = random.Random(rng_seed)
        self.expected_buttons = expected_buttons or set()
        self.rx_buffer = bytearray()
        self.report = SimulationReport()
        self.scenario = scenario
        self.report.scenario = scenario
        self.door = door_model or DoorModel()
        self.command_sender = command_sender or self._transport_command
        self.fault_every_cycles = fault_every_cycles
        self.fault_cycles = fault_cycles or set()
        self.goto_position = max(0, min(POSITION_OPEN, goto_position))
        self.strict_scenario_verdict = strict_scenario_verdict
        self.emulate_commands = emulate_commands
        self.scenario_actions = self.build_scenario_actions(scenario)
        self.seen_button_presses: set[str] = set()
        self.active_button_presses: set[str] = set()
        self.trace_start_s = time.monotonic()
        self.trace_file: TextIO | None = None
        if trace_path is not None:
            trace_path.parent.mkdir(parents=True, exist_ok=True)
            self.trace_file = trace_path.open("w", encoding="utf-8")
        self.progress_file: TextIO | None = None
        self.last_progress_s: float | None = None
        if progress_path is not None:
            progress_path.parent.mkdir(parents=True, exist_ok=True)
            self.progress_file = progress_path.open("w", encoding="utf-8")

    def _transport_command(self, button: str) -> str:
        return self.transport.command(f"press {button}")

    def build_scenario_actions(self, scenario: str) -> dict[int, list[tuple[str, str]]]:
        if scenario not in SCENARIOS:
            raise ValueError(f"unknown scenario: {scenario}")
        if scenario == "steady":
            return {}
        if scenario == "open-from-closed":
            return {2: [("button", "open")]}
        if scenario == "close-from-open":
            self.door = DoorModel.open(**self.door.as_dict()["parameters"])  # type: ignore[arg-type]
            return {2: [("button", "close")]}
        if scenario == "stop-mid-opening":
            return {2: [("button", "open")], 12: [("button", "stop")]}
        if scenario == "reverse-open-to-close":
            return {2: [("button", "open")], 12: [("button", "close")]}
        if scenario == "reverse-close-to-open":
            self.door = DoorModel.open(**self.door.as_dict()["parameters"])  # type: ignore[arg-type]
            return {2: [("button", "close")], 12: [("button", "open")]}
        if scenario == "half-position":
            return {2: [("button", "half")]}
        if scenario == "vent-position":
            return {2: [("button", "vent")]}
        if scenario == "light-toggle":
            return {2: [("button", "light")]}
        if scenario == "closing-obstruction":
            self.door = DoorModel.open(**self.door.as_dict()["parameters"])  # type: ignore[arg-type]
            if self.door.obstruction_cycle is None:
                self.door.obstruction_cycle = 16
            return {2: [("button", "close")]}
        if scenario == "goto-position":
            initial = DoorModel.closed(**self.door.as_dict()["parameters"])  # type: ignore[arg-type]
            self.door = initial
            return {2: [("button", "open")]}
        return {}

    def close(self) -> None:
        if self.trace_file is not None:
            self.trace_file.close()
            self.trace_file = None
        if self.progress_file is not None:
            self.progress_file.close()
            self.progress_file = None

    def trace(self, event: str, **fields: object) -> None:
        if self.trace_file is None:
            return
        record = {
            "t_s": time.monotonic() - self.trace_start_s,
            "event": event,
            **fields,
        }
        self.trace_file.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
        self.trace_file.flush()

    def progress(
        self,
        event: str,
        *,
        cycle: int | None = None,
        cycles: int | None = None,
        duration_s: float | None = None,
        force: bool = False,
    ) -> None:
        if self.progress_file is None:
            return
        now = time.monotonic()
        if (
            not force
            and self.progress_interval_s > 0
            and self.last_progress_s is not None
            and now - self.last_progress_s < self.progress_interval_s
        ):
            return
        record = {
            "t_s": now - self.trace_start_s,
            "event": event,
            "cycle": cycle,
            "cycles": cycles,
            "duration_s": duration_s,
            **self.report.progress_dict(),
        }
        self.progress_file.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
        self.progress_file.flush()
        if self.progress_fsync:
            os.fsync(self.progress_file.fileno())
        self.last_progress_s = now

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
                    self.trace("ignored_response", context=context, frame=frame.hex())
                    if self.report.ignored_responses <= 8:
                        self.report.notes.append(f"{context} ignored unexpected response {frame.hex()}")
                    continue
                del self.rx_buffer[0]
        return None

    def run(
        self,
        cycles: int,
        faults: set[str] | None = None,
        command: str | None = None,
        *,
        duration_s: float | None = None,
    ) -> SimulationReport:
        faults = faults or set()
        legacy_light_command = command == "light" and self.scenario == "steady"
        if command in COMMAND_TO_SCENARIO and self.scenario == "steady":
            self.scenario = COMMAND_TO_SCENARIO[command]
            self.report.scenario = self.scenario
            self.strict_scenario_verdict = False
            self.scenario_actions = self.build_scenario_actions(self.scenario)
        deadline_s = time.monotonic() + duration_s if duration_s is not None else None
        self.progress("start", cycle=0, cycles=cycles, duration_s=duration_s, force=True)
        self.run_bus_scan()
        self.progress("bus_scan", cycle=0, cycles=cycles, duration_s=duration_s, force=True)
        if not self.report.scan_ok:
            self.report.verdict = "scan-failed"
            self.report.elapsed_s = time.monotonic() - self.trace_start_s
            self.progress("final", cycle=0, cycles=cycles, duration_s=duration_s, force=True)
            return self.report

        cycle = 0
        while True:
            if self.report.verdict != "ok":
                break
            if deadline_s is None:
                if cycle >= cycles:
                    break
            elif time.monotonic() >= deadline_s:
                break
            if self.report.consecutive_misses >= self.missed_poll_threshold:
                self.report.verdict = "error-04"
                break
            self.run_cycle(cycle, faults=faults, legacy_light_command=legacy_light_command)
            cycle += 1
            self.progress("progress", cycle=cycle, cycles=cycles, duration_s=duration_s)

        missing_buttons = sorted(button for button in self.expected_buttons if button not in self.report.button_observations)
        if self.report.verdict == "ok" and missing_buttons:
            self.report.verdict = "button-missing"
            self.report.notes.append(f"missing button observations: {','.join(missing_buttons)}")
        elif self.report.fault_unexpected_responses:
            self.report.verdict = "fault-failed"
        elif self.report.consecutive_misses >= self.missed_poll_threshold:
            self.report.verdict = "error-04"
        elif self.report.verdict == "ok" and self.strict_scenario_verdict:
            self.apply_scenario_verdict()
        self.report.door = self.door.as_dict()
        if self.report.verdict == "ok":
            self.report.notes.append(f"scenario={self.scenario}")
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
            if self.emulate_commands:
                self.report.notes.append(
                    "ESPHome commands were emulated by the simulator; decoded HCP2 button observations still require a real command path"
                )
        self.report.elapsed_s = time.monotonic() - self.trace_start_s
        self.progress("final", cycle=self.report.polls_sent, cycles=cycles, duration_s=duration_s, force=True)
        return self.report

    def apply_scenario_verdict(self) -> None:
        if self.scenario == "open-from-closed" and self.door.state != 0x20:
            self.report.verdict = "door-scenario-failed"
            self.report.notes.append(f"door did not reach open: {self.door.as_dict()}")
        elif self.scenario == "close-from-open" and self.door.state != 0x40:
            self.report.verdict = "door-scenario-failed"
            self.report.notes.append(f"door did not reach closed: {self.door.as_dict()}")
        elif self.scenario == "stop-mid-opening" and self.door.state != 0x00:
            self.report.verdict = "door-scenario-failed"
            self.report.notes.append(f"door did not stop mid-opening: {self.door.as_dict()}")
        elif self.scenario == "half-position" and self.door.state != 0x80:
            self.report.verdict = "door-scenario-failed"
            self.report.notes.append(f"door did not reach half position: {self.door.as_dict()}")
        elif self.scenario == "vent-position" and self.door.state != 0x0A:
            self.report.verdict = "door-scenario-failed"
            self.report.notes.append(f"door did not reach vent position: {self.door.as_dict()}")
        elif self.scenario == "closing-obstruction" and self.door.incomplete_cycles < 1:
            self.report.verdict = "door-scenario-failed"
            self.report.notes.append("obstruction scenario did not inject an obstruction")
        elif self.scenario == "light-toggle" and self.door.light_word != 0x00:
            self.report.verdict = "door-scenario-failed"
            self.report.notes.append(f"light toggle did not change broadcast light word: {self.door.as_dict()}")
        elif self.scenario == "goto-position":
            tolerance = max(2, self.door.overshoot_raw_ticks + 1)
            if abs(self.door.position - self.goto_position) > tolerance or self.door.state != 0x00:
                self.report.verdict = "door-scenario-failed"
                self.report.notes.append(
                    f"goto target {self.goto_position} missed with tolerance {tolerance}: {self.door.as_dict()}"
                )

    def run_bus_scan(self) -> None:
        start = time.monotonic()
        request = protocol.bus_scan_request(self.slave_id)
        self.trace("bus_scan_tx", request=request.hex())
        self.write_frame(request)
        frame = self.read_response(
            match=lambda response: response == protocol.SCAN_RESPONSE,
            context="bus scan",
        )
        if frame == protocol.SCAN_RESPONSE:
            self.report.scan_ok = True
            latency_ms = (time.monotonic() - start) * 1000.0
            self.report.record_latency(latency_ms)
            self.trace("bus_scan_rx", response=frame.hex(), host_rtt_ms=latency_ms, latency_ms=latency_ms)
        else:
            self.report.notes.append(f"bus scan failed: {frame.hex() if frame else 'timeout'}")
            self.trace("bus_scan_miss", response=frame.hex() if frame else None)

    def run_cycle(self, cycle: int, *, faults: set[str], legacy_light_command: bool = False) -> None:
        state = self.broadcast_for_cycle(cycle)
        self.trace(
            "broadcast_tx",
            target=state.target,
            current=state.current,
            state=state.state,
            light=state.light,
            scenario=self.scenario,
        )
        self.write_frame(protocol.broadcast_status(state))
        self.scaled_sleep(BROADCAST_TO_POLL_GAP_US, jitter="jitter" in faults)

        counter = cycle & 0xFF
        if self.should_inject_faults(cycle):
            self.inject_faults(faults, counter)

        for kind, value in self.scenario_actions.get(cycle, []):
            if kind == "button":
                self.issue_button(value)
                if self.report.verdict != "ok":
                    return

        if self.scenario == "goto-position" and cycle > 2 and "stop" not in self.seen_button_presses:
            position_step = max(1, round(POSITION_OPEN / self.door.travel_cycles))
            stop_lead = max(position_step, self.door.overshoot_raw_ticks)
            if state.current + stop_lead >= self.goto_position:
                self.issue_button("stop")
                if self.report.verdict != "ok":
                    return

        if legacy_light_command and cycle == 2:
            self.run_light_command(0x02, enabled=True)

        self.send_status_poll(counter, split="split" in faults)
        if self.report.verdict != "ok":
            return
        self.door.tick()
        self.scaled_sleep(POLL_TO_NEXT_BROADCAST_GAP_US, jitter="jitter" in faults)

    def should_inject_faults(self, cycle: int) -> bool:
        if cycle == 1:
            return True
        if cycle in self.fault_cycles:
            return True
        return self.fault_every_cycles > 0 and cycle > 0 and cycle % self.fault_every_cycles == 0

    def issue_button(self, button: str) -> None:
        deadline = time.monotonic() + 0.3
        while True:
            try:
                ack = self.command_sender(button)
            except Exception as exc:
                self.report.verdict = "command-send-failed"
                self.report.notes.append(f"press {button} failed: {type(exc).__name__}: {exc}")
                return
            if ack != "ERR busy" or time.monotonic() >= deadline:
                break
            time.sleep(0.01)
        if not ack.startswith("OK"):
            self.report.verdict = "command-send-failed"
            self.report.notes.append(f"press {button} rejected: {ack}")
            return
        self.trace("command_request", button=button, ack=ack)
        if self.emulate_commands:
            self.apply_emulated_button(button)

    def apply_emulated_button(self, button: str) -> None:
        self.report.emulated_button_observations[button] = (
            self.report.emulated_button_observations.get(button, 0) + 1
        )
        key = f"{button}:press"
        self.report.emulated_button_phase_observations[key] = (
            self.report.emulated_button_phase_observations.get(key, 0) + 1
        )
        self.seen_button_presses.add(button)
        self.door.command(button)
        self.trace("emulated_esphome_command", button=button, phase="press")

    def broadcast_for_cycle(self, cycle: int) -> BroadcastState:
        return self.door.broadcast_state()

    def send_status_poll(self, counter: int, *, split: bool = False) -> None:
        frame = protocol.status_poll(counter, self.slave_id)
        self.report.polls_sent += 1
        start = time.monotonic()
        self.trace("poll_tx", counter=counter, request=frame.hex(), poll_index=self.report.polls_sent)
        self.write_frame(frame, split=split)
        response = self.read_response(
            match=lambda reply: protocol.decode_response_kind(reply) == "status"
            and protocol.response_counter(reply) == counter,
            context=f"status poll {counter:02x}",
        )
        if response is not None:
            decoded_button = protocol.decode_status_button_phase(response)
            button = decoded_button[0] if decoded_button is not None else None
            button_phase = decoded_button[1] if decoded_button is not None else None
            if button is not None:
                self.report.button_observations[button] = self.report.button_observations.get(button, 0) + 1
            if button is not None and button_phase is not None:
                key = f"{button}:{button_phase}"
                self.report.button_phase_observations[key] = self.report.button_phase_observations.get(key, 0) + 1
                if button_phase == "press":
                    if button not in self.active_button_presses:
                        self.active_button_presses.add(button)
                        self.seen_button_presses.add(button)
                        self.door.command(button)
                elif button_phase == "release":
                    self.active_button_presses.discard(button)
            elif button is None:
                self.active_button_presses.clear()
            latency_ms = (time.monotonic() - start) * 1000.0
            self.report.record_latency(latency_ms)
            self.report.replies += 1
            self.report.consecutive_misses = 0
            self.trace(
                "poll_rx",
                counter=counter,
                response=response.hex(),
                host_rtt_ms=latency_ms,
                latency_ms=latency_ms,
                poll_index=self.report.polls_sent,
                button=button,
                button_phase=button_phase,
            )
        else:
            self.report.misses += 1
            self.report.consecutive_misses += 1
            self.report.max_consecutive_misses = max(
                self.report.max_consecutive_misses, self.report.consecutive_misses
            )
            self.trace(
                "poll_miss",
                counter=counter,
                poll_index=self.report.polls_sent,
                consecutive_misses=self.report.consecutive_misses,
            )
            if self.abort_on_first_miss and self.report.verdict == "ok":
                self.report.verdict = "missed-poll"
                self.report.notes.append(
                    f"aborted after first missed status poll at poll_index={self.report.polls_sent}"
                )

    def run_light_command(self, counter: int, *, enabled: bool) -> None:
        request = protocol.light_command(counter, enabled, self.slave_id)
        self.trace("command_tx", counter=counter, request=request.hex(), command="light", enabled=enabled)
        self.write_frame(request)
        expected = protocol.COMMAND_RESPONSE_BY_COUNTER.get(counter)
        response = self.read_response(
            match=lambda reply: expected is not None and reply == expected,
            context=f"light command {counter:02x}",
        )
        if response == expected:
            self.report.command_replies += 1
            self.trace("command_rx", counter=counter, response=response.hex(), command="light")
        else:
            self.report.notes.append(f"light command failed: {response.hex() if response else 'timeout'}")
            self.trace("command_miss", counter=counter, response=response.hex() if response else None, command="light")

    def inject_faults(self, faults: set[str], counter: int) -> None:
        valid = protocol.status_poll(counter, self.slave_id)
        if "garbage" in faults:
            self.transport.write(b"\x55\x00\xff\x10")
        if "corrupt-crc" in faults:
            corrupt = bytearray(valid)
            corrupt[-1] ^= 0x55
            self.expect_no_response(bytes(corrupt), "corrupt-crc")
        if "wrong-slave" in faults:
            wrong_slave = protocol.status_poll(counter, (self.slave_id % 247) + 1)
            self.expect_no_response(wrong_slave, "wrong-slave")
        if "wrong-register" in faults:
            wrong_register = bytearray(valid[:-2])
            wrong_register[3] ^= 0x01
            self.expect_no_response(protocol.append_crc(bytes(wrong_register)), "wrong-register")
        if "bad-byte-count" in faults:
            bad_byte_count = bytearray(valid[:-2])
            bad_byte_count[10] = 0x06
            self.expect_no_response(protocol.append_crc(bytes(bad_byte_count)), "bad-byte-count")
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
