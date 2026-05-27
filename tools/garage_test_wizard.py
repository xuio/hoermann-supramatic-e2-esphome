#!/usr/bin/env python3
"""Interactive Garage Door test and calibration wizard.

The wizard guides a physical test session for the SupraMatic E2 firmware. It
can control Home Assistant when a long-lived access token is supplied, but every
movement still requires a manual confirmation before the next step. Without a
token it falls back to explicit manual instructions while still controlling the
ESP HTTP protocol logger.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_COVER = "cover.garage_door_opener_garage_door"
DEFAULT_LIGHT = "light.garage_door_opener_garage_door_light"
DEFAULT_TARGET_NUMBER = "number.garage_door_opener_garage_door_target_position"
DEFAULT_OBSTRUCTION = "binary_sensor.garage_door_opener_garage_door_obstruction_state"
DEFAULT_VALID_BROADCAST = "binary_sensor.garage_door_opener_garage_door_valid_hcp_broadcast"
DEFAULT_STATE_SENSOR = "sensor.garage_door_opener_garage_door_state"
DEFAULT_RAW_STATUS = "sensor.garage_door_opener_garage_door_raw_hcp_status_hex"
DEFAULT_OPEN_DURATION = "number.garage_door_opener_garage_door_open_duration"
DEFAULT_CLOSE_DURATION = "number.garage_door_opener_garage_door_close_duration"


@dataclass
class HttpResult:
    ok: bool
    status: int | None
    body: bytes
    text: str
    json_data: Any | None
    error: str | None = None


def utc_now_iso() -> str:
    return dt.datetime.now(dt.UTC).replace(microsecond=0).isoformat()


def slugify(text: str) -> str:
    out = []
    for char in text.lower():
        if char.isalnum():
            out.append(char)
        elif out and out[-1] != "-":
            out.append("-")
    return "".join(out).strip("-") or "step"


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def request_url(
    url: str,
    *,
    method: str = "GET",
    headers: dict[str, str] | None = None,
    payload: Any | None = None,
    timeout: float = 10.0,
) -> HttpResult:
    data: bytes | None = None
    request_headers = dict(headers or {})
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        request_headers.setdefault("Content-Type", "application/json")

    request = urllib.request.Request(url, data=data, headers=request_headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read()
            status = response.status
    except urllib.error.HTTPError as err:
        body = err.read()
        status = err.code
        text = body.decode("utf-8", errors="replace")
        return HttpResult(False, status, body, text, decode_json(text), f"HTTP {status}")
    except (urllib.error.URLError, TimeoutError, OSError) as err:
        return HttpResult(False, None, b"", "", None, str(err))

    text = body.decode("utf-8", errors="replace")
    return HttpResult(200 <= status < 300, status, body, text, decode_json(text), None)


def decode_json(text: str) -> Any | None:
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


class EspClient:
    def __init__(self, host: str, port: int) -> None:
        self.base_url = f"http://{host}:{port}"

    def get(self, path: str, *, timeout: float = 10.0, retries: int = 2) -> HttpResult:
        url = self.base_url + path
        last = request_url(url, timeout=timeout)
        for _ in range(retries):
            if last.ok and last.text.strip() != "busy":
                return last
            time.sleep(0.5)
            last = request_url(url, timeout=timeout)
        return last

    def save_endpoint(self, path: str, output: Path, *, timeout: float = 120.0) -> HttpResult:
        result = self.get(path, timeout=timeout, retries=0)
        output.write_bytes(result.body)
        return result


class HomeAssistantClient:
    def __init__(self, base_url: str, token: str | None) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token

    @property
    def available(self) -> bool:
        return bool(self.token)

    def headers(self) -> dict[str, str]:
        if not self.token:
            return {}
        return {"Authorization": f"Bearer {self.token}"}

    def state(self, entity_id: str) -> HttpResult:
        quoted = urllib.parse.quote(entity_id, safe="")
        return request_url(f"{self.base_url}/api/states/{quoted}", headers=self.headers(), timeout=10)

    def call_service(self, domain: str, service: str, payload: dict[str, Any]) -> HttpResult:
        return request_url(
            f"{self.base_url}/api/services/{domain}/{service}",
            method="POST",
            headers=self.headers(),
            payload=payload,
            timeout=20,
        )


class GarageTestWizard:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.esp = EspClient(args.esp_host, args.esp_port)
        self.ha = HomeAssistantClient(args.ha_url, args.ha_token)
        timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.output_dir = Path(args.output_dir) / f"garage-test-{timestamp}"
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.full_open_height_m: float | None = args.full_open_height_m
        self.run: dict[str, Any] = {
            "started_at": utc_now_iso(),
            "config": {
                "esp_host": args.esp_host,
                "esp_port": args.esp_port,
                "ha_url": args.ha_url,
                "ha_api_enabled": self.ha.available,
                "cover_entity": args.cover_entity,
                "light_entity": args.light_entity,
                "target_number_entity": args.target_number_entity,
                "obstruction_entity": args.obstruction_entity,
                "valid_broadcast_entity": args.valid_broadcast_entity,
                "state_sensor_entity": args.state_sensor_entity,
                "raw_status_entity": args.raw_status_entity,
                "open_duration_entity": args.open_duration_entity,
                "close_duration_entity": args.close_duration_entity,
            },
            "events": [],
            "full_open_height_m": self.full_open_height_m,
            "measurements": [],
            "notes": [],
        }

    def print_header(self, title: str) -> None:
        print()
        print("=" * 72)
        print(title)
        print("=" * 72)

    def note(self, message: str) -> None:
        print(message)
        self.run["notes"].append({"at": utc_now_iso(), "message": message})
        self.save_run()

    def prompt_enter(self, message: str = "Press Enter to continue") -> str:
        while True:
            value = input(f"{message} [Enter/q/s]: ").strip()
            if value.lower() in {"q", "quit", "exit"}:
                raise KeyboardInterrupt
            if value.lower() == "s":
                label = input("Snapshot label: ").strip() or "manual-snapshot"
                self.snapshot(label)
                continue
            return value

    def prompt_yes_no(self, message: str, *, default: bool = False) -> bool:
        suffix = "Y/n" if default else "y/N"
        while True:
            value = input(f"{message} [{suffix}]: ").strip().lower()
            if not value:
                return default
            if value in {"y", "yes"}:
                return True
            if value in {"n", "no"}:
                return False
            print("Please answer y or n.")

    def prompt_float(
        self,
        message: str,
        *,
        minimum: float | None = None,
        maximum: float | None = None,
        allow_blank: bool = False,
    ) -> float | None:
        while True:
            raw = input(f"{message}: ").strip().replace(",", ".")
            if allow_blank and raw == "":
                return None
            try:
                value = float(raw)
            except ValueError:
                print("Please enter a number.")
                continue
            if minimum is not None and value < minimum:
                print(f"Value must be at least {minimum}.")
                continue
            if maximum is not None and value > maximum:
                print(f"Value must be at most {maximum}.")
                continue
            return value

    def prompt_percent_list(self, default: list[float]) -> list[float]:
        default_text = ",".join(f"{value:g}" for value in default)
        while True:
            raw = input(f"Target percentages to test [{default_text}]: ").strip()
            if not raw:
                return default
            try:
                values = [float(item.strip().replace(",", ".")) for item in raw.replace(";", ",").split(",")]
            except ValueError:
                print("Use comma-separated numbers, for example: 25,50,75.")
                continue
            if not values or any(value < 0 or value > 100 for value in values):
                print("All percentages must be between 0 and 100.")
                continue
            return values

    def add_event(self, kind: str, label: str, data: dict[str, Any] | None = None) -> None:
        self.run["events"].append({"at": utc_now_iso(), "kind": kind, "label": label, "data": data or {}})
        self.save_run()

    def save_run(self) -> None:
        (self.output_dir / "measurements.json").write_text(json.dumps(self.run, indent=2, sort_keys=True) + "\n")
        self.write_measurements_csv()
        self.write_summary()

    def write_measurements_csv(self) -> None:
        path = self.output_dir / "measurements.csv"
        rows = self.run.get("measurements", [])
        fields = [
            "at",
            "label",
            "kind",
            "direction",
            "target_percent",
            "height_m",
            "actual_percent",
            "error_percent",
            "elapsed_s",
            "notes",
        ]
        with path.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            for row in rows:
                writer.writerow({field: row.get(field, "") for field in fields})

    def write_summary(self) -> None:
        lines = [
            "# Garage Door Test Summary",
            "",
            f"Started: {self.run['started_at']}",
            f"ESP: `{self.args.esp_host}:{self.args.esp_port}`",
            f"Home Assistant API: {'enabled' if self.ha.available else 'manual mode'}",
            "",
        ]
        if self.full_open_height_m is not None:
            lines.append(f"Full open measured height: `{self.full_open_height_m:.3f} m`")
            lines.append("")

        rows = self.run.get("measurements", [])
        if rows:
            lines.extend(
                [
                    "| Step | Direction | Target % | Height m | Actual % | Error % |",
                    "| --- | --- | ---: | ---: | ---: | ---: |",
                ]
            )
            for row in rows:
                lines.append(
                    "| {label} | {direction} | {target} | {height} | {actual} | {error} |".format(
                        label=row.get("label", ""),
                        direction=row.get("direction", ""),
                        target=format_optional(row.get("target_percent")),
                        height=format_optional(row.get("height_m")),
                        actual=format_optional(row.get("actual_percent")),
                        error=format_optional(row.get("error_percent")),
                    )
                )
            lines.append("")
        (self.output_dir / "summary.md").write_text("\n".join(lines) + "\n")

    def snapshot(self, label: str) -> dict[str, Any]:
        data: dict[str, Any] = {
            "label": label,
            "esp": {},
            "home_assistant": {},
        }
        for name, path in {
            "stats": "/stats",
            "broadcast_status": "/broadcast_status",
        }.items():
            result = self.esp.get(path, timeout=8, retries=2)
            data["esp"][name] = result.json_data if result.json_data is not None else result.text.strip()
            if result.error:
                data["esp"][f"{name}_error"] = result.error

        if self.ha.available:
            for entity_id in self.entity_ids_for_snapshot():
                result = self.ha.state(entity_id)
                data["home_assistant"][entity_id] = result.json_data if result.json_data is not None else {
                    "status": result.status,
                    "error": result.error,
                    "text": result.text[:500],
                }

        filename = f"snapshot-{len(self.run['events']):03d}-{slugify(label)}.json"
        (self.output_dir / filename).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
        self.add_event("snapshot", label, {"file": filename, "summary": summarize_snapshot(data)})
        print_snapshot_summary(data)
        return data

    def entity_ids_for_snapshot(self) -> list[str]:
        return [
            self.args.cover_entity,
            self.args.light_entity,
            self.args.target_number_entity,
            self.args.obstruction_entity,
            self.args.valid_broadcast_entity,
            self.args.state_sensor_entity,
            self.args.raw_status_entity,
            self.args.open_duration_entity,
            self.args.close_duration_entity,
        ]

    def start_recording(self) -> None:
        self.print_header("Start ESP Protocol Recording")
        clear = self.esp.get("/persistent_log/clear", timeout=20, retries=2)
        self.add_event("esp", "persistent_log_clear", result_to_event(clear))
        print_result("clear", clear)
        if clear.json_data and clear.json_data.get("format_required"):
            if self.prompt_yes_no("ESP log filesystem needs formatting. Format it now?", default=True):
                formatted = self.esp.get("/persistent_log/format", timeout=120, retries=0)
                self.add_event("esp", "persistent_log_format", result_to_event(formatted))
                print_result("format", formatted)
        start = self.esp.get("/persistent_log/start", timeout=20, retries=2)
        self.add_event("esp", "persistent_log_start", result_to_event(start))
        print_result("start", start)

    def stop_and_download_recording(self) -> None:
        self.print_header("Stop And Download Logs")
        stop = self.esp.get("/persistent_log/stop", timeout=30, retries=2)
        self.add_event("esp", "persistent_log_stop", result_to_event(stop))
        print_result("stop", stop)

        downloads = {
            "esp-stats-final.json": ("/stats", 10),
            "esp-recent-final.json": ("/recent", 20),
            "esp-broadcast-status-final.json": ("/broadcast_status", 10),
            "persistent-log.json": ("/persistent_log", 180),
            "persistent-log.bin": ("/persistent_log.bin", 180),
        }
        for filename, (path, timeout) in downloads.items():
            result = self.esp.save_endpoint(path, self.output_dir / filename, timeout=timeout)
            self.add_event("download", filename, {"endpoint": path, "status": result.status, "error": result.error})
            status = result.status if result.status is not None else "failed"
            print(f"Saved {filename} from {path} ({status}, {len(result.body)} bytes)")

    def preflight(self) -> None:
        self.print_header("Preflight")
        print("Keep the door in sight for every movement. Do not stand under the door.")
        if not self.ha.available:
            print()
            print("Home Assistant API token not set. The wizard will use manual HA instructions.")
            print("To automate HA commands, run with:")
            print("  HA_TOKEN='long-lived-access-token' python3 tools/garage_test_wizard.py")
        print()
        self.snapshot("preflight")
        stats = self.esp.get("/stats", timeout=10, retries=2)
        if stats.json_data:
            if not stats.json_data.get("valid_broadcast"):
                self.note("WARNING: ESP stats do not show valid_broadcast=true.")
            if stats.json_data.get("obstruction"):
                self.note("WARNING: ESP obstruction latch is currently active.")
        self.prompt_enter("Confirm you are physically present at the garage door")

    def command_cover(self, command: str) -> None:
        if not self.ha.available:
            print(f"Manual action: in Home Assistant, run cover.{command}_cover for {self.args.cover_entity}.")
            self.prompt_enter("Press Enter immediately after sending the command")
            self.add_event("manual_command", f"cover.{command}_cover", {"entity_id": self.args.cover_entity})
            return
        result = self.ha.call_service("cover", f"{command}_cover", {"entity_id": self.args.cover_entity})
        self.add_event("ha_command", f"cover.{command}_cover", result_to_event(result))
        print_result(f"cover.{command}_cover", result)
        if not result.ok:
            print(f"Manual fallback: run cover.{command}_cover for {self.args.cover_entity}.")
            self.prompt_enter("Press Enter immediately after sending the command manually")

    def command_position(self, target_percent: float) -> None:
        payload = {"entity_id": self.args.cover_entity, "position": target_percent}
        if self.ha.available:
            result = self.ha.call_service("cover", "set_cover_position", payload)
            self.add_event("ha_command", "cover.set_cover_position", result_to_event(result) | {"target_percent": target_percent})
            print_result(f"cover.set_cover_position {target_percent:g}%", result)
            if result.ok:
                return

            print("Cover position service failed; trying the exposed target-position number entity.")
            fallback = self.ha.call_service(
                "number",
                "set_value",
                {"entity_id": self.args.target_number_entity, "value": target_percent},
            )
            self.add_event("ha_command", "number.set_value", result_to_event(fallback) | {"target_percent": target_percent})
            print_result(f"number.set_value {target_percent:g}%", fallback)
            if fallback.ok:
                return

        print(f"Manual action: set {self.args.target_number_entity} to {target_percent:g}%.")
        self.prompt_enter("Press Enter immediately after setting the target")
        self.add_event("manual_command", "set_target_position", {"target_percent": target_percent})

    def toggle_light(self) -> None:
        if not self.ha.available:
            print(f"Manual action: toggle {self.args.light_entity} in Home Assistant.")
            self.prompt_enter("Press Enter after toggling the light")
            self.add_event("manual_command", "light.toggle", {"entity_id": self.args.light_entity})
            return
        result = self.ha.call_service("light", "toggle", {"entity_id": self.args.light_entity})
        self.add_event("ha_command", "light.toggle", result_to_event(result))
        print_result("light.toggle", result)

    def measure_height(
        self,
        *,
        label: str,
        kind: str,
        direction: str = "",
        target_percent: float | None = None,
        elapsed_s: float | None = None,
        allow_blank: bool = False,
        measured_height_m: float | None = None,
    ) -> None:
        if self.full_open_height_m is None:
            print("Full open height is not known yet.")
            self.full_open_height_m = self.prompt_float("Enter full open clear height in meters", minimum=0.1)
            self.run["full_open_height_m"] = self.full_open_height_m

        max_height = self.full_open_height_m
        height = measured_height_m
        if height is None:
            height = self.prompt_float(
                f"Measured clear opening height for '{label}' in meters",
                minimum=0.0,
                maximum=max_height * 1.1,
                allow_blank=allow_blank,
            )
        if height is None:
            return
        actual_percent = clamp((height / max_height) * 100.0, 0.0, 100.0)
        error_percent = None if target_percent is None else actual_percent - target_percent
        row = {
            "at": utc_now_iso(),
            "label": label,
            "kind": kind,
            "direction": direction,
            "target_percent": target_percent,
            "height_m": height,
            "actual_percent": actual_percent,
            "error_percent": error_percent,
            "elapsed_s": elapsed_s,
            "notes": "",
        }
        self.run["measurements"].append(row)
        self.add_event("measurement", label, row)
        print(
            f"Measured {actual_percent:.1f}% open"
            + (f" ({error_percent:+.1f}% vs target)" if error_percent is not None else "")
        )
        self.save_run()

    def run_full_travel_calibration(self) -> None:
        self.print_header("Full Travel Calibration")
        print("Start with the door fully closed. If needed, close it now and wait for the closed end state.")
        self.prompt_enter("Press Enter when the door is physically fully closed")
        self.snapshot("closed-start")

        self.prompt_enter("Ready to command full open")
        t0 = time.monotonic()
        self.command_cover("open")
        self.prompt_enter("Press Enter when the door is physically fully open")
        elapsed = time.monotonic() - t0
        self.snapshot("fully-open")
        if self.full_open_height_m is None:
            self.full_open_height_m = self.prompt_float("Measure and enter the full open clear height in meters", minimum=0.1)
            self.run["full_open_height_m"] = self.full_open_height_m
        self.measure_height(
            label="full-open",
            kind="endpoint",
            direction="opening",
            target_percent=100.0,
            elapsed_s=elapsed,
            measured_height_m=self.full_open_height_m,
        )

        self.prompt_enter("Ready to command full close")
        t0 = time.monotonic()
        self.command_cover("close")
        self.prompt_enter("Press Enter when the door is physically fully closed")
        elapsed = time.monotonic() - t0
        self.snapshot("fully-closed")
        self.measure_height(label="full-closed", kind="endpoint", direction="closing", target_percent=0.0, elapsed_s=elapsed)

    def run_stop_test(self) -> None:
        self.print_header("Stop Test")
        if not self.prompt_yes_no("Run a stop test from closed toward open?", default=True):
            return
        print("Start from fully closed. The script will command open; press Enter when you want it to send stop.")
        self.prompt_enter("Press Enter when the door is fully closed and you are ready")
        self.command_cover("open")
        self.prompt_enter("Press Enter now to send STOP")
        t0 = time.monotonic()
        self.command_cover("stop")
        self.prompt_enter("Press Enter when the door has physically stopped")
        elapsed = time.monotonic() - t0
        self.snapshot("stopped-midway")
        self.measure_height(label="stop-test", kind="stop", direction="opening", target_percent=None, elapsed_s=elapsed)

        if self.prompt_yes_no("Open fully again before position tests?", default=True):
            self.command_cover("open")
            self.prompt_enter("Press Enter when fully open")
            self.snapshot("after-stop-open")

    def run_position_tests(self) -> None:
        self.print_header("Position Target Tests")
        targets = self.prompt_percent_list([25.0, 50.0, 75.0, 100.0, 50.0, 0.0])
        previous: float | None = None
        for target in targets:
            direction = ""
            if previous is not None:
                direction = "opening" if target > previous else "closing" if target < previous else "same"
            self.prompt_enter(f"Ready to test target {target:g}%")
            t0 = time.monotonic()
            self.command_position(target)
            self.prompt_enter(f"Press Enter when the door has stopped after target {target:g}%")
            elapsed = time.monotonic() - t0
            self.snapshot(f"target-{target:g}-percent")
            self.measure_height(
                label=f"target-{target:g}%",
                kind="position",
                direction=direction,
                target_percent=target,
                elapsed_s=elapsed,
            )
            previous = target

    def run_light_test(self) -> None:
        self.print_header("Light Test")
        if not self.prompt_yes_no("Run light toggle test?", default=True):
            return
        self.toggle_light()
        physical_on = self.prompt_yes_no("Did the physical garage light turn on?", default=True)
        self.add_event("observation", "light-toggle-on", {"physical_on": physical_on})
        self.snapshot("light-after-toggle")
        if self.prompt_yes_no("Toggle the light again to return it to the previous state?", default=True):
            self.toggle_light()
            self.prompt_enter("Press Enter after checking the physical light")
            self.snapshot("light-after-second-toggle")

    def run_obstruction_test(self) -> None:
        self.print_header("Obstruction Test")
        if not self.prompt_yes_no("Run an obstruction test now?", default=False):
            return
        print("Use a safe obstruction test object. Do not use your body.")
        self.prompt_enter("Press Enter when the door is fully open and the obstruction object is ready")
        self.command_cover("close")
        self.prompt_enter("Obstruct the closing door, then press Enter once the opener stops or shows an error")
        display_code = input("Opener display code, if any (blank if none): ").strip()
        print("Waiting 8 seconds for the firmware obstruction latch and HA state to update.")
        time.sleep(8)
        self.snapshot("after-obstruction")
        self.add_event("observation", "obstruction", {"display_code": display_code})
        if self.prompt_yes_no("Command open for recovery?", default=True):
            self.command_cover("open")
            self.prompt_enter("Press Enter when recovery movement has finished")
            self.snapshot("after-obstruction-recovery")

    def finish(self) -> None:
        self.stop_and_download_recording()
        self.run["finished_at"] = utc_now_iso()
        self.save_run()
        print()
        print(f"Test bundle saved in: {self.output_dir}")
        print(f"Summary: {self.output_dir / 'summary.md'}")
        print(f"Measurements JSON: {self.output_dir / 'measurements.json'}")
        print(f"Measurements CSV: {self.output_dir / 'measurements.csv'}")

    def run_all(self) -> int:
        try:
            self.preflight()
            self.start_recording()
            self.run_full_travel_calibration()
            self.run_stop_test()
            self.run_position_tests()
            self.run_light_test()
            self.run_obstruction_test()
            self.finish()
        except KeyboardInterrupt:
            print()
            print("Interrupted. Stopping ESP protocol recording before exit.")
            try:
                self.finish()
            except Exception as err:  # noqa: BLE001
                print(f"Could not finish cleanly: {err}", file=sys.stderr)
                self.save_run()
            return 130
        return 0


def format_optional(value: Any) -> str:
    if value is None or value == "":
        return ""
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value)


def result_to_event(result: HttpResult) -> dict[str, Any]:
    return {
        "ok": result.ok,
        "status": result.status,
        "error": result.error,
        "json": result.json_data,
        "text": result.text[:1000] if result.json_data is None else "",
    }


def print_result(label: str, result: HttpResult) -> None:
    status = result.status if result.status is not None else "failed"
    print(f"{label}: {status}")
    if result.json_data is not None:
        print(json.dumps(result.json_data, indent=2, sort_keys=True)[:2000])
    elif result.text:
        print(result.text[:500])
    if result.error:
        print(f"error: {result.error}")


def summarize_snapshot(data: dict[str, Any]) -> dict[str, Any]:
    esp_stats = data.get("esp", {}).get("stats")
    summary: dict[str, Any] = {}
    if isinstance(esp_stats, dict):
        summary["esp_state"] = esp_stats.get("state")
        summary["valid_broadcast"] = esp_stats.get("valid_broadcast")
        summary["obstruction"] = esp_stats.get("obstruction")
        summary["raw_status"] = esp_stats.get("raw_status")
        summary["unknown_valid_frame_count"] = esp_stats.get("unknown_valid_frame_count")

    ha = data.get("home_assistant", {})
    for entity_id, state in ha.items():
        if isinstance(state, dict) and "state" in state:
            summary[entity_id] = state.get("state")
    return summary


def print_snapshot_summary(data: dict[str, Any]) -> None:
    summary = summarize_snapshot(data)
    if not summary:
        return
    print("Snapshot summary:")
    for key, value in summary.items():
        print(f"  {key}: {value}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Interactive SupraMatic E2 garage-door test wizard")
    parser.add_argument("--esp-host", default=os.environ.get("ESP_HOST", "supramatic-e2.local"))
    parser.add_argument("--esp-port", type=int, default=int(os.environ.get("ESP_PORT", "8080")))
    parser.add_argument("--ha-url", default=os.environ.get("HA_URL", "http://homeassistant.local:8123"))
    parser.add_argument("--ha-token", default=os.environ.get("HA_TOKEN") or os.environ.get("HOME_ASSISTANT_TOKEN"))
    parser.add_argument("--cover-entity", default=DEFAULT_COVER)
    parser.add_argument("--light-entity", default=DEFAULT_LIGHT)
    parser.add_argument("--target-number-entity", default=DEFAULT_TARGET_NUMBER)
    parser.add_argument("--obstruction-entity", default=DEFAULT_OBSTRUCTION)
    parser.add_argument("--valid-broadcast-entity", default=DEFAULT_VALID_BROADCAST)
    parser.add_argument("--state-sensor-entity", default=DEFAULT_STATE_SENSOR)
    parser.add_argument("--raw-status-entity", default=DEFAULT_RAW_STATUS)
    parser.add_argument("--open-duration-entity", default=DEFAULT_OPEN_DURATION)
    parser.add_argument("--close-duration-entity", default=DEFAULT_CLOSE_DURATION)
    parser.add_argument("--full-open-height-m", type=float, help="Known full-open clear height in meters")
    parser.add_argument("--output-dir", default="captures")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    wizard = GarageTestWizard(args)
    return wizard.run_all()


if __name__ == "__main__":
    raise SystemExit(main())
