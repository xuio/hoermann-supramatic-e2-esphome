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
        "abort_on_miss": False,
        "fault": [],
        "command": None,
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
