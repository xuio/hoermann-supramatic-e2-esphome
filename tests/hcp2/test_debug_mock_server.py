"""Contract tests for the local HCP2 debug mock server.

These run headlessly in CI (stdlib only) and gate the debug-UI verification
harness. The full Playwright browser E2E is included but skips automatically
when playwright (or its browser) is unavailable.
"""

from __future__ import annotations

import base64
import json
import os
import socket
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Iterator

import pytest

from tools.hcp2_debug_mock_server import (
    canonical_records,
    build_server,
    extract_debug_html,
)

ALL_RECORD_TYPES = {"control", "protocol", "state", "command", "lp_trace", "lp_trace_overflow"}


def test_html_extraction_is_a_complete_document() -> None:
    html = extract_debug_html()
    stripped = html.strip()
    assert stripped.startswith("<!doctype html>")
    assert stripped.lower().endswith("</html>")
    # Load-bearing hooks the E2E and firmware contract depend on.
    for marker in ('<pre id="log">', "Reconnect Stream", "Download JSON", "HP core controls", 'id="verdict"'):
        assert marker in html, marker


def test_canonical_fixture_covers_all_record_types_and_faults() -> None:
    recs = canonical_records()
    types = {r["type"] for r in recs}
    assert ALL_RECORD_TYPES <= types
    events = {r.get("event") for r in recs if r["type"] == "protocol"}
    assert {"rx", "tx", "bad_crc", "rx_error"} <= events
    # A genuine reply is scheduled ~4200us after its poll.
    polls = [r for r in recs if r.get("frame") == "status_poll" and r.get("event") == "rx"]
    txs = [r for r in recs if r.get("event") == "tx"]
    assert polls and txs
    delay = txs[0]["event_us"] - polls[0]["event_us"]
    assert 4000 <= delay <= 4400, delay


@pytest.fixture()
def mock_server() -> Iterator[str]:
    server = build_server("127.0.0.1", 0, "nominal")
    server.mock_state.started = time.monotonic() - 120  # HP reset controls are available after boot validation.
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def _request(base: str, path: str, *, method: str = "GET") -> tuple[int, str]:
    try:
        request = urllib.request.Request(base + path, method=method)
        with urllib.request.urlopen(request, timeout=5) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as exc:  # 503 etc. still carry a body
        return exc.code, exc.read().decode()


def _get(base: str, path: str) -> tuple[int, str]:
    return _request(base, path)


def test_json_routes(mock_server: str) -> None:
    status, body = _get(mock_server, "/health")
    health = json.loads(body)
    assert status == 200 and health["verdict"] == "ok"
    assert {"checks", "stats", "door", "lp", "hp"} <= set(health)
    assert "warnings" in health
    assert health["checks"]["diagnostic_clear_ms"] == 3000
    assert health["lp"]["max_response_schedule_to_tx_start_us"] >= 0
    assert health["lp"]["max_response_tx_us"] >= 0

    _, stats_body = _get(mock_server, "/stats")
    stats = json.loads(stats_body)
    assert stats["protocol"] == "hcp2"
    assert {"websocket", "protocol_log"} <= set(stats)

    status, _ = _get(mock_server, "/support")
    assert status == 200
    status, _ = _get(mock_server, "/hcp2_log/start")
    assert status == 200


def test_hp_control_routes_are_post_only(mock_server: str) -> None:
    status, _ = _get(mock_server, "/control/hp/restart")
    assert status == 405
    for action in ("restart", "cpu_reset", "panic"):
        status, body = _request(mock_server, f"/control/hp/{action}", method="POST")
        payload = json.loads(body)
        assert status == 202
        assert payload["ok"] is True
        assert payload["action"] == action
        assert payload["lp_expected"] == "running"


def test_hp_control_routes_wait_for_boot_validation() -> None:
    server = build_server("127.0.0.1", 0, "nominal")
    server.mock_state.started = time.monotonic() - 30
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{port}"
    try:
        status, body = _request(base, "/control/hp/restart", method="POST")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    payload = json.loads(body)
    assert status == 409
    assert payload["error"] == "boot validation pending"
    assert 0 < payload["retry_after_ms"] <= 40_500


def _ws_connect(host: str, port: int, path: str = "/hcp2_log/ws") -> tuple[socket.socket, str]:
    sock = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(1)
        if not chunk:
            break
        buf += chunk
    status_line = buf.split(b"\r\n", 1)[0].decode()
    return sock, status_line


def _ws_read_text(sock: socket.socket) -> str | None:
    b0 = sock.recv(1)
    if not b0:
        return None
    length = sock.recv(1)[0] & 0x7F
    if length == 126:
        length = int.from_bytes(sock.recv(2), "big")
    elif length == 127:
        length = int.from_bytes(sock.recv(8), "big")
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            break
        data += chunk
    return data.decode("utf-8", "replace")


def test_websocket_push_contract(mock_server: str) -> None:
    host, port = "127.0.0.1", int(mock_server.rsplit(":", 1)[1])
    sock, status = _ws_connect(host, port)
    assert "101" in status

    sock.settimeout(4)
    types: set[str] = set()
    saw_protocol_poll = False
    deadline = time.time() + 4
    while time.time() < deadline and not (types >= {"health", "log"} and saw_protocol_poll):
        frame = _ws_read_text(sock)
        if frame is None:
            break
        msg = json.loads(frame)
        types.add(msg.get("type"))
        if msg.get("type") == "log":
            text = msg.get("text", "")
            if '"type":"protocol"' in text and "status_poll" in text:
                saw_protocol_poll = True
    assert {"health", "log"} <= types
    assert saw_protocol_poll

    # Duplicate connection without replace -> 409, primary survives.
    dup, dup_status = _ws_connect(host, port)
    assert "409" in dup_status
    dup.close()
    assert _ws_read_text(sock) is not None

    # replace=1 takes over; the old socket is closed.
    repl, repl_status = _ws_connect(host, port, "/hcp2_log/ws?replace=1")
    assert "101" in repl_status
    sock.settimeout(3)
    try:
        assert _ws_read_text(sock) is None
    except OSError:
        pass
    assert _ws_read_text(repl) is not None
    repl.close()
    sock.close()


def test_browser_e2e_against_mock(mock_server: str, tmp_path) -> None:
    pytest.importorskip("playwright", reason="playwright not installed")
    from tools.hcp2_debug_browser_e2e import parse_args, run_browser_e2e

    args = parse_args(
        [
            "--url",
            mock_server,
            "--settle-seconds",
            "1",
            "--live-wait-seconds",
            "2",
            "--duplicate-tab-seconds",
            "2",
            "--scroll-wait-seconds",
            "2",
            "--reconnect-wait-seconds",
            "1",
            "--timeout",
            "30",
        ]
    )
    try:
        report = run_browser_e2e(args)
    except Exception as exc:  # noqa: BLE001 - playwright browser may be absent in CI
        if "Executable doesn't exist" in str(exc) or "playwright install" in str(exc):
            pytest.skip(f"playwright browser unavailable: {exc}")
        raise
    assert report["verdict"] == "ok"
    assert "ok" in report["ui"]["verdict"].lower()
