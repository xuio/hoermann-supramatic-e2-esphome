"""Run HCP2 HIL simulator scenarios while external load generators are active."""

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
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from tools.hcp2_hil_la import load_samples, parse_channel_map, verify_samples
from tools.supramatic_sim.__main__ import FAULTS
from tools.supramatic_sim.door_model import (
    DEFAULT_HALF_POSITION,
    DEFAULT_VENT_POSITION,
    REVERSAL_PROFILES,
    DoorModel,
)
from tools.supramatic_sim.simulator import (
    DEFAULT_DOOR_TRAVEL_CYCLES,
    DEFAULT_MISSED_POLL_THRESHOLD,
    DEFAULT_SPEED_FACTOR,
    SCENARIOS,
    SupraMaticSimulator,
    cycles_for_duration_hours,
)
from tools.supramatic_sim.transport import SerialTransport

LP_HEALTH_FLAG_RX_STARVATION = 0x0002
DEVICE_HEALTH_TIMEOUT_S = 3.0
DEVICE_HEALTH_STALE_POLL_MS = 1000


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
    parser.add_argument("--esp-http-port", type=int, default=80, help="ESP HTTP debug port for /health classification")
    parser.add_argument(
        "--health-check",
        choices=["auto", "never", "require"],
        default="auto",
        help=(
            "Fetch /health when --esp-host is set. auto records unavailable health without failing; "
            "require fails the run when health is unavailable or continuity-classified as failed."
        ),
    )
    parser.add_argument("--esp-api-key", help="ESPHome native API encryption key; omit for plaintext API")
    parser.add_argument("--secrets-file", type=Path, default=Path("secrets.yaml"))
    parser.add_argument("--api-key-secret", default="api_key_supramatic_4_dev")
    parser.add_argument("--esp-expected-name", help="ESPHome expected node name for native API")
    parser.add_argument("--esp-cover-object-id", help="Cover object_id used for open/close/stop")
    parser.add_argument(
        "--esp-button-object-id",
        action="append",
        default=[],
        metavar="BUTTON=OBJECT_ID",
        help="Button object_id for half/vent/light or explicit open/close/stop injection; may be repeated",
    )
    parser.add_argument(
        "--command-mode",
        choices=["auto", "native-api", "emulated"],
        default="auto",
        help=(
            "How scenario button actions are issued. auto uses ESPHome native API when object IDs are configured, "
            "otherwise emulates accepted ESPHome commands inside the simulator door model."
        ),
    )
    parser.add_argument("--fault", action="append", choices=sorted(FAULTS), default=[])
    parser.add_argument(
        "--fault-every-cycles",
        type=int,
        default=0,
        help="Repeat selected fault injections every N cycles; 0 keeps the legacy single injection near startup",
    )
    parser.add_argument(
        "--fault-cycle",
        action="append",
        type=int,
        default=[],
        help="Inject selected faults at an explicit cycle; may be repeated",
    )
    parser.add_argument("--command", choices=["open", "close", "light"])
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), default="steady")
    parser.add_argument("--door-travel-cycles", type=int, default=DEFAULT_DOOR_TRAVEL_CYCLES)
    parser.add_argument("--reverse-profile", choices=sorted(REVERSAL_PROFILES), default="stop_then_reverse")
    parser.add_argument("--reverse-dwell-cycles", type=int, default=4)
    parser.add_argument("--stop-latency-cycles", type=int, default=1)
    parser.add_argument("--overshoot-raw-ticks", type=int, default=1)
    parser.add_argument("--half-position-raw", type=int, default=DEFAULT_HALF_POSITION)
    parser.add_argument("--vent-position-raw", type=int, default=DEFAULT_VENT_POSITION)
    parser.add_argument("--goto-position-raw", type=int, default=80)
    parser.add_argument("--obstruction-cycle", type=int)
    parser.add_argument("--obstruction-no-reverse", action="store_true")
    parser.add_argument("--speculative-obstruction-flags", action="store_true")
    parser.add_argument(
        "--expect-button",
        action="append",
        choices=["open", "close", "stop", "vent", "half", "light"],
        default=[],
    )
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


def parse_button_object_ids(values: list[str]) -> dict[str, str]:
    mapping: dict[str, str] = {}
    valid = {"open", "close", "stop", "half", "vent", "light"}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--esp-button-object-id must be BUTTON=OBJECT_ID, got {value!r}")
        button, object_id = value.split("=", 1)
        button = button.strip().lower()
        object_id = object_id.strip()
        if button not in valid:
            raise ValueError(f"unsupported ESPHome button mapping {button!r}; expected one of {sorted(valid)}")
        if not object_id:
            raise ValueError(f"empty ESPHome object_id for {button!r}")
        mapping[button] = object_id
    return mapping


def load_api_key(*, api_key: str | None, secrets_file: Path, secret_name: str) -> str | None:
    if api_key:
        return api_key
    if not secrets_file.exists():
        return None
    text = secrets_file.read_text(encoding="utf-8")
    match = re.search(rf"^{re.escape(secret_name)}:\s*[\"']?([^\"'\n#]+)", text, re.MULTILINE)
    return match.group(1).strip() if match else None


def scenario_uses_esphome_commands(args: argparse.Namespace) -> bool:
    return args.scenario != "steady" or args.command in {"open", "close"}


def resolve_command_mode(args: argparse.Namespace, button_object_ids: dict[str, str]) -> str:
    has_native_api_config = bool(args.esp_cover_object_id or button_object_ids)
    if args.command_mode == "native-api":
        return "native-api"
    if args.command_mode == "emulated":
        return "emulated"
    if has_native_api_config:
        return "native-api"
    if scenario_uses_esphome_commands(args):
        return "emulated"
    return "none"


class EmulatedEspHomeCommandSender:
    """Records commands as if ESPHome accepted them; simulator applies the door-model effect."""

    def __init__(self) -> None:
        self.commands: list[dict[str, Any]] = []
        self.errors: list[str] = []

    def __call__(self, button: str) -> str:
        self.commands.append({"button": button, "ok": True, "mode": "emulated"})
        return f"OK emulated-esphome {button}"

    def stop(self) -> None:
        return

    def report(self) -> dict[str, Any]:
        return {
            "mode": "emulated",
            "commands": list(self.commands),
            "errors": list(self.errors),
            "coverage": "virtual-door-model-only",
        }


class NativeApiCommandSender:
    """Schedules ESPHome native API commands without blocking the HIL poll loop."""

    def __init__(
        self,
        *,
        host: str,
        port: int,
        api_key: str | None,
        expected_name: str | None,
        cover_object_id: str | None,
        button_object_ids: dict[str, str],
        ready_timeout_s: float = 10.0,
    ) -> None:
        self.host = host
        self.port = port
        self.api_key = api_key
        self.expected_name = expected_name
        self.cover_object_id = cover_object_id
        self.button_object_ids = button_object_ids
        self.ready_timeout_s = ready_timeout_s
        self.loop: asyncio.AbstractEventLoop | None = None
        self.client: Any = None
        self.cover_key: int | None = None
        self.cover_device_id = 0
        self.button_keys: dict[str, tuple[int, int]] = {}
        self.entity_object_ids: dict[str, list[str]] = {}
        self.errors: list[str] = []
        self.commands: list[dict[str, Any]] = []
        self.ready = threading.Event()
        self.stop_requested = threading.Event()
        self.thread = threading.Thread(target=self._run, name="hcp2-native-api-command-sender", daemon=True)

    def start(self) -> None:
        self.thread.start()
        if not self.ready.wait(self.ready_timeout_s):
            raise RuntimeError("ESPHome native API command sender did not become ready")
        if self.errors:
            raise RuntimeError(self.errors[-1])

    def stop(self) -> None:
        self.stop_requested.set()
        if self.loop is not None:
            self.loop.call_soon_threadsafe(lambda: None)
        self.thread.join(timeout=5.0)

    def _run(self) -> None:
        try:
            asyncio.run(self._run_async())
        except Exception as exc:  # noqa: BLE001
            self.errors.append(f"worker failed: {type(exc).__name__}: {exc}")
            self.ready.set()

    async def _run_async(self) -> None:
        try:
            from aioesphomeapi import APIClient  # type: ignore
        except ImportError as exc:
            raise RuntimeError("aioesphomeapi is required; run this tool through uv") from exc

        self.loop = asyncio.get_running_loop()
        self.client = APIClient(
            self.host,
            self.port,
            None,
            noise_psk=self.api_key,
            expected_name=self.expected_name,
            client_info="garage-hcp2-hil-load",
        )
        await self.client.connect(login=True)
        try:
            _info, entities, _services = await self.client.device_info_and_list_entities()
            self._index_entities(entities)
            self._validate_entities()
            self.ready.set()
            while not self.stop_requested.is_set():
                await asyncio.sleep(0.05)
        finally:
            with contextlib.suppress(Exception):
                await self.client.disconnect()

    def _index_entities(self, entities: list[Any]) -> None:
        for entity in entities:
            kind = type(entity).__name__
            object_id = str(getattr(entity, "object_id", ""))
            if object_id:
                self.entity_object_ids.setdefault(kind, []).append(object_id)
            if kind == "CoverInfo" and object_id == self.cover_object_id:
                self.cover_key = int(entity.key)
                self.cover_device_id = int(getattr(entity, "device_id", 0))
            if kind == "ButtonInfo":
                for button, wanted_object_id in self.button_object_ids.items():
                    if object_id == wanted_object_id:
                        self.button_keys[button] = (int(entity.key), int(getattr(entity, "device_id", 0)))

    def _validate_entities(self) -> None:
        if self.cover_object_id and self.cover_key is None:
            raise RuntimeError(
                f"could not find cover object_id {self.cover_object_id!r}; covers={self.entity_object_ids.get('CoverInfo', [])}"
            )
        missing_buttons = sorted(set(self.button_object_ids) - set(self.button_keys))
        if missing_buttons:
            raise RuntimeError(
                "could not find ESPHome button object_ids for "
                f"{missing_buttons}; buttons={self.entity_object_ids.get('ButtonInfo', [])}"
            )

    def __call__(self, button: str) -> str:
        if self.loop is None or self.client is None:
            return "ERR native-api-not-ready"
        future = asyncio.run_coroutine_threadsafe(self._send(button), self.loop)
        future.add_done_callback(lambda done, command=button: self._record_done(command, done))
        return f"OK api-scheduled {button}"

    async def _send(self, button: str) -> None:
        if button in self.button_keys:
            key, device_id = self.button_keys[button]
            self.client.button_command(key, device_id=device_id)
            return
        if self.cover_key is None:
            raise RuntimeError(f"no command path configured for {button!r}")
        if button == "open":
            self.client.cover_command(self.cover_key, position=1.0)
        elif button == "close":
            self.client.cover_command(self.cover_key, position=0.0)
        elif button == "stop":
            self.client.cover_command(self.cover_key, stop=True)
        else:
            raise RuntimeError(f"no command path configured for {button!r}")

    def _record_done(self, button: str, future: "asyncio.Future[None]") -> None:
        record: dict[str, Any] = {"button": button, "ok": False}
        try:
            future.result()
            record["ok"] = True
        except Exception as exc:  # noqa: BLE001
            record["error"] = f"{type(exc).__name__}: {exc}"
            self.errors.append(f"press {button}: {record['error']}")
        self.commands.append(record)

    def report(self) -> dict[str, Any]:
        return {
            "mode": "native-api",
            "host": self.host,
            "port": self.port,
            "expected_name": self.expected_name,
            "cover_object_id": self.cover_object_id,
            "button_object_ids": dict(self.button_object_ids),
            "cover_key": self.cover_key,
            "button_keys": {name: key for name, (key, _device_id) in self.button_keys.items()},
            "commands": list(self.commands),
            "errors": list(self.errors),
        }


def build_door_model(args: argparse.Namespace) -> DoorModel:
    return DoorModel(
        travel_cycles=args.door_travel_cycles,
        reversal_profile=args.reverse_profile,
        reverse_dwell_cycles=args.reverse_dwell_cycles,
        stop_latency_cycles=args.stop_latency_cycles,
        overshoot_raw_ticks=args.overshoot_raw_ticks,
        half_position=args.half_position_raw,
        vent_position=args.vent_position_raw,
        obstruction_cycle=args.obstruction_cycle,
        obstruction_reverses=not args.obstruction_no_reverse,
        speculative_obstruction_flags=args.speculative_obstruction_flags,
    )


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


def _int_value(value: Any, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _bool_value(value: Any, default: bool = True) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    text = str(value).strip().lower()
    if text in {"1", "true", "yes", "on", "ok"}:
        return True
    if text in {"0", "false", "no", "off", "fail"}:
        return False
    return default


def _nested_int(payload: dict[str, Any], name: str, default: int = 0) -> int:
    for section_name in ("checks", "lp", "stats"):
        section = payload.get(section_name)
        if isinstance(section, dict) and name in section:
            return _int_value(section.get(name), default)
    return _int_value(payload.get(name), default)


def _nested_bool(payload: dict[str, Any], name: str, default: bool = True) -> bool:
    for section_name in ("checks", "lp", "stats"):
        section = payload.get(section_name)
        if isinstance(section, dict) and name in section:
            return _bool_value(section.get(name), default)
    return _bool_value(payload.get(name), default)


def classify_device_health(payload: dict[str, Any], *, fault_injection_expected: bool) -> dict[str, Any]:
    """Classify ESP /health as a continuity verdict for HIL reports.

    The firmware endpoint owns the live continuity verdict. This host-side
    classifier still blocks on poll loss and active transport faults, but it
    treats sticky LP diagnostics as warnings once the firmware has declared the
    recent continuity window healthy again.
    """

    reasons = payload.get("reasons")
    firmware_reasons = [str(reason) for reason in reasons] if isinstance(reasons, list) else []
    firmware_ok = payload.get("verdict") == "ok"
    blocking: list[str] = []
    warnings: list[str] = []

    for name in ("lp_mode", "lp_seen", "bus_online", "valid_broadcast"):
        if not _nested_bool(payload, name, True):
            blocking.append(name)

    for name in (
        "missed_polls",
        "raw_missed_polls",
        "tx_aborts",
        "collisions",
        "loop_overruns",
        "stuck_de_recoveries",
    ):
        value = _nested_int(payload, name)
        if value != 0:
            blocking.append(f"{name}:{value}")

    last_poll_age_ms = _nested_int(payload, "last_poll_age_ms", 0)
    if last_poll_age_ms > DEVICE_HEALTH_STALE_POLL_MS:
        blocking.append(f"last_poll_age_ms:{last_poll_age_ms}")

    health_flags = _nested_int(payload, "health_flags")
    rx_starvations = _nested_int(payload, "rx_starvations")
    rx_flag_set = bool(health_flags & LP_HEALTH_FLAG_RX_STARVATION)
    non_rx_flags = health_flags & ~LP_HEALTH_FLAG_RX_STARVATION
    if non_rx_flags:
        if firmware_ok and not blocking:
            warnings.append(f"lp_health_flags_sticky:0x{non_rx_flags:04x}")
        else:
            blocking.append(f"lp_health_flags_non_rx:0x{non_rx_flags:04x}")

    has_rx_warning = rx_starvations != 0 or rx_flag_set
    if has_rx_warning:
        if firmware_ok and not blocking:
            warnings.append(f"rx_starvations_sticky:{rx_starvations}")
        elif fault_injection_expected and not blocking:
            warnings.append(f"rx_starvations_during_fault_injection:{rx_starvations}")
        else:
            blocking.append(f"rx_starvations:{rx_starvations}")

    allowed_firmware_reasons = {"lp_health_flags", "rx_starvations"} if (fault_injection_expected or firmware_ok) else set()
    for reason in firmware_reasons:
        if reason not in allowed_firmware_reasons and reason not in blocking:
            blocking.append(f"firmware:{reason}")
    if payload.get("verdict") == "fail" and not firmware_reasons and not blocking and not warnings:
        blocking.append("firmware:fail")

    verdict = "fail" if blocking else ("warn" if warnings else "ok")
    return {
        "verdict": verdict,
        "continuity_verdict": "fail" if blocking else "ok",
        "firmware_verdict": str(payload.get("verdict", "unknown")),
        "firmware_reasons": firmware_reasons,
        "fault_injection_expected": fault_injection_expected,
        "blocking_reasons": blocking,
        "warnings": warnings,
        "health_flags": health_flags,
        "rx_starvations": rx_starvations,
        "last_poll_age_ms": last_poll_age_ms,
    }


def fetch_device_health(args: argparse.Namespace) -> dict[str, Any] | None:
    if args.health_check == "never" or not args.esp_host:
        return None
    url = f"http://{args.esp_host}:{args.esp_http_port}/health"
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=DEVICE_HEALTH_TIMEOUT_S) as response:
            status = int(response.status)
            body = response.read()
    except urllib.error.HTTPError as exc:
        status = int(exc.code)
        body = exc.read()
    except (OSError, TimeoutError, urllib.error.URLError) as exc:
        return {
            "url": url,
            "verdict": "unavailable",
            "continuity_verdict": "unknown",
            "error": f"{type(exc).__name__}: {exc}",
        }

    try:
        payload = json.loads(body.decode("utf-8", errors="replace"))
    except json.JSONDecodeError as exc:
        return {
            "url": url,
            "http_status": status,
            "verdict": "unavailable",
            "continuity_verdict": "unknown",
            "error": f"JSONDecodeError: {exc}",
        }
    if not isinstance(payload, dict):
        return {
            "url": url,
            "http_status": status,
            "verdict": "unavailable",
            "continuity_verdict": "unknown",
            "error": f"unexpected JSON payload type: {type(payload).__name__}",
        }

    classification = classify_device_health(payload, fault_injection_expected=bool(args.fault))
    return {
        "url": url,
        "http_status": status,
        **classification,
        "payload": payload,
    }


def run_session(args: argparse.Namespace, *, run_index: int = 1, load_commands: list[str] | None = None) -> dict[str, Any]:
    cycles = effective_cycles(args)
    duration_s = effective_duration_s(args)
    if cycles < 1:
        raise ValueError("--cycles must be positive")
    if args.speed_factor <= 0:
        raise ValueError("--speed-factor must be positive")
    if args.progress_interval_s < 0:
        raise ValueError("--progress-interval-s must not be negative")
    if args.fault_every_cycles < 0:
        raise ValueError("--fault-every-cycles must not be negative")
    if args.door_travel_cycles < 1:
        raise ValueError("--door-travel-cycles must be positive")
    if args.reverse_dwell_cycles < 0 or args.stop_latency_cycles < 0:
        raise ValueError("door cycle delays must not be negative")
    load_commands = effective_load_commands(args) if load_commands is None else load_commands
    button_object_ids = parse_button_object_ids(args.esp_button_object_id)
    command_mode = resolve_command_mode(args, button_object_ids)
    api_key = load_api_key(api_key=args.esp_api_key, secrets_file=args.secrets_file, secret_name=args.api_key_secret)
    use_native_api_commands = command_mode == "native-api"
    use_emulated_commands = command_mode == "emulated"
    if (
        command_mode == "native-api"
        and scenario_uses_esphome_commands(args)
        and not (args.esp_cover_object_id or button_object_ids)
    ):
        raise ValueError(
            "native API command mode requires --esp-cover-object-id or --esp-button-object-id for command scenarios"
        )
    if use_native_api_commands and not args.esp_host:
        raise ValueError("native API command injection requires --esp-host")
    if use_emulated_commands and args.expect_button:
        raise ValueError(
            "--expect-button requires decoded HCP2 output; emulated command mode records emulated_button_observations instead"
        )

    report: dict[str, Any] = {
        "serial": args.serial,
        "run_index": run_index,
        "command_mode": command_mode,
        "cycles": cycles,
        "duration_hours": args.duration_hours,
        "speed_factor": args.speed_factor,
        "abort_on_miss": args.abort_on_miss,
        "preset": args.preset,
        "esp_host": args.esp_host,
        "esp_api_port": args.esp_api_port,
        "esp_api_encryption": "noise" if api_key else "plaintext",
        "esp_http_port": args.esp_http_port,
        "health_check": args.health_check,
        "esp_expected_name": args.esp_expected_name,
        "esp_cover_object_id": args.esp_cover_object_id,
        "esp_button_object_ids": button_object_ids,
        "faults": sorted(args.fault),
        "fault_every_cycles": args.fault_every_cycles,
        "fault_cycles": sorted(args.fault_cycle),
        "command": args.command,
        "scenario": args.scenario,
        "expected_buttons": sorted(args.expect_button),
        "door_model": {
            "travel_cycles": args.door_travel_cycles,
            "reverse_profile": args.reverse_profile,
            "reverse_dwell_cycles": args.reverse_dwell_cycles,
            "stop_latency_cycles": args.stop_latency_cycles,
            "overshoot_raw_ticks": args.overshoot_raw_ticks,
            "half_position_raw": args.half_position_raw,
            "vent_position_raw": args.vent_position_raw,
            "goto_position_raw": args.goto_position_raw,
            "obstruction_cycle": args.obstruction_cycle,
            "obstruction_reverses": not args.obstruction_no_reverse,
            "speculative_obstruction_flags": args.speculative_obstruction_flags,
        },
        "pre_commands": [],
        "load_commands": [],
        "post_commands": [],
        "host_tuning": {},
        "native_api_commands": None,
        "emulated_esphome_commands": None,
        "device_health": None,
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
    command_sender: NativeApiCommandSender | EmulatedEspHomeCommandSender | None = None
    try:
        for command in load_commands:
            proc, record = start_load(command)
            loads.append((proc, record))
            report["load_commands"].append(record)

        if use_native_api_commands:
            command_sender = NativeApiCommandSender(
                host=args.esp_host,
                port=args.esp_api_port,
                api_key=api_key,
                expected_name=args.esp_expected_name,
                cover_object_id=args.esp_cover_object_id,
                button_object_ids=button_object_ids,
            )
            command_sender.start()
            report["native_api_commands"] = command_sender.report()
        elif use_emulated_commands:
            command_sender = EmulatedEspHomeCommandSender()
            report["emulated_esphome_commands"] = command_sender.report()

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
            scenario=args.scenario,
            door_model=build_door_model(args),
            command_sender=command_sender,
            fault_every_cycles=args.fault_every_cycles,
            fault_cycles=set(args.fault_cycle),
            goto_position=args.goto_position_raw,
            emulate_commands=use_emulated_commands,
        )
        simulation = simulator.run(cycles, faults=set(args.fault), command=args.command, duration_s=duration_s).as_dict()
        report["simulation"] = simulation
        report["verdict"] = str(simulation["verdict"])
        if command_sender is not None:
            if use_native_api_commands:
                report["native_api_commands"] = command_sender.report()
            elif use_emulated_commands:
                report["emulated_esphome_commands"] = command_sender.report()
            if report["verdict"] == "ok" and command_sender.errors:
                report["verdict"] = (
                    "native-api-command-failed" if use_native_api_commands else "emulated-command-failed"
                )
    finally:
        if "simulator" in locals():
            simulator.close()
        if transport is not None:
            transport.close()
        if command_sender is not None:
            command_sender.stop()
            if use_native_api_commands:
                report["native_api_commands"] = command_sender.report()
            elif use_emulated_commands:
                report["emulated_esphome_commands"] = command_sender.report()
        for proc, record in loads:
            stop_load(proc, record)

    for command in args.post_command:
        record = run_shell(command)
        report["post_commands"].append(record)
        if report["verdict"] == "ok" and record["returncode"] != 0:
            report["verdict"] = "post-command-failed"

    device_health = fetch_device_health(args)
    if device_health is not None:
        report["device_health"] = device_health
        if report["verdict"] == "ok":
            if device_health.get("continuity_verdict") == "fail":
                report["verdict"] = "device-health-failed"
            elif args.health_check == "require" and device_health.get("continuity_verdict") == "unknown":
                report["verdict"] = "device-health-unavailable"

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
        "command_mode": resolve_command_mode(args, parse_button_object_ids(args.esp_button_object_id)),
        "esp_host": args.esp_host,
        "esp_api_port": args.esp_api_port,
        "esp_http_port": args.esp_http_port,
        "health_check": args.health_check,
        "repeat": args.repeat,
        "completed_runs": len(runs),
        "cycles_per_run": effective_cycles(args),
        "duration_hours": args.duration_hours,
        "speed_factor": args.speed_factor,
        "abort_on_miss": args.abort_on_miss,
        "faults": sorted(args.fault),
        "fault_every_cycles": args.fault_every_cycles,
        "fault_cycles": sorted(args.fault_cycle),
        "command": args.command,
        "scenario": args.scenario,
        "expected_buttons": sorted(args.expect_button),
        "door_model": {
            "travel_cycles": args.door_travel_cycles,
            "reverse_profile": args.reverse_profile,
            "reverse_dwell_cycles": args.reverse_dwell_cycles,
            "stop_latency_cycles": args.stop_latency_cycles,
            "overshoot_raw_ticks": args.overshoot_raw_ticks,
            "half_position_raw": args.half_position_raw,
            "vent_position_raw": args.vent_position_raw,
            "goto_position_raw": args.goto_position_raw,
            "obstruction_cycle": args.obstruction_cycle,
            "obstruction_reverses": not args.obstruction_no_reverse,
            "speculative_obstruction_flags": args.speculative_obstruction_flags,
        },
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
