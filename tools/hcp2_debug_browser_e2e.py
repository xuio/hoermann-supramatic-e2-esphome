from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_CACHE_AGE_MS = 30 * 60 * 1000
DEFAULT_CACHE_BYTES = 100 * 1024 * 1024


def import_sync_playwright():
    try:
        from playwright.sync_api import sync_playwright
    except ModuleNotFoundError as exc:
        raise SystemExit(
            "Playwright is required for this manual HIL tool. Run with "
            "`uv run --with playwright garage-hcp2-debug-browser-e2e ...` "
            "or install the Python playwright package in the active environment."
        ) from exc
    return sync_playwright


def rows_to_dict(text: str) -> dict[str, str]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return {lines[i]: lines[i + 1] for i in range(0, len(lines) - 1, 2)}


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def nonnegative_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def base_url_from_args(args: argparse.Namespace) -> str:
    if args.url:
        return args.url.rstrip("/")
    return f"http://{args.host}:{args.port}"


def fetch_json(base_url: str, path: str, timeout: float) -> dict[str, Any]:
    request = urllib.request.Request(f"{base_url}{path}", headers={"User-Agent": "garage-hcp2-debug-browser-e2e/1"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode())


def fetch_text(base_url: str, path: str, timeout: float) -> str:
    request = urllib.request.Request(f"{base_url}{path}", headers={"User-Agent": "garage-hcp2-debug-browser-e2e/1"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode()


def line_count(page: Any) -> int:
    return page.evaluate(
        "document.querySelector('#log').textContent.split(String.fromCharCode(10)).filter(Boolean).length"
    )


def wait_stream_and_log(page: Any, timeout_ms: int) -> None:
    page.wait_for_function("document.querySelector('#logStream')?.textContent.includes('stream connected')", timeout=timeout_ms)
    page.wait_for_function("(document.querySelector('#log')?.textContent || '').length > 1000", timeout=timeout_ms)


def assert_zero_bus_faults(counters: dict[str, str], stats_rows: dict[str, str]) -> None:
    for key in ("missed polls", "tx aborts", "collisions"):
        value = counters.get(key) or stats_rows.get(key)
        if value not in ("0", 0):
            raise AssertionError(f"{key} is {value}, expected 0")


def int_row(counters: dict[str, str], stats_rows: dict[str, str], key: str) -> int:
    value = counters.get(key) or stats_rows.get(key)
    if value is None:
        raise AssertionError(f"missing UI row: {key}")
    return int(value)


def launch_browser(playwright: Any, args: argparse.Namespace) -> Any:
    launch_kwargs: dict[str, Any] = {"headless": not args.headful}
    if args.browser_executable:
        launch_kwargs["executable_path"] = str(args.browser_executable)
    elif args.browser_channel:
        launch_kwargs["channel"] = args.browser_channel
    return playwright.chromium.launch(**launch_kwargs)


def write_jsonl(path: Path | None, record: dict[str, Any]) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as file:
        file.write(json.dumps(record, sort_keys=True) + "\n")
        file.flush()


def run_browser_e2e(args: argparse.Namespace) -> dict[str, Any]:
    sync_playwright = import_sync_playwright()
    base_url = base_url_from_args(args)
    timeout_ms = int(args.timeout * 1000)
    progress_output: Path | None = args.progress_output

    if args.start_log:
        fetch_json(base_url, "/hcp2_log/start", args.http_timeout)

    with sync_playwright() as playwright:
        browser = launch_browser(playwright, args)
        context = browser.new_context(
            viewport={"width": args.viewport_width, "height": args.viewport_height},
            device_scale_factor=1,
            accept_downloads=True,
        )
        page = context.new_page()
        console: list[dict[str, str]] = []
        page_errors: list[dict[str, str]] = []
        ws_frames = {"page1": 0, "page2": 0}
        responses: list[dict[str, Any]] = []
        page2 = None

        page.on("console", lambda msg: console.append({"page": "page1", "type": msg.type, "text": msg.text}))
        page.on("pageerror", lambda exc: page_errors.append({"page": "page1", "error": str(exc)}))
        page.on(
            "websocket",
            lambda ws: ws.on("framereceived", lambda payload: ws_frames.__setitem__("page1", ws_frames["page1"] + 1)),
        )
        page.on(
            "response",
            lambda response: responses.append({"page": "page1", "url": response.url, "status": response.status})
            if any(path in response.url for path in ("/health", "/stats", "/hcp2_log"))
            else None,
        )

        try:
            page.goto(base_url + "/", wait_until="domcontentloaded", timeout=timeout_ms)
            page.wait_for_selector("#verdict", timeout=timeout_ms)
            wait_stream_and_log(page, timeout_ms)
            time.sleep(args.settle_seconds)

            verdict_text = page.locator("#verdict").inner_text(timeout=timeout_ms).strip()
            log_text = page.locator("#log").inner_text(timeout=timeout_ms)
            if "ok" not in verdict_text.lower():
                raise AssertionError(f"verdict is not ok: {verdict_text!r}")
            if '"type":"protocol"' not in log_text:
                raise AssertionError("protocol JSON entries missing from live log")
            if "status_poll" not in log_text and "broadcast_status" not in log_text:
                raise AssertionError("packet frame names missing from live log")

            counters1 = rows_to_dict(page.locator("#counters").inner_text(timeout=timeout_ms))
            stats1 = rows_to_dict(page.locator("#statsPanel").inner_text(timeout=timeout_ms))
            assert_zero_bus_faults(counters1, stats1)
            polls1 = int_row(counters1, stats1, "polls seen")
            lines1 = line_count(page)

            time.sleep(args.live_wait_seconds)
            page.wait_for_function(
                f"document.querySelector('#log').textContent.split(String.fromCharCode(10)).filter(Boolean).length > {lines1}",
                timeout=timeout_ms,
            )
            counters2 = rows_to_dict(page.locator("#counters").inner_text(timeout=timeout_ms))
            stats2 = rows_to_dict(page.locator("#statsPanel").inner_text(timeout=timeout_ms))
            assert_zero_bus_faults(counters2, stats2)
            polls2 = int_row(counters2, stats2, "polls seen")
            lines2 = line_count(page)
            if polls2 <= polls1:
                raise AssertionError(f"polls seen did not increase in UI: {polls1} -> {polls2}")

            page2 = context.new_page()
            page2.on("console", lambda msg: console.append({"page": "page2", "type": msg.type, "text": msg.text}))
            page2.on("pageerror", lambda exc: page_errors.append({"page": "page2", "error": str(exc)}))
            page2.on(
                "websocket",
                lambda ws: ws.on("framereceived", lambda payload: ws_frames.__setitem__("page2", ws_frames["page2"] + 1)),
            )
            page2.on(
                "response",
                lambda response: responses.append({"page": "page2", "url": response.url, "status": response.status})
                if any(path in response.url for path in ("/health", "/stats", "/hcp2_log"))
                else None,
            )
            page2.goto(base_url + "/", wait_until="domcontentloaded", timeout=timeout_ms)
            time.sleep(args.duplicate_tab_seconds)
            duplicate_stream_text = page2.locator("#logStream").inner_text(timeout=timeout_ms)
            if "stream connected" not in page.locator("#logStream").inner_text(timeout=timeout_ms):
                raise AssertionError("primary stream was displaced by duplicate tab")
            lines3 = line_count(page)
            if lines3 <= lines2:
                raise AssertionError(f"primary stream stopped after duplicate tab: {lines2} -> {lines3}")
            page2.close()
            page2 = None

            page.evaluate("const el=document.querySelector('#log'); el.scrollTop=0")
            scroll_top_before = page.evaluate("document.querySelector('#log').scrollTop")
            scroll_height_before = page.evaluate("document.querySelector('#log').scrollHeight")
            time.sleep(args.scroll_wait_seconds)
            scroll_top_after = page.evaluate("document.querySelector('#log').scrollTop")
            scroll_height_after = page.evaluate("document.querySelector('#log').scrollHeight")
            if scroll_top_before != 0 or scroll_top_after > 4:
                raise AssertionError(f"scrollback moved while user was scrolled up: {scroll_top_before} -> {scroll_top_after}")
            if scroll_height_after <= scroll_height_before:
                raise AssertionError(
                    f"scrollback did not grow while user was scrolled up: {scroll_height_before} -> {scroll_height_after}"
                )

            before_reconnect = line_count(page)
            page.click("text=Reconnect Stream", timeout=timeout_ms)
            wait_stream_and_log(page, timeout_ms)
            time.sleep(args.reconnect_wait_seconds)
            page.wait_for_function(
                f"document.querySelector('#log').textContent.split(String.fromCharCode(10)).filter(Boolean).length > {before_reconnect}",
                timeout=timeout_ms,
            )

            soak_started = time.monotonic()
            soak_checkpoints: list[dict[str, Any]] = []
            last_soak_lines = line_count(page)
            while time.monotonic() - soak_started < args.soak_seconds:
                time.sleep(min(args.soak_checkpoint_seconds, args.soak_seconds - (time.monotonic() - soak_started)))
                counters = rows_to_dict(page.locator("#counters").inner_text(timeout=timeout_ms))
                stats_rows = rows_to_dict(page.locator("#statsPanel").inner_text(timeout=timeout_ms))
                assert_zero_bus_faults(counters, stats_rows)
                current_lines = line_count(page)
                current_polls = int_row(counters, stats_rows, "polls seen")
                if "stream connected" not in page.locator("#logStream").inner_text(timeout=timeout_ms):
                    raise AssertionError("stream disconnected during soak")
                if current_lines <= last_soak_lines:
                    raise AssertionError(f"live log stopped growing during soak: {last_soak_lines} -> {current_lines}")
                device_stats = fetch_json(base_url, "/stats", args.http_timeout)
                websocket_stats = device_stats.get("websocket", {})
                if websocket_stats.get("write_failures") not in (0, None):
                    raise AssertionError(f"websocket write failures during soak: {websocket_stats}")
                checkpoint = {
                    "event": "soak_progress",
                    "elapsed_s": round(time.monotonic() - soak_started, 3),
                    "polls_seen": current_polls,
                    "log_lines": current_lines,
                    "missed_polls": counters.get("missed polls") or stats_rows.get("missed polls"),
                    "tx_aborts": counters.get("tx aborts") or stats_rows.get("tx aborts"),
                    "collisions": counters.get("collisions") or stats_rows.get("collisions"),
                    "websocket": websocket_stats,
                }
                soak_checkpoints.append(checkpoint)
                write_jsonl(progress_output, checkpoint)
                last_soak_lines = current_lines

            with page.expect_download(timeout=timeout_ms) as download_info:
                page.click("text=Download JSON", timeout=timeout_ms)
            download = download_info.value
            download_payload = json.loads(Path(download.path()).read_text(encoding="utf-8"))
            cache_info = download_payload.get("cache", {})
            if download_payload.get("format") != "hcp2-debug-log-cache-v1":
                raise AssertionError("cached download has wrong format")
            if cache_info.get("max_age_ms") != args.expect_cache_age_ms:
                raise AssertionError(f"cached download max_age_ms is {cache_info.get('max_age_ms')}")
            if cache_info.get("max_bytes") != args.expect_cache_bytes:
                raise AssertionError(f"cached download max_bytes is {cache_info.get('max_bytes')}")
            if not download_payload.get("records"):
                raise AssertionError("cached download has no records")

            if args.screenshot:
                args.screenshot.parent.mkdir(parents=True, exist_ok=True)
                page.screenshot(path=str(args.screenshot), full_page=True)

            final_counters = rows_to_dict(page.locator("#counters").inner_text(timeout=timeout_ms))
            final_stats_rows = rows_to_dict(page.locator("#statsPanel").inner_text(timeout=timeout_ms))
            assert_zero_bus_faults(final_counters, final_stats_rows)
            page1_errors = [msg for msg in console if msg["page"] == "page1" and msg["type"] == "error"]
            page1_warnings = [msg for msg in console if msg["page"] == "page1" and msg["type"] == "warning"]
            page1_page_errors = [entry for entry in page_errors if entry["page"] == "page1"]
            if page1_errors and not args.allow_console_errors:
                raise AssertionError(f"main page console errors: {page1_errors}")
            if page1_page_errors:
                raise AssertionError(f"main page errors: {page1_page_errors}")

            report = {
                "verdict": "ok",
                "base_url": base_url,
                "browser": {
                    "channel": args.browser_channel,
                    "executable": str(args.browser_executable) if args.browser_executable else None,
                    "headless": not args.headful,
                },
                "ui": {
                    "verdict": page.locator("#verdict").inner_text(timeout=timeout_ms).strip(),
                    "stream": page.locator("#logStream").inner_text(timeout=timeout_ms).strip(),
                    "duplicate_tab_stream": duplicate_stream_text,
                    "polls_seen": [polls1, polls2, int_row(final_counters, final_stats_rows, "polls seen")],
                    "missed_polls": final_counters.get("missed polls") or final_stats_rows.get("missed polls"),
                    "raw_missed_polls": final_counters.get("raw missed polls") or final_stats_rows.get("raw missed polls"),
                    "tx_aborts": final_counters.get("tx aborts") or final_stats_rows.get("tx aborts"),
                    "collisions": final_counters.get("collisions") or final_stats_rows.get("collisions"),
                    "log_lines": {"initial": lines1, "after_wait": lines2, "after_duplicate_tab": lines3, "final": line_count(page)},
                    "scrollback": {
                        "top_after": scroll_top_after,
                        "height_before": scroll_height_before,
                        "height_after": scroll_height_after,
                    },
                    "websocket_frames": ws_frames,
                },
                "download": {
                    "suggested_filename": download.suggested_filename,
                    "records": len(download_payload.get("records", [])),
                    "cache": cache_info,
                },
                "soak": {
                    "requested_s": args.soak_seconds,
                    "checkpoints": soak_checkpoints,
                },
                "console": {
                    "page1_errors": page1_errors,
                    "page1_warnings": page1_warnings,
                },
                "page_errors": page_errors,
                "responses_tail": responses[-30:],
                "screenshot": str(args.screenshot) if args.screenshot else None,
            }
        finally:
            if page2 is not None and not page2.is_closed():
                page2.close()
            context.close()
            browser.close()

    return report


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the HCP2 debug web UI browser E2E/HIL check")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--host", help="ESP hostname or IP address")
    target.add_argument("--url", help="Full ESP debug base URL")
    parser.add_argument("--port", type=int, default=80, help="ESP debug HTTP port when --host is used")
    parser.add_argument("--timeout", type=nonnegative_float, default=30.0, help="Browser step timeout in seconds")
    parser.add_argument("--http-timeout", type=nonnegative_float, default=10.0, help="HTTP helper timeout in seconds")
    parser.add_argument("--browser-channel", default=None, help="Playwright Chromium channel, e.g. chrome")
    parser.add_argument("--browser-executable", type=Path, help="Chromium/Chrome executable path")
    parser.add_argument("--headful", action="store_true", help="Run browser with a visible window")
    parser.add_argument("--viewport-width", type=positive_int, default=1440)
    parser.add_argument("--viewport-height", type=positive_int, default=1000)
    parser.add_argument("--settle-seconds", type=nonnegative_float, default=2.0)
    parser.add_argument("--live-wait-seconds", type=nonnegative_float, default=4.0)
    parser.add_argument("--duplicate-tab-seconds", type=nonnegative_float, default=3.0)
    parser.add_argument("--scroll-wait-seconds", type=nonnegative_float, default=3.0)
    parser.add_argument("--reconnect-wait-seconds", type=nonnegative_float, default=2.0)
    parser.add_argument("--soak-seconds", type=nonnegative_float, default=0.0, help="Keep the page streaming after E2E")
    parser.add_argument("--soak-checkpoint-seconds", type=nonnegative_float, default=60.0)
    parser.add_argument("--progress-output", type=Path, help="Optional soak progress JSONL path")
    parser.add_argument("--report", type=Path, help="Optional JSON report path")
    parser.add_argument("--screenshot", type=Path, help="Optional browser screenshot path")
    parser.add_argument("--expect-cache-age-ms", type=positive_int, default=DEFAULT_CACHE_AGE_MS)
    parser.add_argument("--expect-cache-bytes", type=positive_int, default=DEFAULT_CACHE_BYTES)
    parser.add_argument("--no-start-log", dest="start_log", action="store_false", help="Do not call /hcp2_log/start first")
    parser.set_defaults(start_log=True)
    parser.add_argument("--allow-console-errors", action="store_true", help="Do not fail on main-page console errors")
    parser.add_argument("--json", action="store_true", help="Print full JSON report")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    try:
        report = run_browser_e2e(args)
    except (AssertionError, urllib.error.URLError, OSError) as exc:
        print(f"garage-hcp2-debug-browser-e2e failed: {exc}", file=sys.stderr)
        return 1

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if args.json or not args.report:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(args.report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
