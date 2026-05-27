#!/usr/bin/env python3
"""Fullscreen phone-video sync display and HCP command coordinator."""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import os
import re
import socket
import threading
import time
import tkinter as tk
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import cv2
import numpy as np


DEFAULT_COVER_OBJECT_ID = "garage_door"

EVENT_IDLE = 0
EVENT_SCHEDULE_OPEN = 1
EVENT_SCHEDULE_CLOSE = 2
EVENT_COMMAND_OPEN = 3
EVENT_COMMAND_CLOSE = 4
EVENT_HCP_OPENING = 5
EVENT_HCP_CLOSING = 6
EVENT_HCP_OPEN = 7
EVENT_HCP_CLOSED = 8
EVENT_MANUAL_MARKER = 9
EVENT_CANCEL = 10
EVENT_STARTUP = 11
EVENT_SCHEDULE_VENT = 12
EVENT_COMMAND_VENT = 13
EVENT_HCP_VENTING = 14
EVENT_SEQUENCE_DONE = 15

QR_PAYLOAD_PREFIX = "GDS1"
QR_COMPACT_PAYLOAD_PREFIX = "G1"
QR_QUIET_ZONE_MODULES = 4
QR_SEQ_MODULUS = 36**3
QR_ELAPSED_MODULUS = 36**4
# OpenCV's QR detector is scale-sensitive for exact synthetic module sizes; stick to sizes
# covered by the decoder self-test instead of arbitrary fractional scaling.
QR_SAFE_MODULE_PIXEL_SIZES = tuple([47, 46, 45, 44, 41, 38] + list(range(37, 7, -1)))
BASE36_ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"

EVENT_NAMES = {
    EVENT_IDLE: "idle",
    EVENT_SCHEDULE_OPEN: "scheduled_open",
    EVENT_SCHEDULE_CLOSE: "scheduled_close",
    EVENT_COMMAND_OPEN: "command_open",
    EVENT_COMMAND_CLOSE: "command_close",
    EVENT_HCP_OPENING: "hcp_opening",
    EVENT_HCP_CLOSING: "hcp_closing",
    EVENT_HCP_OPEN: "hcp_open",
    EVENT_HCP_CLOSED: "hcp_closed",
    EVENT_MANUAL_MARKER: "manual_marker",
    EVENT_CANCEL: "cancel",
    EVENT_STARTUP: "startup",
    EVENT_SCHEDULE_VENT: "scheduled_vent",
    EVENT_COMMAND_VENT: "command_vent",
    EVENT_HCP_VENTING: "hcp_venting",
    EVENT_SEQUENCE_DONE: "sequence_done",
}


def qr_payload_crc8(payload: bytes) -> int:
    crc = 0xA5
    for byte in payload:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def base36_encode(value: int, width: int) -> str:
    value = max(0, value)
    out = ""
    for _ in range(width):
        value, digit = divmod(value, 36)
        out = BASE36_ALPHABET[digit] + out
    return out


def base36_decode(text: str) -> int:
    value = 0
    for char in text:
        value = value * 36 + BASE36_ALPHABET.index(char)
    return value


def phone_sync_qr_payload(seq: int, event_code: int, elapsed_tenths: int) -> str:
    event_code %= 36
    seq %= QR_SEQ_MODULUS
    elapsed_tenths %= QR_ELAPSED_MODULUS
    body = f"{base36_encode(event_code, 1)}{base36_encode(seq, 3)}{base36_encode(elapsed_tenths, 4)}"
    crc = qr_payload_crc8(body.encode("ascii"))
    return f"{body}{base36_encode(crc, 2)}"


def parse_phone_sync_qr_payload(payload: str) -> dict[str, int]:
    payload = payload.strip()
    if len(payload) == 10 and all(char in BASE36_ALPHABET for char in payload):
        body = payload[:8]
        expected_crc = qr_payload_crc8(body.encode("ascii"))
        actual_crc = base36_decode(payload[8:10])
        if actual_crc != expected_crc:
            raise ValueError("QR payload CRC mismatch")
        return {
            "seq": base36_decode(payload[1:4]),
            "event_code": base36_decode(payload[0]),
            "elapsed_tenths": base36_decode(payload[4:8]),
        }

    if payload.startswith(QR_COMPACT_PAYLOAD_PREFIX) and len(payload) == 16:
        body = payload[:14]
        expected_crc = qr_payload_crc8(body.encode("ascii"))
        actual_crc = int(payload[14:16], 16)
        if actual_crc != expected_crc:
            raise ValueError("QR payload CRC mismatch")
        return {
            "seq": int(payload[2:8], 16),
            "event_code": int(payload[8:10], 16),
            "elapsed_tenths": int(payload[10:14], 16),
        }

    parts = payload.split(":")
    if len(parts) == 5 and parts[0] == QR_PAYLOAD_PREFIX:
        body = ":".join(parts[:4])
        expected_crc = qr_payload_crc8(body.encode("ascii"))
        actual_crc = int(parts[4], 16)
        if actual_crc != expected_crc:
            raise ValueError("QR payload CRC mismatch")
        return {
            "seq": int(parts[1], 16),
            "event_code": int(parts[2], 16),
            "elapsed_tenths": int(parts[3], 16),
        }

    raise ValueError("not a garage phone-sync QR payload")


def create_phone_sync_qr_encoder() -> Any:
    params = cv2.QRCodeEncoder_Params()
    params.version = 1
    params.correction_level = cv2.QRCodeEncoder_CORRECT_LEVEL_Q
    params.mode = cv2.QRCodeEncoder_MODE_ALPHANUMERIC
    return cv2.QRCodeEncoder_create(params)


COMMAND_EVENT = {
    "open": EVENT_COMMAND_OPEN,
    "close": EVENT_COMMAND_CLOSE,
    "vent": EVENT_COMMAND_VENT,
}

SCHEDULE_EVENT = {
    "open": EVENT_SCHEDULE_OPEN,
    "close": EVENT_SCHEDULE_CLOSE,
    "vent": EVENT_SCHEDULE_VENT,
}

COMMAND_COLOR = {
    "open": "#00ff88",
    "close": "#ff66aa",
    "vent": "#b86bff",
}

SCHEDULE_COLOR = {
    "open": "#0b4dff",
    "close": "#ff2020",
    "vent": "#8a2be2",
}


@dataclass
class HttpResult:
    ok: bool
    status: int | None
    body: bytes
    text: str
    json_data: Any | None
    error: str | None = None


class EspHttpClient:
    def __init__(self, host: str, port: int) -> None:
        self.base_url = f"http://{host}:{port}"

    def get(self, path: str, *, timeout: float = 20.0, retries: int = 2) -> HttpResult:
        url = self.base_url + path
        last = self._request(url, timeout=timeout)
        for _ in range(retries):
            if last.ok and last.text.strip() != "busy":
                return last
            time.sleep(0.5)
            last = self._request(url, timeout=timeout)
        return last

    def save(self, path: str, output: Path, *, timeout: float = 180.0, nonempty: bool = False) -> HttpResult:
        attempts = 3 if nonempty else 1
        last = self.get(path, timeout=timeout, retries=0)
        output.write_bytes(last.body)
        for _ in range(attempts - 1):
            if last.ok and len(last.body) > 0:
                return last
            time.sleep(1.0)
            last = self.get(path, timeout=timeout, retries=0)
            output.write_bytes(last.body)
        return last

    @staticmethod
    def _request(url: str, *, timeout: float) -> HttpResult:
        try:
            with urllib.request.urlopen(url, timeout=timeout) as response:
                body = response.read()
                status = response.status
        except urllib.error.HTTPError as err:
            body = err.read()
            text = body.decode("utf-8", errors="replace")
            return HttpResult(False, err.code, body, text, decode_json(text), f"HTTP {err.code}")
        except (urllib.error.URLError, TimeoutError, OSError) as err:
            return HttpResult(False, None, b"", "", None, str(err))
        text = body.decode("utf-8", errors="replace")
        return HttpResult(200 <= status < 300, status, body, text, decode_json(text), None)


def decode_json(text: str) -> Any | None:
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


def utc_now_iso() -> str:
    return dt.datetime.now(dt.UTC).replace(microsecond=0).isoformat()


def load_api_key(args: argparse.Namespace) -> str:
    if args.api_key:
        return args.api_key
    env_key = os.environ.get("ESPHOME_API_KEY") or os.environ.get("API_KEY_SUPRAMATIC_E2")
    if env_key:
        return env_key
    text = args.secrets_file.read_text()
    match = re.search(r"api_key_supramatic_e2:\s*[\"']?([^\"'\n#]+)", text)
    if not match:
        raise SystemExit(f"Could not find api_key_supramatic_e2 in {args.secrets_file}")
    return match.group(1).strip()


def print_http(label: str, result: HttpResult) -> None:
    status = result.status if result.status is not None else "failed"
    print(f"{label}: {status}")
    if result.error:
        print(f"  error: {result.error}")
    elif result.json_data is not None:
        print(json.dumps(result.json_data, indent=2, sort_keys=True)[:1000])
    elif result.text:
        print(result.text[:300])


def parse_sequence(sequence: str) -> list[dict[str, str]]:
    presets = {
        "full_and_vent": [
            ("open", "Open", "full_open"),
            ("close", "Closed", "full_close"),
            ("vent", "Venting", "vent_from_closed"),
            ("open", "Open", "setup_open_for_vent_from_open"),
            ("vent", "Venting", "vent_from_open"),
            ("close", "Closed", "final_close"),
        ],
    }
    if sequence in presets:
        source = presets[sequence]
    else:
        source = []
        for item in sequence.split(","):
            action = item.strip().lower()
            if not action:
                continue
            if action == "open":
                source.append(("open", "Open", "open"))
            elif action == "close":
                source.append(("close", "Closed", "close"))
            elif action in {"vent", "venting"}:
                source.append(("vent", "Venting", "vent"))
            else:
                raise SystemExit(f"Unsupported sequence action: {action}")
    return [{"action": action, "target_state": target, "label": label} for action, target, label in source]


class CaptureCoordinator:
    def __init__(self, args: argparse.Namespace, output_dir: Path) -> None:
        self.args = args
        self.output_dir = output_dir
        self.run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.started_monotonic = time.monotonic()
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.visual_seq = 0
        self.manifest: dict[str, Any] = {
            "started_at": utc_now_iso(),
            "run_id": self.run_id,
            "config": {
                "esp_host": args.esp_host,
                "esp_http_port": args.esp_http_port,
                "esp_api_port": args.esp_api_port,
                "command_delay_s": args.command_delay,
                "initial_delay_s": args.initial_delay,
                "settle_delay_s": args.settle_delay,
                "motion_timeout_s": args.motion_timeout,
                "cover_object_id": args.cover_object_id,
                "automatic_sequence": args.sequence,
                "dry_run": args.dry_run,
                "dry_run_motion_duration_s": args.dry_run_motion_duration,
            },
            "events": [],
        }
        self.hcp: dict[str, Any] = {}
        self.last_state_text = ""
        self.error: str | None = None
        self.scheduled: dict[str, Any] | None = None
        self.sequence_steps = parse_sequence(args.sequence)
        self.sequence_index = -1
        self.sequence_waiting_for_state: str | None = None
        self.sequence_wait_started_monotonic: float | None = None
        self.sequence_done = False
        self.last_event_code = EVENT_STARTUP
        self.last_event_name = EVENT_NAMES[EVENT_STARTUP]
        self.flash_until = self.started_monotonic + 1.0
        self.flash_color = "#ffffff"
        self.flash_label = "STARTUP"
        self.manifest_path = output_dir / "phone-sync-run.json"
        self.visual_log = (output_dir / "visual-timeline.jsonl").open("a", buffering=1)
        self.save_manifest()

    def close(self) -> None:
        self.visual_log.close()

    def run_elapsed_s(self) -> float:
        return time.monotonic() - self.started_monotonic

    def add_event(self, kind: str, label: str, data: dict[str, Any] | None = None) -> None:
        now = time.monotonic()
        event = {
            "at": utc_now_iso(),
            "monotonic_s": now,
            "run_elapsed_s": now - self.started_monotonic,
            "kind": kind,
            "label": label,
            "data": data or {},
        }
        with self.lock:
            self.manifest["events"].append(event)
            self.save_manifest_locked()

    def save_manifest(self) -> None:
        with self.lock:
            self.save_manifest_locked()

    def save_manifest_locked(self) -> None:
        self.manifest_path.write_text(json.dumps(self.manifest, indent=2, sort_keys=True) + "\n")

    def set_error(self, message: str) -> None:
        with self.lock:
            self.error = message
        self.add_event("error", "runtime_error", {"message": message})
        self.set_flash(EVENT_CANCEL, "#ffcc00", "ERROR", duration_s=2.0)

    def set_flash(self, event_code: int, color: str, label: str, *, duration_s: float = 0.75) -> None:
        with self.lock:
            self.last_event_code = event_code
            self.last_event_name = EVENT_NAMES.get(event_code, "unknown")
            self.flash_color = color
            self.flash_label = label
            self.flash_until = time.monotonic() + duration_s

    def schedule_action(self, action: str, *, delay_s: float | None = None, label: str | None = None) -> None:
        if action not in COMMAND_EVENT:
            self.set_error(f"Unsupported action: {action}")
            return
        delay = self.args.command_delay if delay_s is None else delay_s
        due = time.monotonic() + delay
        event_code = SCHEDULE_EVENT[action]
        color = SCHEDULE_COLOR[action]
        with self.lock:
            self.scheduled = {
                "action": action,
                "label": label or action,
                "delay_s": delay,
                "due_monotonic_s": due,
                "scheduled_at_monotonic_s": time.monotonic(),
            }
        self.add_event("schedule", f"{action}_scheduled", {"action": action, "label": label or action, "delay_s": delay})
        self.set_flash(event_code, color, f"{action.upper()} SCHEDULED", duration_s=0.9)

    def cancel_scheduled(self) -> None:
        with self.lock:
            old = self.scheduled
            self.scheduled = None
        self.add_event("schedule", "cancelled", {"previous": old or {}})
        self.set_flash(EVENT_CANCEL, "#ffff00", "CANCELLED", duration_s=0.7)

    def start_or_cancel(self) -> None:
        with self.lock:
            has_scheduled = self.scheduled is not None
            not_started = self.sequence_index < 0 and not self.sequence_done
        if has_scheduled:
            self.cancel_scheduled()
        elif not_started:
            self.start_auto_sequence()
        else:
            self.marker()

    def marker(self) -> None:
        self.add_event("manual_marker", "marker_flash", {})
        self.set_flash(EVENT_MANUAL_MARKER, "#ffffff", "MARKER", duration_s=1.0)

    def request_exit(self) -> None:
        self.add_event("control", "exit_requested", {})
        self.stop_event.set()

    def start_auto_sequence(self) -> None:
        if not self.sequence_steps:
            self.set_error("Automatic sequence is empty")
            return
        with self.lock:
            if self.sequence_index >= 0 or self.sequence_done:
                return
        self.add_event("sequence", "start", {"steps": self.sequence_steps, "initial_delay_s": self.args.initial_delay})
        self.advance_sequence_(initial=True)

    def advance_sequence_(self, *, initial: bool = False) -> None:
        next_index = 0 if initial else self.sequence_index + 1
        if next_index >= len(self.sequence_steps):
            with self.lock:
                self.sequence_done = True
                self.sequence_waiting_for_state = None
                self.sequence_wait_started_monotonic = None
            self.add_event("sequence", "done", {})
            self.set_flash(EVENT_SEQUENCE_DONE, "#ffffff", "SEQUENCE DONE", duration_s=2.0)
            self.request_exit()
            return

        step = self.sequence_steps[next_index]
        delay = self.args.initial_delay if initial else self.args.settle_delay
        with self.lock:
            self.sequence_index = next_index
            self.sequence_waiting_for_state = None
            self.sequence_wait_started_monotonic = None
        self.add_event("sequence", "step_scheduled", {"index": next_index, "step": step, "delay_s": delay})
        self.schedule_action(step["action"], delay_s=delay, label=step["label"])

    def on_sequence_command_sent(self, action: str) -> None:
        with self.lock:
            if self.sequence_index < 0 or self.sequence_index >= len(self.sequence_steps):
                return
            step = self.sequence_steps[self.sequence_index]
            if step["action"] != action:
                return
            self.sequence_waiting_for_state = step["target_state"]
            self.sequence_wait_started_monotonic = time.monotonic()
        self.add_event("sequence", "waiting_for_state", {"index": self.sequence_index, "target_state": step["target_state"], "step": step})

    def service_sequence_wait_(self) -> None:
        with self.lock:
            target_state = self.sequence_waiting_for_state
            wait_started = self.sequence_wait_started_monotonic
            current_state = self.last_state_text
            index = self.sequence_index
        if not target_state or wait_started is None:
            return
        if current_state.lower() == target_state.lower():
            self.add_event("sequence", "target_state_reached", {"index": index, "target_state": target_state})
            with self.lock:
                self.sequence_waiting_for_state = None
                self.sequence_wait_started_monotonic = None
            self.advance_sequence_()
            return
        if time.monotonic() - wait_started > self.args.motion_timeout:
            self.set_error(f"Timed out waiting for HCP state {target_state}")
            self.request_exit()

    def tick(self, worker: Any) -> None:
        due_action: str | None = None
        due_label: str | None = None
        with self.lock:
            if self.scheduled and time.monotonic() >= self.scheduled["due_monotonic_s"]:
                due_action = self.scheduled["action"]
                due_label = self.scheduled.get("label")
                self.scheduled = None
        if due_action is not None:
            event_code = COMMAND_EVENT[due_action]
            color = COMMAND_COLOR[due_action]
            self.add_event("command", f"{due_action}_command_requested", {"action": due_action, "label": due_label or due_action})
            self.set_flash(event_code, color, f"SEND {due_action.upper()}", duration_s=1.2)
            worker.submit_cover_command(due_action)
            self.on_sequence_command_sent(due_action)
        self.service_sequence_wait_()

    def update_hcp_state(self, object_id: str, payload: dict[str, Any]) -> None:
        flash: tuple[int, str, str] | None = None
        with self.lock:
            if "state" in payload:
                self.hcp[object_id] = payload["state"]
            if object_id == "garage_door_state":
                text = str(payload.get("state", ""))
                if text and text != self.last_state_text:
                    self.last_state_text = text
                    lower = text.lower()
                    if lower == "opening":
                        flash = (EVENT_HCP_OPENING, "#00aaff", "HCP OPENING")
                    elif lower == "closing":
                        flash = (EVENT_HCP_CLOSING, "#ff8800", "HCP CLOSING")
                    elif lower == "open":
                        flash = (EVENT_HCP_OPEN, "#00ff00", "HCP OPEN")
                    elif lower == "closed":
                        flash = (EVENT_HCP_CLOSED, "#ff0000", "HCP CLOSED")
                    elif lower == "venting":
                        flash = (EVENT_HCP_VENTING, "#bf7bff", "HCP VENTING")
            elif object_id == "garage_door":
                if "position" in payload:
                    self.hcp["cover_position"] = payload["position"]
                if "current_operation" in payload:
                    self.hcp["cover_operation"] = payload["current_operation"]
        self.add_event("native_api_state", object_id, payload)
        if flash is not None:
            code, color, label = flash
            self.set_flash(code, color, label, duration_s=0.8)

    def display_state(self) -> dict[str, Any]:
        now = time.monotonic()
        with self.lock:
            scheduled = None
            scheduled_progress = None
            if self.scheduled is not None:
                total_s = max(0.1, self.scheduled.get("delay_s", self.args.command_delay))
                remaining_s = max(0.0, self.scheduled["due_monotonic_s"] - now)
                elapsed_s = min(total_s, max(0.0, total_s - remaining_s))
                scheduled_progress = elapsed_s / total_s
                scheduled = {
                    "action": self.scheduled["action"],
                    "label": self.scheduled.get("label", self.scheduled["action"]),
                    "total_s": total_s,
                    "remaining_s": remaining_s,
                    "elapsed_s": elapsed_s,
                    "progress": scheduled_progress,
                }
            wait_elapsed_s = (
                max(0.0, now - self.sequence_wait_started_monotonic)
                if self.sequence_wait_started_monotonic is not None
                else None
            )
            current_step_progress = 0.0
            if self.sequence_done:
                current_step_progress = 1.0
            elif self.sequence_index >= 0:
                if scheduled_progress is not None:
                    current_step_progress = scheduled_progress
                elif wait_elapsed_s is not None:
                    current_step_progress = min(1.0, wait_elapsed_s / max(0.1, self.args.motion_timeout))
            sequence_count = len(self.sequence_steps)
            if self.sequence_done:
                automation_progress = 1.0
            elif self.sequence_index >= 0 and sequence_count > 0:
                automation_progress = min(1.0, max(0.0, (self.sequence_index + current_step_progress) / sequence_count))
            else:
                automation_progress = 0.0
            return {
                "run_id": self.run_id,
                "run_elapsed_s": now - self.started_monotonic,
                "run_elapsed_tenths": int((now - self.started_monotonic) * 10) & 0xFFFF,
                "event_code": self.last_event_code if now <= self.flash_until else EVENT_IDLE,
                "event_name": self.last_event_name if now <= self.flash_until else EVENT_NAMES[EVENT_IDLE],
                "flash": {
                    "active": now <= self.flash_until,
                    "color": self.flash_color,
                    "label": self.flash_label,
                },
                "scheduled": scheduled,
                "sequence": {
                    "index": self.sequence_index,
                    "count": len(self.sequence_steps),
                    "current_step": self.sequence_steps[self.sequence_index]
                    if 0 <= self.sequence_index < len(self.sequence_steps)
                    else None,
                    "waiting_for_state": self.sequence_waiting_for_state,
                    "wait_elapsed_s": wait_elapsed_s,
                    "done": self.sequence_done,
                },
                "progress": {
                    "current_step": current_step_progress,
                    "automation": automation_progress,
                },
                "hcp": dict(self.hcp),
                "error": self.error,
            }

    def log_visual_frame(self, state: dict[str, Any]) -> None:
        self.visual_seq = (self.visual_seq + 1) & 0xFFFFFF
        now = time.monotonic()
        row = {
            "at": utc_now_iso(),
            "monotonic_s": now,
            "run_elapsed_s": now - self.started_monotonic,
            "seq": self.visual_seq,
            "event_code": state["event_code"],
            "event_name": state["event_name"],
            "run_elapsed_tenths": state["run_elapsed_tenths"],
            "display_state": state,
        }
        self.visual_log.write(json.dumps(row, sort_keys=True) + "\n")


class NativeApiWorker(threading.Thread):
    def __init__(self, coordinator: CaptureCoordinator, host: str, port: int, api_key: str, expected_name: str, cover_object_id: str) -> None:
        super().__init__(daemon=True)
        self.coordinator = coordinator
        self.host = host
        self.port = port
        self.api_key = api_key
        self.expected_name = expected_name
        self.cover_object_id = cover_object_id
        self.loop: asyncio.AbstractEventLoop | None = None
        self.client: Any = None
        self.cover_key: int | None = None
        self.vent_button_key: int | None = None
        self.entity_names: dict[int, tuple[str, str]] = {}
        self.stop_requested = threading.Event()
        self.ready = threading.Event()

    def run(self) -> None:
        try:
            asyncio.run(self.run_async())
        except Exception as err:  # noqa: BLE001
            self.coordinator.add_event("native_api_error", "worker_failed", {"error": repr(err)})
            self.coordinator.set_error(f"ESPHome API worker failed: {err}")

    async def run_async(self) -> None:
        try:
            from aioesphomeapi import APIClient  # type: ignore
        except ImportError as err:
            raise RuntimeError(
                "aioesphomeapi is required. Run this tool through uv from the project root:\n"
                "  uv run garage-phone-sync"
            ) from err

        self.loop = asyncio.get_running_loop()
        self.client = APIClient(
            self.host,
            self.port,
            None,
            noise_psk=self.api_key,
            expected_name=self.expected_name,
            client_info="garage-phone-video-sync-capture",
        )
        await self.client.connect(login=True)
        _info, entities, _services = await self.client.device_info_and_list_entities()
        for entity in entities:
            object_id = getattr(entity, "object_id", "")
            self.entity_names[entity.key] = (type(entity).__name__, object_id)
            if type(entity).__name__ == "CoverInfo" and object_id == self.cover_object_id:
                self.cover_key = entity.key
            elif type(entity).__name__ == "ButtonInfo" and object_id == "garage_door_vent":
                self.vent_button_key = entity.key
        if self.cover_key is None:
            raise RuntimeError(f"Could not find cover object_id {self.cover_object_id!r}")
        if self.vent_button_key is None:
            raise RuntimeError("Could not find vent button object_id 'garage_door_vent'")
        self.client.subscribe_states(self.on_state)
        self.ready.set()
        self.coordinator.add_event("native_api", "connected", {"cover_key": self.cover_key, "vent_button_key": self.vent_button_key})
        while not self.stop_requested.is_set():
            await asyncio.sleep(0.1)
        await self.client.disconnect()

    def on_state(self, state: Any) -> None:
        _kind, object_id = self.entity_names.get(state.key, ("", ""))
        if not object_id:
            return
        payload: dict[str, Any] = {"object_id": object_id, "type": type(state).__name__}
        for attr in ("state", "position", "current_operation", "missing_state"):
            if hasattr(state, attr):
                value = getattr(state, attr)
                payload[attr] = str(value) if not isinstance(value, (int, float, bool, str, type(None))) else value
        self.coordinator.update_hcp_state(object_id, payload)

    def submit_cover_command(self, action: str) -> None:
        if self.loop is None or self.client is None or self.cover_key is None:
            self.coordinator.set_error("ESPHome API is not ready")
            return
        future = asyncio.run_coroutine_threadsafe(self.send_cover_command(action), self.loop)
        future.add_done_callback(self.on_command_done)

    def on_command_done(self, future: "asyncio.Future[None]") -> None:
        try:
            future.result()
        except Exception as err:  # noqa: BLE001
            self.coordinator.set_error(f"ESPHome command failed: {err}")

    async def send_cover_command(self, action: str) -> None:
        if action == "open":
            self.client.cover_command(self.cover_key, position=1.0)
        elif action == "close":
            self.client.cover_command(self.cover_key, position=0.0)
        elif action == "vent":
            self.client.button_command(self.vent_button_key)
        elif action == "stop":
            self.client.cover_command(self.cover_key, stop=True)
        else:
            raise ValueError(f"Unsupported cover action: {action}")
        self.coordinator.add_event("native_api_command", f"cover_{action}_sent", {"action": action})

    def stop(self) -> None:
        self.stop_requested.set()


class DryRunWorker:
    """No-op command worker that simulates enough HCP feedback for visual checks."""

    def __init__(self, coordinator: CaptureCoordinator, motion_duration_s: float) -> None:
        self.coordinator = coordinator
        self.motion_duration_s = motion_duration_s
        self.ready = threading.Event()
        self.ready.set()
        self.stopped = threading.Event()
        self.lock = threading.Lock()
        self.timers: list[threading.Timer] = []

    def start(self) -> None:
        self.coordinator.add_event(
            "dry_run",
            "started",
            {"motion_duration_s": self.motion_duration_s, "note": "No ESP connection and no opener commands"},
        )
        self._publish_state("Closed", position=0.0, operation="IDLE", raw="0xD00002")

    def is_alive(self) -> bool:
        return False

    def join(self, timeout: float | None = None) -> None:  # noqa: ARG002
        return

    def stop(self) -> None:
        self.stopped.set()
        with self.lock:
            timers = list(self.timers)
            self.timers.clear()
        for timer in timers:
            timer.cancel()

    def submit_cover_command(self, action: str) -> None:
        if self.stopped.is_set():
            return
        plan = self._simulation_plan(action)
        self.coordinator.add_event(
            "dry_run_command",
            f"{action}_simulated",
            {"action": action, "duration_s": self.motion_duration_s, "plan": plan},
        )
        self._publish_state(
            plan["moving_state"],
            position=plan["moving_position"],
            operation=plan["moving_operation"],
            raw=plan["moving_raw"],
        )
        timer = threading.Timer(self.motion_duration_s, self._finish_command, args=(plan,))
        with self.lock:
            self.timers.append(timer)
        timer.start()

    def _finish_command(self, plan: dict[str, Any]) -> None:
        if self.stopped.is_set():
            return
        self._publish_state(
            plan["target_state"],
            position=plan["target_position"],
            operation="IDLE",
            raw=plan["target_raw"],
        )
        self.coordinator.add_event("dry_run_command", "target_state_simulated", plan)
        with self.lock:
            self.timers = [timer for timer in self.timers if timer.is_alive()]

    def _simulation_plan(self, action: str) -> dict[str, Any]:
        if action == "open":
            return {
                "moving_state": "Opening",
                "target_state": "Open",
                "moving_position": 0.5,
                "target_position": 1.0,
                "moving_operation": "OPENING",
                "moving_raw": "0xD00040",
                "target_raw": "0xD00001",
            }
        if action == "close":
            return {
                "moving_state": "Closing",
                "target_state": "Closed",
                "moving_position": 0.5,
                "target_position": 0.0,
                "moving_operation": "CLOSING",
                "moving_raw": "0xD00060",
                "target_raw": "0xD00002",
            }
        if action == "vent":
            moving_state = "Opening"
            moving_operation = "OPENING"
            with self.coordinator.lock:
                current_step = (
                    self.coordinator.sequence_steps[self.coordinator.sequence_index]
                    if 0 <= self.coordinator.sequence_index < len(self.coordinator.sequence_steps)
                    else {}
                )
            if current_step.get("label") == "vent_from_open":
                moving_state = "Closing"
                moving_operation = "CLOSING"
            return {
                "moving_state": moving_state,
                "target_state": "Venting",
                "moving_position": 0.6,
                "target_position": 0.2,
                "moving_operation": moving_operation,
                "moving_raw": "0xD00080",
                "target_raw": "0xD00081",
            }
        raise ValueError(f"Unsupported cover action: {action}")

    def _publish_state(self, state: str, *, position: float, operation: str, raw: str) -> None:
        self.coordinator.update_hcp_state("garage_door_valid_hcp_broadcast", {"state": True})
        self.coordinator.update_hcp_state("garage_door_raw_hcp_status_hex", {"state": raw})
        self.coordinator.update_hcp_state("garage_door_obstruction_state", {"state": False})
        self.coordinator.update_hcp_state("garage_door_light", {"state": False})
        self.coordinator.update_hcp_state("garage_door", {"position": position, "current_operation": operation})
        self.coordinator.update_hcp_state("garage_door_state", {"state": state})


class FullscreenDisplay:
    def __init__(self, coordinator: CaptureCoordinator, worker: Any) -> None:
        self.coordinator = coordinator
        self.worker = worker
        self.root = tk.Tk()
        self.root.title("Garage Sync Capture")
        self.root.attributes("-fullscreen", True)
        self.root.configure(background="black")
        self.canvas = tk.Canvas(self.root, background="black", highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.qr_encoder = create_phone_sync_qr_encoder()
        self.qr_cache_payload: str | None = None
        self.qr_cache_matrix: np.ndarray | None = None
        self.root.bind("<Escape>", self.on_exit)
        self.root.bind("q", self.on_exit)
        self.root.bind("Q", self.on_exit)
        self.root.bind("<space>", lambda _event: self.coordinator.start_or_cancel())
        self.root.bind("m", lambda _event: self.coordinator.marker())
        self.root.bind("M", lambda _event: self.coordinator.marker())

    def on_exit(self, _event: object | None = None) -> None:
        self.coordinator.request_exit()

    def run(self) -> None:
        self.root.after(100, self.draw)
        self.root.mainloop()

    def draw(self) -> None:
        self.coordinator.tick(self.worker)
        state = self.coordinator.display_state()
        self.coordinator.log_visual_frame(state)
        self.render(state)
        if self.coordinator.stop_event.is_set():
            self.root.destroy()
            return
        self.root.after(100, self.draw)

    def render(self, state: dict[str, Any]) -> None:
        canvas = self.canvas
        canvas.delete("all")
        w = max(canvas.winfo_width(), 800)
        h = max(canvas.winfo_height(), 600)
        flash = state["flash"]
        bg = flash["color"] if flash["active"] else "#05070a"
        fg = "#000000" if flash["active"] else "#ffffff"
        muted = "#111111" if flash["active"] else "#b8c7d9"
        accent = "#000000" if flash["active"] else "#f4d35e"
        canvas.create_rectangle(0, 0, w, h, fill=bg, outline=bg)

        margin = max(6, int(min(w, h) * 0.006))
        small_size = max(11, min(20, int(h * 0.014)))
        status_size = max(18, min(34, int(h * 0.026)))
        line_gap = int(small_size * 1.35)

        title = "GARAGE SYNC / DRY RUN" if self.coordinator.args.dry_run else "GARAGE SYNC"
        elapsed = state["run_elapsed_s"]
        hcp = state["hcp"]
        sequence = state["sequence"]
        current_step = sequence["current_step"] or {}
        step_text = "not started" if sequence["index"] < 0 else f"{sequence['index'] + 1}/{sequence['count']} {current_step.get('label', '')}"
        if sequence["done"]:
            step_text = "done"

        qr_payload = phone_sync_qr_payload(self.coordinator.visual_seq, state["event_code"], state["run_elapsed_tenths"])
        qr_matrix = self.qr_matrix(qr_payload)
        qr_size = self.qr_size(w, h, margin, qr_matrix.shape[0])
        qr_x = (w - qr_size) / 2
        qr_y = (h - qr_size) / 2
        self.draw_qr_code(qr_x, qr_y, qr_size, qr_matrix)

        scheduled = state["scheduled"]
        if scheduled:
            action = scheduled["action"].upper()
            remaining = scheduled["remaining_s"]
            status = f"{action} IN {remaining:0.1f}s"
            center_color = "#64d2ff" if action == "OPEN" else "#bf7bff" if action == "VENT" else "#ff6b6b"
        elif state["sequence"]["index"] < 0 and not state["sequence"]["done"] and not flash["active"]:
            status = "SPACE TO START"
            center_color = "#ffffff"
        elif flash["active"] and flash["label"]:
            status = flash["label"]
            center_color = fg
        else:
            status = "READY"
            center_color = fg

        if not getattr(self.coordinator.args, "hide_side_status", False):
            self.draw_progress_bars(w, h, state.get("progress", {}), flash["active"])
            self.draw_side_status(
                w=w,
                h=h,
                margin=margin,
                qr_x=qr_x,
                qr_size=qr_size,
                status=status,
                status_color=center_color,
                step_text=step_text,
                hcp=hcp,
                elapsed=elapsed,
                muted=muted,
                accent=accent,
            )

        if self.coordinator.args.show_overlay_text:
            left_width = max(0.0, qr_x - 2 * margin)
            right_width = max(0.0, w - (qr_x + qr_size) - 2 * margin)
            if left_width >= 130:
                self.text(margin, margin, self.truncate(title, left_width, status_size), status_size, fg, "nw")
                self.text(margin, margin + line_gap * 1.7, self.truncate(status, left_width, status_size), status_size, center_color, "nw")
                self.text(margin, margin + line_gap * 3.6, self.truncate(f"seq {self.coordinator.visual_seq:06d}", left_width, small_size), small_size, accent, "nw")
                self.text(margin, margin + line_gap * 4.8, self.truncate(f"event {state['event_code']:02d} {state['event_name']}", left_width, small_size), small_size, accent, "nw")
                self.text(margin, margin + line_gap * 6.4, self.truncate(f"Sequence: {step_text}", left_width, small_size), small_size, "#f4d35e", "nw")
                self.text(margin, margin + line_gap * 7.6, self.truncate(f"HCP: {hcp.get('garage_door_state', '-')}", left_width, small_size), small_size, fg, "nw")
                self.text(margin, margin + line_gap * 8.8, self.truncate(f"Raw: {hcp.get('garage_door_raw_hcp_status_hex', '-')}", left_width, small_size), small_size, fg, "nw")
            if scheduled and left_width >= 80:
                self.draw_countdown_bar(
                    margin,
                    min(h - margin - small_size * 4, margin + line_gap * 11),
                    left_width,
                    max(12, small_size),
                    scheduled["remaining_s"],
                    scheduled.get("total_s", self.coordinator.args.command_delay),
                    scheduled["action"].upper(),
                )
            if right_width >= 130:
                x = w - margin
                mode = "DRY RUN" if self.coordinator.args.dry_run else "LIVE"
                self.text(x, margin, self.truncate(mode, right_width, status_size), status_size, fg, "ne")
                self.text(x, margin + line_gap * 1.7, self.truncate(f"T {elapsed:07.1f}s", right_width, status_size), status_size, accent, "ne")
                self.text(x, margin + line_gap * 3.6, self.truncate(f"run {state['run_id']}", right_width, small_size), small_size, muted, "ne")
                self.text(x, margin + line_gap * 5.2, self.truncate(f"pos {self.format_percent(hcp.get('cover_position'))}", right_width, small_size), small_size, fg, "ne")
                self.text(x, margin + line_gap * 6.4, self.truncate(f"op {hcp.get('cover_operation', '-')}", right_width, small_size), small_size, fg, "ne")
                self.text(x, margin + line_gap * 7.6, self.truncate(f"valid {hcp.get('garage_door_valid_hcp_broadcast', '-')}", right_width, small_size), small_size, fg, "ne")
                self.text(x, margin + line_gap * 8.8, self.truncate(f"obs {hcp.get('garage_door_obstruction_state', '-')}", right_width, small_size), small_size, fg, "ne")
                self.text(x, h - margin - line_gap, self.truncate("SPACE start/cancel", right_width, small_size), small_size, "#d7dee8", "se")
                self.text(x, h - margin, self.truncate("M marker   Q/Esc finish", right_width, small_size), small_size, "#d7dee8", "se")
            if state["error"]:
                self.text(margin, h - margin, self.truncate(f"ERROR {state['error']}", left_width or w - 2 * margin, small_size), small_size, "#ff6b6b", "sw")

    def qr_size(self, width: int, height: int, margin: int, modules: int) -> int:
        available_width = max(320, width - 2 * margin)
        available_height = max(220.0, height - 2 * margin)
        max_size = int(min(available_width, available_height))
        for module_px in QR_SAFE_MODULE_PIXEL_SIZES:
            size = modules * module_px
            if size <= max_size:
                return size
        return max(220, (max_size // modules) * modules)

    def draw_progress_bars(self, w: int, h: int, progress: dict[str, Any], flash_active: bool) -> None:
        bar_width = max(8, min(18, int(min(w, h) * 0.012)))
        inset = max(8, int(h * 0.012))
        track_top = inset
        track_bottom = h - inset
        track_height = max(1.0, track_bottom - track_top)
        left_progress = self.clamp01(progress.get("current_step", 0.0))
        right_progress = self.clamp01(progress.get("automation", 0.0))
        track_fill = "#f2f2f2" if flash_active else "#18202b"
        track_outline = "#000000" if flash_active else "#405066"
        self.draw_vertical_progress_bar(
            0,
            track_top,
            bar_width,
            track_height,
            left_progress,
            track_fill=track_fill,
            track_outline=track_outline,
            fill="#f4d35e" if not flash_active else "#000000",
        )
        self.draw_vertical_progress_bar(
            w - bar_width,
            track_top,
            bar_width,
            track_height,
            right_progress,
            track_fill=track_fill,
            track_outline=track_outline,
            fill="#64d2ff" if not flash_active else "#000000",
        )

    def draw_vertical_progress_bar(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        progress: float,
        *,
        track_fill: str,
        track_outline: str,
        fill: str,
    ) -> None:
        self.canvas.create_rectangle(x, y, x + width, y + height, fill=track_fill, outline=track_outline, width=1)
        fill_height = height * progress
        if fill_height <= 0:
            return
        self.canvas.create_rectangle(x, y + height - fill_height, x + width, y + height, fill=fill, outline=fill, width=0)

    def draw_side_status(
        self,
        *,
        w: int,
        h: int,
        margin: int,
        qr_x: float,
        qr_size: int,
        status: str,
        status_color: str,
        step_text: str,
        hcp: dict[str, Any],
        elapsed: float,
        muted: str,
        accent: str,
    ) -> None:
        left_width = max(0.0, qr_x - 2 * margin)
        right_width = max(0.0, w - (qr_x + qr_size) - 2 * margin)
        vertical_space = max(120.0, h - 2 * margin)
        side_size = max(14, min(30, int(min(h * 0.028, max(left_width, right_width) * 0.42))))
        if left_width >= side_size * 2.0:
            left_text = "  |  ".join(
                [
                    status,
                    f"step {step_text}",
                    f"HCP {hcp.get('garage_door_state', '-')}",
                ]
            )
            self.text(
                margin + left_width / 2,
                h / 2,
                self.truncate(left_text, vertical_space, side_size),
                side_size,
                status_color,
                "center",
                angle=90,
            )
        if right_width >= side_size * 2.0:
            right_text = "  |  ".join(
                [
                    f"T {elapsed:0.1f}s",
                    f"pos {self.format_percent(hcp.get('cover_position'))}",
                    f"op {hcp.get('cover_operation', '-')}",
                    f"obs {hcp.get('garage_door_obstruction_state', '-')}",
                ]
            )
            self.text(
                w - margin - right_width / 2,
                h / 2,
                self.truncate(right_text, vertical_space, side_size),
                side_size,
                accent if hcp.get("garage_door_valid_hcp_broadcast") else muted,
                "center",
                angle=270,
            )

    def draw_countdown_bar(self, x: float, y: float, width: float, height: float, remaining: float, total: float, action: str) -> None:
        frac = max(0.0, min(1.0, remaining / max(total, 0.1)))
        if action == "OPEN":
            color = "#64d2ff"
        elif action == "VENT":
            color = "#bf7bff"
        else:
            color = "#ff6b6b"
        self.canvas.create_rectangle(x, y, x + width, y + height, fill="#18202b", outline="#4d5a6d", width=2)
        self.canvas.create_rectangle(x, y, x + width * frac, y + height, fill=color, outline=color)

    def draw_qr_code(self, x: float, y: float, size: int, matrix: np.ndarray) -> None:
        modules = int(matrix.shape[0])
        module_size = size / modules
        self.canvas.create_rectangle(x, y, x + size, y + size, fill="#ffffff", outline="#ffffff")
        for row in range(modules):
            for col in range(modules):
                if matrix[row, col] < 128:
                    self.canvas.create_rectangle(
                        x + col * module_size,
                        y + row * module_size,
                        x + (col + 1) * module_size,
                        y + (row + 1) * module_size,
                        fill="#000000",
                        outline="#000000",
                    )

    def qr_matrix(self, payload: str) -> np.ndarray:
        if getattr(self, "qr_cache_payload", None) == payload and getattr(self, "qr_cache_matrix", None) is not None:
            return self.qr_cache_matrix
        if not hasattr(self, "qr_encoder"):
            self.qr_encoder = create_phone_sync_qr_encoder()
        matrix = self.qr_encoder.encode(payload)
        if QR_QUIET_ZONE_MODULES > 0:
            matrix = np.pad(matrix, QR_QUIET_ZONE_MODULES, mode="constant", constant_values=255)
        self.qr_cache_payload = payload
        self.qr_cache_matrix = matrix
        return matrix

    @staticmethod
    def truncate(text: str, max_width: float, font_size: int) -> str:
        max_chars = max(4, int(max_width / max(font_size * 0.62, 1)))
        if len(text) <= max_chars:
            return text
        return text[: max(1, max_chars - 1)] + "…"

    def text(self, x: float, y: float, text: str, size: int, color: str, anchor: str, angle: int = 0) -> None:
        self.canvas.create_text(
            x,
            y,
            text=text,
            fill=color,
            font=("Menlo", size, "bold"),
            anchor=anchor,
            angle=angle,
        )

    @staticmethod
    def format_percent(value: Any) -> str:
        try:
            return f"{float(value) * 100.0:.1f}%"
        except (TypeError, ValueError):
            return "-"

    @staticmethod
    def clamp01(value: Any) -> float:
        try:
            number = float(value)
        except (TypeError, ValueError):
            return 0.0
        return max(0.0, min(1.0, number))


def start_persistent_log(coordinator: CaptureCoordinator, esp: EspHttpClient) -> None:
    for label, path, timeout in [
        ("persistent_log_clear", "/persistent_log/clear", 12),
        ("persistent_log_start", "/persistent_log/start", 12),
    ]:
        print(f"{label}: requesting {path} ...", flush=True)
        result = esp.get(path, timeout=timeout, retries=2)
        print_http(label, result)
        coordinator.add_event("esp_http", label, {"status": result.status, "error": result.error, "json": result.json_data})
        if not result.ok:
            raise SystemExit(f"ESP HTTP {label} failed")


def stop_persistent_log(coordinator: CaptureCoordinator, esp: EspHttpClient) -> HttpResult:
    print("persistent_log_stop: requesting /persistent_log/stop ...", flush=True)
    stop = esp.get("/persistent_log/stop", timeout=12, retries=2)
    print_http("persistent_log_stop", stop)
    coordinator.add_event("esp_http", "persistent_log_stop", {"status": stop.status, "error": stop.error, "json": stop.json_data})
    return stop


def stop_and_download(coordinator: CaptureCoordinator, esp: EspHttpClient) -> None:
    stop_persistent_log(coordinator, esp)
    downloads = {
        "esp-stats-final.json": ("/stats", 15, False),
        "esp-broadcast-status-final.json": ("/broadcast_status", 15, False),
        "esp-recent-final.txt": ("/recent", 30, False),
        "persistent-log.json": ("/persistent_log", 240, True),
        "persistent-log.bin": ("/persistent_log.bin", 240, False),
    }
    for filename, (path, timeout, nonempty) in downloads.items():
        print(f"Downloading {filename} from {path} ...", flush=True)
        result = esp.save(path, coordinator.output_dir / filename, timeout=timeout, nonempty=nonempty)
        coordinator.add_event(
            "download",
            filename,
            {"endpoint": path, "status": result.status, "error": result.error, "bytes": len(result.body)},
        )
        print(f"Saved {filename}: {len(result.body)} bytes")


def wait_for_tcp(host: str, port: int, timeout_s: float) -> bool:
    deadline = time.monotonic() + max(0.0, timeout_s)
    last_error: str | None = None
    while True:
        try:
            with socket.create_connection((host, port), timeout=3):
                if last_error:
                    print(f"ESP HTTP port is reachable again at {host}:{port}", flush=True)
                return True
        except OSError as err:
            last_error = str(err)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                print(f"ESP HTTP port is not reachable at {host}:{port}: {last_error}", flush=True)
                return False
            print(f"Waiting for ESP HTTP port {host}:{port}: {last_error}", flush=True)
            time.sleep(min(2.0, remaining))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Fullscreen phone video sync display and HCP command coordinator")
    parser.add_argument("--esp-host", default=os.environ.get("ESP_HOST", "supramatic-e2.local"))
    parser.add_argument("--esp-http-port", type=int, default=int(os.environ.get("ESP_PORT", "8080")))
    parser.add_argument("--esp-api-port", type=int, default=int(os.environ.get("ESPHOME_API_PORT", "6053")))
    parser.add_argument("--expected-name", default="supramatic-e2")
    parser.add_argument("--cover-object-id", default=DEFAULT_COVER_OBJECT_ID)
    parser.add_argument("--api-key", default=os.environ.get("ESPHOME_API_KEY"))
    parser.add_argument("--secrets-file", type=Path, default=Path("secrets.yaml"))
    parser.add_argument("--command-delay", type=float, default=10.0, help="Fallback delay for manual scheduled commands")
    parser.add_argument("--initial-delay", type=float, default=15.0, help="Delay after Space before the first automatic command")
    parser.add_argument("--settle-delay", type=float, default=10.0, help="Delay between one reached HCP state and the next command")
    parser.add_argument("--motion-timeout", type=float, default=90.0, help="Maximum seconds to wait for each HCP target state")
    parser.add_argument("--dry-run", action="store_true", help="Show the fullscreen visuals and simulated HCP feedback without contacting the ESP or moving the opener")
    parser.add_argument("--dry-run-motion-duration", type=float, default=3.0, help="Seconds before each simulated dry-run command reaches its target state")
    parser.add_argument("--startup-check", action="store_true", help="Start persistent logging and connect to ESPHome, then stop and exit without opening the UI or sending commands")
    parser.add_argument("--startup-reachability-timeout", type=float, default=45.0, help="Seconds to wait for the ESP HTTP port before starting the real capture")
    parser.add_argument(
        "--show-overlay-text",
        action="store_true",
        help="Show small side debug text next to the QR; disabled by default to keep the video code unobstructed",
    )
    parser.add_argument(
        "--hide-side-status",
        action="store_true",
        help="Hide the default rotated side status text and progress bars, showing only the QR code",
    )
    parser.add_argument(
        "--sequence",
        default="full_and_vent",
        help="Preset 'full_and_vent' or comma-separated commands such as open,close,vent,open,vent,close",
    )
    parser.add_argument("--output-dir", type=Path, default=Path("captures"))
    parser.add_argument("--yes", action="store_true", help="Skip the physical-presence prompt")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.yes and not args.dry_run:
        print("This tool will run an automatic garage-door movement sequence from fullscreen input.")
        input("Confirm you are physically present, the door path is clear, and press Enter to continue: ")
    elif args.dry_run:
        print("Dry run mode: no ESP connection, no persistent logging, and no opener commands will be sent.")

    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    prefix = "phone-sync-dry-run" if args.dry_run else "phone-sync-capture"
    output_dir = args.output_dir / f"{prefix}-{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    coordinator = CaptureCoordinator(args, output_dir)
    esp = None if args.dry_run else EspHttpClient(args.esp_host, args.esp_http_port)
    persistent_log_started = False
    worker_started = False
    worker: Any
    if args.dry_run:
        worker = DryRunWorker(coordinator, args.dry_run_motion_duration)
    else:
        api_key = load_api_key(args)
        worker = NativeApiWorker(coordinator, args.esp_host, args.esp_api_port, api_key, args.expected_name, args.cover_object_id)

    try:
        if esp is not None:
            if not wait_for_tcp(args.esp_host, args.esp_http_port, args.startup_reachability_timeout):
                raise SystemExit("ESP HTTP port is not reachable; capture was not started")
            start_persistent_log(coordinator, esp)
            persistent_log_started = True
            print("persistent_log: started", flush=True)
        print("native_api_worker: starting ...", flush=True)
        worker.start()
        worker_started = True
        if not worker.ready.wait(timeout=20):
            raise SystemExit("Timed out connecting to ESPHome native API")
        print("native_api_worker: ready", flush=True)
        if args.startup_check:
            print("Startup check passed: persistent log started and ESPHome native API is ready.")
            return 0
        print()
        print(f"Output bundle: {output_dir}")
        print("Controls: Space=start/cancel, M=marker flash, Q/Esc=finish.")
        print(f"Automatic sequence: {args.sequence}; first command starts {args.initial_delay:g}s after Space.")
        FullscreenDisplay(coordinator, worker).run()
    except KeyboardInterrupt:
        coordinator.add_event("control", "keyboard_interrupt", {})
    finally:
        coordinator.stop_event.set()
        if worker_started:
            worker.stop()
            if worker.is_alive():
                worker.join(timeout=5)
        if esp is not None and persistent_log_started:
            if args.startup_check:
                stop_persistent_log(coordinator, esp)
            else:
                stop_and_download(coordinator, esp)
        elif esp is not None:
            print("Skipping persistent log stop/download because logging did not start.", flush=True)
        coordinator.manifest["finished_at"] = utc_now_iso()
        coordinator.save_manifest()
        coordinator.close()
        print(f"Phone sync capture bundle saved in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
