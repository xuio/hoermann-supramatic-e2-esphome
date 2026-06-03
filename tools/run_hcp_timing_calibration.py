#!/usr/bin/env python3
"""Run an HCP-only timing calibration cycle through the ESPHome native API."""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_COVER_OBJECT_ID = "garage_door"


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

    def save(self, path: str, output: Path, *, timeout: float = 180.0) -> HttpResult:
        result = self.get(path, timeout=timeout, retries=0)
        output.write_bytes(result.body)
        return result

    def save_nonempty(self, path: str, output: Path, *, timeout: float = 180.0, attempts: int = 3) -> HttpResult:
        last = self.save(path, output, timeout=timeout)
        for _ in range(attempts - 1):
            if last.ok and len(last.body) > 0:
                return last
            time.sleep(1.0)
            last = self.save(path, output, timeout=timeout)
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
    secrets = args.secrets_file.read_text()
    match = re.search(r"api_key_supramatic_e2:\s*[\"']?([^\"'\n#]+)", secrets)
    if not match:
        raise SystemExit(f"Could not find api_key_supramatic_e2 in {args.secrets_file}")
    return match.group(1).strip()


def print_http(label: str, result: HttpResult) -> None:
    status = result.status if result.status is not None else "failed"
    print(f"{label}: {status}")
    if result.error:
        print(f"  error: {result.error}")
    if result.json_data is not None:
        print(json.dumps(result.json_data, indent=2, sort_keys=True)[:1200])
    elif result.text:
        print(result.text[:400])


class NativeApiSession:
    def __init__(self, host: str, port: int, api_key: str, expected_name: str, cover_object_id: str) -> None:
        try:
            from aioesphomeapi import APIClient, CoverOperation  # type: ignore
        except ImportError as err:
            raise SystemExit(
                "aioesphomeapi is required for direct ESPHome commands. "
                "Install the project requirements in a virtual environment, or run this script with ESPHome's Python."
            ) from err
        self.api_client_cls = APIClient
        self.cover_operation_cls = CoverOperation
        self.host = host
        self.port = port
        self.api_key = api_key
        self.expected_name = expected_name
        self.cover_object_id = cover_object_id
        self.client: Any = None
        self.cover_key: int | None = None
        self.names: dict[int, tuple[str, str]] = {}
        self.states: dict[str, Any] = {}

    async def __aenter__(self) -> "NativeApiSession":
        self.client = self.api_client_cls(
            self.host,
            self.port,
            None,
            noise_psk=self.api_key,
            expected_name=self.expected_name,
            client_info="garage-hcp-timing-calibration",
        )
        await self.client.connect(login=True)
        _info, entities, _services = await self.client.device_info_and_list_entities()
        for entity in entities:
            object_id = getattr(entity, "object_id", "")
            self.names[entity.key] = (type(entity).__name__, object_id)
            if type(entity).__name__ == "CoverInfo" and object_id == self.cover_object_id:
                self.cover_key = entity.key
        if self.cover_key is None:
            covers = [object_id for _kind, object_id in self.names.values() if object_id]
            raise SystemExit(f"Could not find cover object_id {self.cover_object_id!r}; entities: {covers}")
        self.client.subscribe_states(self._on_state)
        await asyncio.sleep(2.0)
        return self

    async def __aexit__(self, *_exc: object) -> None:
        if self.client is not None:
            await self.client.disconnect()

    def _on_state(self, state: Any) -> None:
        _kind, object_id = self.names.get(state.key, ("", ""))
        if object_id:
            self.states[object_id] = state

    def state_snapshot(self) -> dict[str, Any]:
        snapshot: dict[str, Any] = {}
        for object_id, state in self.states.items():
            item: dict[str, Any] = {"type": type(state).__name__}
            for attr in ("state", "position", "current_operation", "missing_state"):
                if hasattr(state, attr):
                    value = getattr(state, attr)
                    item[attr] = str(value) if not isinstance(value, (int, float, bool, str, type(None))) else value
            snapshot[object_id] = item
        return snapshot

    def text_state(self) -> str:
        state = self.states.get("garage_door_state")
        return str(getattr(state, "state", ""))

    async def command_cover(self, action: str) -> None:
        assert self.client is not None
        assert self.cover_key is not None
        if action == "open":
            self.client.cover_command(self.cover_key, position=1.0)
        elif action == "close":
            self.client.cover_command(self.cover_key, position=0.0)
        elif action == "stop":
            self.client.cover_command(self.cover_key, stop=True)
        else:
            raise ValueError(f"Unsupported cover action: {action}")

    async def wait_for_text_state(self, expected: str, timeout_s: float) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_s
        last_text = ""
        while time.monotonic() < deadline:
            last_text = self.text_state()
            if last_text.lower() == expected.lower():
                await asyncio.sleep(2.0)
                return {"ok": True, "state": last_text}
            await asyncio.sleep(0.25)
        return {"ok": False, "state": last_text, "error": f"timeout waiting for {expected}"}


async def run_cycle(args: argparse.Namespace, output_dir: Path) -> dict[str, Any]:
    api_key = load_api_key(args)
    esp = EspHttpClient(args.esp_host, args.esp_http_port)
    manifest: dict[str, Any] = {
        "started_at": utc_now_iso(),
        "config": {
            "esp_host": args.esp_host,
            "esp_http_port": args.esp_http_port,
            "esp_api_port": args.esp_api_port,
            "expected_name": args.expected_name,
            "cover_object_id": args.cover_object_id,
            "sequence": args.sequence,
        },
        "events": [],
    }

    def add_event(kind: str, label: str, data: dict[str, Any] | None = None) -> None:
        manifest["events"].append(
            {
                "at": utc_now_iso(),
                "monotonic_s": time.monotonic(),
                "kind": kind,
                "label": label,
                "data": data or {},
            }
        )
        (output_dir / "hcp-timing-run.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    if not args.yes:
        print("This will move the garage door through the configured sequence.")
        input("Confirm you are physically present, the door path is clear, and press Enter to continue: ")

    for label, path, timeout in [
        ("persistent_log_clear", "/persistent_log/clear", 30),
        ("persistent_log_start", "/persistent_log/start", 30),
    ]:
        result = esp.get(path, timeout=timeout, retries=2)
        print_http(label, result)
        add_event("esp_http", label, {"status": result.status, "error": result.error, "json": result.json_data})
        if not result.ok:
            raise SystemExit(f"ESP HTTP {label} failed")

    async with NativeApiSession(
        args.esp_host,
        args.esp_api_port,
        api_key,
        args.expected_name,
        args.cover_object_id,
    ) as session:
        add_event("native_api", "initial_state", session.state_snapshot())
        print("Initial ESPHome state:")
        print(json.dumps(session.state_snapshot(), indent=2, sort_keys=True))

        for action in args.sequence.split(","):
            action = action.strip().lower()
            if action not in {"open", "close"}:
                raise SystemExit(f"Unsupported sequence action: {action}")
            expected = "Open" if action == "open" else "Closed"
            print(f"Sending cover {action}; waiting for HCP text state {expected}.")
            add_event("native_api_command", f"cover_{action}_start", session.state_snapshot())
            await session.command_cover(action)
            add_event("native_api_command", f"cover_{action}_sent", session.state_snapshot())
            result = await session.wait_for_text_state(expected, args.motion_timeout)
            add_event("native_api_wait", f"wait_{expected.lower()}", result | {"snapshot": session.state_snapshot()})
            print(json.dumps(result, indent=2, sort_keys=True))
            if not result.get("ok"):
                raise SystemExit(result.get("error") or f"Timed out waiting for {expected}")
            await asyncio.sleep(args.settle_time)

    stop = esp.get("/persistent_log/stop", timeout=30, retries=2)
    print_http("persistent_log_stop", stop)
    add_event("esp_http", "persistent_log_stop", {"status": stop.status, "error": stop.error, "json": stop.json_data})

    downloads = {
        "esp-stats-final.json": ("/stats", 15),
        "esp-broadcast-status-final.json": ("/broadcast_status", 15),
        "esp-recent-final.txt": ("/recent", 30),
        "persistent-log.json": ("/persistent_log", 240),
        "persistent-log.bin": ("/persistent_log.bin", 240),
    }
    for filename, (path, timeout) in downloads.items():
        if filename == "persistent-log.json":
            result = esp.save_nonempty(path, output_dir / filename, timeout=timeout, attempts=3)
        else:
            result = esp.save(path, output_dir / filename, timeout=timeout)
        add_event("download", filename, {"endpoint": path, "status": result.status, "error": result.error, "bytes": len(result.body)})
        print(f"Saved {filename}: {len(result.body)} bytes")

    manifest["finished_at"] = utc_now_iso()
    (output_dir / "hcp-timing-run.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def run_analyzer(args: argparse.Namespace, output_dir: Path) -> None:
    command = [
        sys.executable,
        str(Path(__file__).with_name("analyze_hcp_timing.py")),
        "--persistent-log",
        str(output_dir / ("persistent-log.bin" if (output_dir / "persistent-log.json").stat().st_size == 0 else "persistent-log.json")),
        "--curve-lookup",
        str(args.curve_lookup),
        "--output-dir",
        str(output_dir),
    ]
    subprocess.run(command, check=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run HCP-only timing calibration against an ESPHome UAP1 emulator")
    parser.add_argument("--esp-host", default=os.environ.get("ESP_HOST", "supramatic-e2.local"))
    parser.add_argument("--esp-http-port", type=int, default=int(os.environ.get("ESP_PORT", "8080")))
    parser.add_argument("--esp-api-port", type=int, default=int(os.environ.get("ESPHOME_API_PORT", "6053")))
    parser.add_argument("--expected-name", default="supramatic-e2")
    parser.add_argument("--cover-object-id", default=DEFAULT_COVER_OBJECT_ID)
    parser.add_argument("--api-key", default=os.environ.get("ESPHOME_API_KEY"))
    parser.add_argument("--secrets-file", type=Path, default=Path("configs/secrets.yaml"))
    parser.add_argument("--sequence", default="open,close", help="Comma-separated full-travel sequence, for example open,close")
    parser.add_argument("--curve-lookup", type=Path, default=Path("docs/research/analysis/garage-door-motion-20260527/curve_lookup.json"))
    parser.add_argument("--motion-timeout", type=float, default=90.0)
    parser.add_argument("--settle-time", type=float, default=4.0)
    parser.add_argument("--output-dir", type=Path, default=Path("captures"))
    parser.add_argument("--yes", action="store_true", help="Skip the physical-presence prompt")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = args.output_dir / f"hcp-timing-calibration-{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    asyncio.run(run_cycle(args, output_dir))
    run_analyzer(args, output_dir)
    print(f"Calibration bundle saved in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
