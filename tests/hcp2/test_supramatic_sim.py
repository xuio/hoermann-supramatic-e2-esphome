from __future__ import annotations

import json
import os
import pty
import select
import subprocess
import threading
import time
import tty
from argparse import Namespace
from pathlib import Path

from tools.supramatic_sim.__main__ import run_once, selftest
from tools.supramatic_sim import protocol
from tools.supramatic_sim.door_model import DEFAULT_HALF_POSITION, DEFAULT_VENT_POSITION
from tools.supramatic_sim.simulator import DEFAULT_MISSED_POLL_THRESHOLD, SupraMaticSimulator
from tools.supramatic_sim.transport import HOST_RESPONDER, build_host_responder


def scenario(**kwargs: object) -> Namespace:
    defaults = {
        "pty": True,
        "socketpair": False,
        "serial": None,
        "cycles": 100,
        "duration_hours": None,
        "speed_factor": 50.0,
        "dut_response_delay_us": 4200,
        "missed_poll_threshold": DEFAULT_MISSED_POLL_THRESHOLD,
        "report": None,
        "trace": None,
        "progress": None,
        "progress_interval_s": 60.0,
        "no_progress_fsync": False,
        "abort_on_miss": False,
        "fault": [],
        "fault_every_cycles": 0,
        "fault_cycle": [],
        "command": None,
        "scenario": "steady",
        "door_travel_cycles": 260,
        "reverse_profile": "stop_then_reverse",
        "reverse_dwell_cycles": 4,
        "stop_latency_cycles": 1,
        "overshoot_raw_ticks": 1,
        "half_position_raw": DEFAULT_HALF_POSITION,
        "vent_position_raw": DEFAULT_VENT_POSITION,
        "goto_position_raw": 80,
        "obstruction_cycle": None,
        "obstruction_no_reverse": False,
        "speculative_obstruction_flags": False,
        "emulate_esphome_commands": False,
        "expect_button": [],
    }
    defaults.update(kwargs)
    return Namespace(**defaults)


def test_simulator_selftest() -> None:
    assert selftest() == 0


def test_pty_functional_report(tmp_path: Path) -> None:
    report_path = tmp_path / "report.json"
    result = run_once(scenario(cycles=120, report=report_path))
    assert result["verdict"] == "ok"
    assert result["scan_ok"] is True
    assert result["polls_sent"] == 120
    assert result["replies"] == 120
    assert result["misses"] == 0
    assert result["latency_p99_ms"] is not None

    report = json.loads(report_path.read_text())
    assert report["verdict"] == "ok"
    assert report["polls_sent"] == 120


def test_simulator_writes_per_poll_trace(tmp_path: Path) -> None:
    trace_path = tmp_path / "trace.jsonl"
    result = run_once(scenario(cycles=3, trace=trace_path))

    assert result["verdict"] == "ok"
    events = [json.loads(line) for line in trace_path.read_text(encoding="utf-8").splitlines()]
    assert any(event["event"] == "bus_scan_tx" for event in events)
    assert [event["counter"] for event in events if event["event"] == "poll_tx"] == [0, 1, 2]
    assert [event["counter"] for event in events if event["event"] == "poll_rx"] == [0, 1, 2]


def test_simulator_writes_low_volume_progress(tmp_path: Path) -> None:
    progress_path = tmp_path / "progress.jsonl"
    result = run_once(scenario(cycles=4, progress=progress_path, progress_interval_s=0.0))

    assert result["verdict"] == "ok"
    events = [json.loads(line) for line in progress_path.read_text(encoding="utf-8").splitlines()]
    assert [event["event"] for event in events[:2]] == ["start", "bus_scan"]
    assert events[-1]["event"] == "final"
    assert events[-1]["polls_sent"] == 4
    assert events[-1]["misses"] == 0


class ScanThenSilentTransport:
    def __init__(self) -> None:
        self.pending = bytearray()

    def write(self, data: bytes) -> None:
        if data == protocol.bus_scan_request(protocol.SLAVE_ID):
            self.pending.extend(protocol.SCAN_RESPONSE)

    def read_available(self, timeout: float) -> bytes:
        if self.pending:
            data = bytes(self.pending)
            self.pending.clear()
            return data
        time.sleep(min(timeout, 0.001))
        return b""

    def command(self, line: str) -> str:
        raise RuntimeError(line)

    def close(self) -> None:
        pass


def status_response(counter: int, button: str | None = None, phase: str | None = None) -> bytes:
    payload = bytearray([protocol.SLAVE_ID, protocol.FC_READ_WRITE_MULTIPLE_REGISTERS, 0x10])
    payload.extend([counter & 0xFF, 0x00, 0x03, 0x01])
    if button is None or phase is None:
        payload.extend([0x00, 0x00, 0x00])
    else:
        payload.extend(protocol.BUTTON_STATUS_PHASES[button][phase])
    payload.extend([0x00] * 9)
    assert len(payload) == 19
    return protocol.append_crc(bytes(payload))


class ScriptedStatusTransport:
    def __init__(self, phases: list[tuple[str, str] | None]) -> None:
        self.phases = list(phases)
        self.pending = bytearray()
        self.status_polls = 0

    def write(self, data: bytes) -> None:
        if data == protocol.bus_scan_request(protocol.SLAVE_ID):
            self.pending.extend(protocol.SCAN_RESPONSE)
            return
        if len(data) >= 15 and data[0] == protocol.SLAVE_ID and data[1] == protocol.FC_READ_WRITE_MULTIPLE_REGISTERS:
            counter = data[11]
            phase = self.phases[self.status_polls] if self.status_polls < len(self.phases) else None
            self.status_polls += 1
            if phase is None:
                self.pending.extend(status_response(counter))
            else:
                self.pending.extend(status_response(counter, phase[0], phase[1]))

    def read_available(self, timeout: float) -> bytes:
        if self.pending:
            data = bytes(self.pending)
            self.pending.clear()
            return data
        time.sleep(min(timeout, 0.001))
        return b""

    def command(self, line: str) -> str:
        raise RuntimeError(line)

    def close(self) -> None:
        pass


def test_simulator_abort_on_first_miss(tmp_path: Path) -> None:
    progress_path = tmp_path / "miss-progress.jsonl"
    transport = ScanThenSilentTransport()
    simulator = SupraMaticSimulator(
        transport,
        speed_factor=5000.0,
        response_timeout_s=0.001,
        abort_on_first_miss=True,
        progress_path=progress_path,
        progress_interval_s=0.0,
    )
    try:
        report = simulator.run(5)
    finally:
        simulator.close()

    assert report.verdict == "missed-poll"
    assert report.polls_sent == 1
    assert report.replies == 0
    assert report.misses == 1
    events = [json.loads(line) for line in progress_path.read_text(encoding="utf-8").splitlines()]
    assert events[-1]["event"] == "final"
    assert events[-1]["verdict"] == "missed-poll"


def test_external_button_press_edges_can_repeat_after_release() -> None:
    transport = ScriptedStatusTransport(
        [
            ("open", "press"),
            ("open", "press"),
            ("open", "release"),
            None,
            ("open", "press"),
            ("open", "release"),
        ]
    )
    simulator = SupraMaticSimulator(transport, speed_factor=5000.0, response_timeout_s=0.001)
    try:
        report = simulator.run(6)
    finally:
        simulator.close()

    commands = [
        event
        for event in report.door["events"]
        if event["event"] == "command" and event["button"] == "open"
    ]
    assert report.verdict == "ok"
    assert report.button_phase_observations["open:press"] == 3
    assert report.button_phase_observations["open:release"] == 2
    assert len(commands) == 2


def test_external_button_held_frames_apply_once_until_idle_or_release() -> None:
    transport = ScriptedStatusTransport(
        [
            ("close", "press"),
            ("close", "press"),
            ("close", "press"),
            ("close", "release"),
            ("close", "press"),
        ]
    )
    simulator = SupraMaticSimulator(transport, speed_factor=5000.0, response_timeout_s=0.001)
    try:
        report = simulator.run(5)
    finally:
        simulator.close()

    commands = [
        event
        for event in report.door["events"]
        if event["event"] == "command" and event["button"] == "close"
    ]
    assert report.verdict == "ok"
    assert len(commands) == 2


def test_socketpair_fault_recovery_and_open_command() -> None:
    result = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=100,
            fault=["corrupt-crc", "truncated", "duplicate", "jitter", "garbage", "split"],
            command="open",
            expect_button=["open"],
        )
    )
    assert result["verdict"] == "ok"
    assert result["fault_checks"] == 2
    assert result["fault_recoveries"] == 2
    assert result["fault_unexpected_responses"] == 0
    assert result["misses"] == 0
    assert result["button_observations"].get("open", 0) > 0


def test_socketpair_light_command() -> None:
    result = run_once(scenario(pty=False, socketpair=True, cycles=40, command="light"))
    assert result["verdict"] == "ok"
    assert result["command_replies"] == 1


def test_door_scenario_open_and_close() -> None:
    opened = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=24,
            scenario="open-from-closed",
            door_travel_cycles=8,
            expect_button=["open"],
        )
    )
    assert opened["verdict"] == "ok"
    assert opened["door"]["state_name"] == "open"
    assert opened["door"]["position"] == 200

    closed = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=24,
            scenario="close-from-open",
            door_travel_cycles=8,
            expect_button=["close"],
        )
    )
    assert closed["verdict"] == "ok"
    assert closed["door"]["state_name"] == "closed"
    assert closed["door"]["position"] == 0


def test_emulated_esphome_command_mode_moves_virtual_door_without_decoded_button() -> None:
    result = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=24,
            scenario="open-from-closed",
            door_travel_cycles=8,
            emulate_esphome_commands=True,
        )
    )

    assert result["verdict"] == "ok"
    assert result["door"]["state_name"] == "open"
    assert result["door"]["position"] == 200
    assert result["button_observations"] == {}
    assert result["emulated_button_observations"] == {"open": 1}
    assert result["emulated_button_phase_observations"] == {"open:press": 1}
    assert any("ESPHome commands were emulated" in note for note in result["notes"])


def test_door_scenario_stop_and_reverse_profiles() -> None:
    stopped = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=24,
            scenario="stop-mid-opening",
            door_travel_cycles=16,
            expect_button=["open", "stop"],
        )
    )
    assert stopped["verdict"] == "ok"
    assert stopped["door"]["state_name"] == "stopped"
    assert 0 < stopped["door"]["position"] < 200

    reversed_run = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=50,
            scenario="reverse-open-to-close",
            door_travel_cycles=32,
            reverse_dwell_cycles=2,
            expect_button=["open", "close"],
        )
    )
    assert reversed_run["verdict"] == "ok"
    assert reversed_run["door"]["state_name"] == "closed"
    assert any(event["event"] == "stopped" and event["reason"] == "reverse-dwell" for event in reversed_run["door"]["events"])

    immediate = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=24,
            scenario="reverse-close-to-open",
            door_travel_cycles=8,
            reverse_profile="immediate_reverse",
            expect_button=["close", "open"],
        )
    )
    assert immediate["verdict"] == "ok"
    assert immediate["door"]["state_name"] == "open"


def test_door_scenario_partial_vent_light_obstruction_and_goto() -> None:
    half = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=24,
            scenario="half-position",
            door_travel_cycles=8,
            half_position_raw=24,
            expect_button=["half"],
        )
    )
    assert half["verdict"] == "ok"
    assert half["door"]["state_name"] == "half"
    assert half["door"]["position"] == 24

    vent = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=24,
            scenario="vent-position",
            door_travel_cycles=8,
            vent_position_raw=8,
            expect_button=["vent"],
        )
    )
    assert vent["verdict"] == "ok"
    assert vent["door"]["state_name"] == "vent"
    assert vent["door"]["position"] == 8

    light = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=8,
            scenario="light-toggle",
            expect_button=["light"],
        )
    )
    assert light["verdict"] == "ok"
    assert light["door"]["light_word"] == 0

    obstruction = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=28,
            scenario="closing-obstruction",
            door_travel_cycles=12,
            obstruction_cycle=4,
            expect_button=["close"],
        )
    )
    assert obstruction["verdict"] == "ok"
    assert obstruction["door"]["incomplete_cycles"] == 1
    assert any(event["event"] == "obstruction" for event in obstruction["door"]["events"])

    goto = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=24,
            scenario="goto-position",
            door_travel_cycles=10,
            goto_position_raw=80,
            expect_button=["open", "stop"],
        )
    )
    assert goto["verdict"] == "ok"
    assert goto["door"]["state_name"] == "stopped"
    assert abs(goto["door"]["position"] - 80) <= 2


def test_repeated_fault_injection_uses_explicit_schedule() -> None:
    result = run_once(
        scenario(
            pty=False,
            socketpair=True,
            cycles=12,
            fault=["corrupt-crc", "wrong-slave", "wrong-register", "bad-byte-count"],
            fault_every_cycles=4,
            fault_cycle=[6],
        )
    )
    assert result["verdict"] == "ok"
    # cycle 1, cycle 4, explicit cycle 6, and cycle 8 each run four no-response checks.
    assert result["fault_checks"] == 16
    assert result["fault_recoveries"] == 16
    assert result["fault_unexpected_responses"] == 0


class PseudoSerialPair:
    def __init__(self) -> None:
        self.left_master, self.left_slave = pty.openpty()
        self.right_master, self.right_slave = pty.openpty()
        self.left_path = os.ttyname(self.left_slave)
        self.right_path = os.ttyname(self.right_slave)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._bridge, daemon=True)

        for fd in (self.left_master, self.left_slave, self.right_master, self.right_slave):
            tty.setraw(fd)
        os.set_blocking(self.left_master, False)
        os.set_blocking(self.right_master, False)

    def start(self) -> None:
        self.thread.start()

    def _bridge(self) -> None:
        peers = {
            self.left_master: self.right_master,
            self.right_master: self.left_master,
        }
        while not self.stop.is_set():
            try:
                readable, _, _ = select.select(list(peers), [], [], 0.05)
            except OSError:
                return
            for src in readable:
                try:
                    data = os.read(src, 4096)
                except (BlockingIOError, OSError):
                    continue
                if not data:
                    continue
                dst = peers[src]
                offset = 0
                while offset < len(data) and not self.stop.is_set():
                    try:
                        offset += os.write(dst, data[offset:])
                    except BlockingIOError:
                        select.select([], [dst], [], 0.05)
                    except OSError:
                        return

    def close(self) -> None:
        self.stop.set()
        for fd in (self.left_master, self.left_slave, self.right_master, self.right_slave):
            try:
                os.close(fd)
            except OSError:
                pass
        self.thread.join(timeout=1.0)


def test_serial_transport_with_pseudo_tty_pair() -> None:
    build_host_responder()
    pair = PseudoSerialPair()
    proc = subprocess.Popen(
        [
            str(HOST_RESPONDER),
            "--device",
            pair.right_path,
            "--response-delay-us",
            "0",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert proc.stdout is not None
        pair.start()
        ready = proc.stdout.readline().decode("ascii", errors="replace").strip()
        assert ready.startswith("READY")

        result = run_once(
            scenario(
                pty=False,
                socketpair=False,
                serial=pair.left_path,
                cycles=80,
                speed_factor=5000.0,
                fault=["corrupt-crc", "truncated", "duplicate", "jitter", "garbage", "split"],
            )
        )
        assert result["verdict"] == "ok"
        assert result["polls_sent"] == 81
        assert result["replies"] == 81
        assert result["misses"] == 0
        assert result["fault_unexpected_responses"] == 0
    finally:
        if proc.stdin is not None and proc.poll() is None:
            proc.stdin.write(b"quit\n")
            proc.stdin.flush()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
        pair.close()
