"""Local mock of the HCP2 bridge debug HTTP/WebSocket server.

Serves the *real* embedded debug UI (sliced out of
``components/hcp2bridge/hcp2bridge_http_debug.cpp`` at runtime) plus synthetic
``/health`` / ``/stats`` / ``/support`` / ``/hcp2_log*`` responses and a
push-only, single-client WebSocket that mirrors the firmware contract
(``type:health`` ~500 ms, ``type:log`` ~250 ms, chunked text frames, 409 on a
duplicate connection, ``?replace=1`` takeover).

The point is to iterate on and regression-test the debug UI without flashing an
ESP32: point a browser or ``tools/hcp2_debug_browser_e2e.py`` at this server.

Stdlib only (``http.server`` + a hand-rolled RFC 6455 text-frame writer).
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEBUG_CPP = REPO_ROOT / "components" / "hcp2bridge" / "hcp2bridge_http_debug.cpp"
RESPONSE_DELAY_US = 4200  # HCP2_DEFAULT_RESPONSE_DELAY_US

# Frame-type / event names exactly as the firmware emits them.
FRAME_STATUS_POLL = "status_poll"
FRAME_BROADCAST = "broadcast_status"
FRAME_COMMAND_ARG = "command_arg"

SCENARIOS = (
    "nominal",
    "missed_polls",
    "bad_crc",
    "collisions",
    "tx_abort",
    "stale",
    "disconnect",
    "hp_fallback",
    "soak",
)


def extract_debug_html(cpp_path: Path = DEBUG_CPP) -> str:
    """Slice the embedded debug page out of the C++ raw string literal.

    Matches ``R"HTML(`` ... ``)HTML"`` precisely (full close delimiter, not the
    first ``)HTML``) and asserts the result is a complete document, so a future
    literal that happens to embed the delimiter fails loudly instead of serving
    truncated UI.
    """
    text = cpp_path.read_text(encoding="utf-8")
    open_marker = 'R"HTML('
    start = text.find(open_marker)
    if start < 0:
        raise RuntimeError(f"no R\"HTML(...) literal found in {cpp_path}")
    body_start = start + len(open_marker)
    close_marker = ')HTML"'
    end = text.find(close_marker, body_start)
    if end < 0:
        raise RuntimeError(f"unterminated R\"HTML( literal in {cpp_path}")
    html = text[body_start:end]
    stripped = html.strip()
    if not stripped.lower().endswith("</html>"):
        raise RuntimeError(
            "extracted HTML does not end with </html>; the raw-string slice is "
            "likely truncated (does the page embed the )HTML\" delimiter?)"
        )
    return html


def _hex(data: bytes) -> str:
    return data.hex().upper()


class MockState:
    """Deterministic generator for synthetic device state + protocol records."""

    def __init__(self, scenario: str) -> None:
        self.scenario = scenario
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.seq = 1
        self.event_us = 1_000_000
        self.polls_seen = 0
        self.polls_answered = 0
        self.lp_heartbeat = 0
        self.position = 0.0
        self.target = 0.0
        self.state_name = "closed"
        self.state_raw = 0
        self.light = False
        self.obstruction = False
        self.log_enabled = scenario != "off"
        self.recent: list[str] = []
        self.recent_cap = 600
        # Fault counters surfaced when the scenario calls for them.
        self.missed_polls = 0
        self.tx_aborts = 0
        self.collisions = 0
        self.crc_errors = 0
        self.rx_errors = 0
        self.lp_resets = 0
        self._cycle = 0
        self._jitter_seed = 0

    # -- record helpers -----------------------------------------------------

    def _next_seq(self) -> int:
        s = self.seq
        self.seq += 1
        return s

    def _now_ms(self) -> int:
        return int((time.monotonic() - self.started) * 1000) + 1

    def _record(self, **fields: Any) -> dict[str, Any]:
        rec = {"seq": self._next_seq(), "ms": self._now_ms()}
        rec.update(fields)
        return rec

    def _push(self, recs: list[dict[str, Any]], rec: dict[str, Any]) -> None:
        recs.append(rec)

    # -- scenario clock -----------------------------------------------------

    def generate_batch(self) -> list[str]:
        """Advance the simulated bus by a few poll cycles, returning NDJSON lines."""
        if not self.log_enabled:
            return []
        recs: list[dict[str, Any]] = []
        with self.lock:
            elapsed = time.monotonic() - self.started
            source = "hp" if self.scenario == "hp_fallback" else "lp"
            cycles = 6 if self.scenario == "soak" else 4
            for _ in range(cycles):
                self._cycle += 1
                self.event_us = (self.event_us + 70_000) & 0xFFFFFFFF
                self.lp_heartbeat += 1
                self.polls_seen += 1
                poll_us = self.event_us
                # Status poll from the motor (master -> slave 2).
                self._push(recs, self._record(
                    type="protocol", source=source, event_us=poll_us,
                    event="rx", frame=FRAME_STATUS_POLL, len=5,
                    hex="0217" + _hex(struct.pack("<H", self._cycle & 0xFFFF)) + "00",
                ))

                drop = self.scenario == "missed_polls" and (self._cycle % 7 in (0, 1, 2))
                bad = self.scenario == "bad_crc" and (self._cycle % 5 == 0)
                if self.scenario == "collisions" and (self._cycle % 11 == 0):
                    self.collisions += 1
                    self._push(recs, self._record(
                        type="lp_trace", event_us=poll_us + 120, event="collision",
                        event_id=9, value=self.collisions,
                    ))
                if self.scenario == "tx_abort" and (self._cycle % 13 == 0):
                    self.tx_aborts += 1
                    self._push(recs, self._record(
                        type="lp_trace", event_us=poll_us + 80, event="tx_abort",
                        event_id=8, value=self.tx_aborts,
                    ))

                if bad:
                    self.crc_errors += 1
                    self._push(recs, self._record(
                        type="protocol", source=source, event_us=poll_us + 1500,
                        event="bad_crc", frame=FRAME_BROADCAST, len=9,
                        hex="02170A0000020504DEAD",
                    ))
                elif drop:
                    self.missed_polls += 1
                else:
                    # Genuine reply, scheduled at poll + 4200us + jitter.
                    self._jitter_seed = (self._jitter_seed * 1103515245 + 12345) & 0x7FFFFFFF
                    jitter = (self._jitter_seed % 220) - 60  # -60..+159 us
                    tx_us = (poll_us + RESPONSE_DELAY_US + jitter) & 0xFFFFFFFF
                    self.polls_answered += 1
                    self._push(recs, self._record(
                        type="protocol", source=source, event_us=tx_us,
                        event="tx", frame=FRAME_BROADCAST, len=14,
                        hex="0217" + _hex(struct.pack("<H", int(self.position * 200))) + "059C4100030600",
                    ))

            # Occasional decoded state record.
            if self._cycle % 8 == 0:
                self._push(recs, self._record(
                    type="state", source="broadcast",
                    target_position=int(self.target * 200),
                    current_position=int(self.position * 200),
                    state=self.state_name, state_raw=self.state_raw,
                    light=self.light, bus_online=True, obstruction=self.obstruction,
                ))
            # A command + lp_trace breadcrumb every so often so all record types show up.
            if self._cycle % 40 == 0:
                self._push(recs, self._record(
                    type="command", phase="execute", button="open", ok=True,
                    reason="lp_mailbox",
                ))
                self._push(recs, self._record(
                    type="lp_trace", event_us=self.event_us + 10, event="command",
                    event_id=2, value=1,
                ))

            lines = [json.dumps(r, separators=(",", ":")) for r in recs]
            self.recent.extend(lines)
            if len(self.recent) > self.recent_cap:
                self.recent = self.recent[-self.recent_cap:]
            self._elapsed = elapsed
        return lines

    # -- snapshots ----------------------------------------------------------

    def recent_ndjson(self) -> str:
        with self.lock:
            return ("\n".join(self.recent) + "\n") if self.recent else ""

    def _reasons(self) -> list[str]:
        reasons: list[str] = []
        if self.scenario == "hp_fallback":
            reasons.append("hp_fallback_enabled")
        if self.missed_polls:
            reasons.append("missed_polls")
        if self.tx_aborts:
            reasons.append("tx_aborts")
        if self.collisions:
            reasons.append("collisions")
        if self.crc_errors:
            reasons.append("lp_health_flags")
        return reasons

    def stats(self) -> dict[str, Any]:
        with self.lock:
            mode = "hp_fallback" if self.scenario == "hp_fallback" else "lp"
            return {
                "protocol": "hcp2",
                "mode": mode,
                "uptime_ms": self._now_ms(),
                "bus_online": True,
                "valid_broadcast": True,
                "state": self.state_name,
                "position": round(self.position, 3),
                "polls_seen": self.polls_seen,
                "polls_answered": self.polls_answered,
                "missed_polls": self.missed_polls,
                "raw_missed_polls": self.missed_polls,
                "pending_response": False,
                "crc_errors": self.crc_errors,
                "rx_errors": self.rx_errors,
                "tx_aborts": self.tx_aborts,
                "collisions": self.collisions,
                "lp_heartbeat": self.lp_heartbeat,
                "lp_resets": self.lp_resets,
                "hp_resets": 0,
                "build": "mock dev " + ("2026-06-15"),
                "command_sequence": 1,
                "last_command": "open",
                "last_command_age_ms": 3200,
                "protocol_log": {
                    "enabled": self.log_enabled,
                    "used": len("\n".join(self.recent)),
                    "capacity": 49152,
                    "overwritten_records": 0,
                    "overwritten_bytes": 0,
                    "dropped_records": 0,
                    "dropped_bytes": 0,
                    "next_seq": self.seq,
                    "storage": "ram",
                    "mode": "ring",
                    "flash_writes": False,
                    "ready": True,
                },
                "websocket": {
                    "connected": True,
                    "connects": 1,
                    "disconnects": 0,
                    "rejects": 0,
                    "peer_closes": 0,
                    "read_failures": 0,
                    "write_failures": 0,
                    "last_errno": 0,
                    "last_close_reason": "none",
                },
            }

    def door(self) -> dict[str, Any]:
        with self.lock:
            return {
                "target_position_raw": int(self.target * 200),
                "current_position_raw": int(self.position * 200),
                "state_raw": self.state_raw,
                "state": self.state_name,
                "light": self.light,
                "obstruction": self.obstruction,
            }

    def lp(self) -> dict[str, Any]:
        with self.lock:
            return {
                "health_flags": 1 if self.crc_errors else 0,
                "max_rx_fifo": 14,
                "max_loop_us": 180,
                "max_poll_rx_to_schedule_us": 90,
                "max_response_schedule_to_tx_start_us": 160,
                "max_response_tx_us": 1790,
                "max_de_hold_us": 6200,
                "last_poll_age_ms": 12,
                "loop_overruns": 0,
                "rx_starvations": 0,
                "stuck_de_recoveries": 0,
                "mailbox_repairs": 0,
            }

    def hp(self) -> dict[str, Any]:
        return {"resets": 0, "panic_resets": 0, "wdt_resets": 0, "brownout_resets": 0}

    def health(self) -> dict[str, Any]:
        reasons = self._reasons()
        ok = not reasons
        stats = self.stats()
        checks = {
            "lp_mode": self.scenario != "hp_fallback",
            "lp_seen": True,
            "bus_online": True,
            "valid_broadcast": True,
            "last_poll_age_ms": 12,
            "polls_seen": stats["polls_seen"],
            "polls_answered": stats["polls_answered"],
            "missed_polls": stats["missed_polls"],
            "raw_missed_polls": stats["raw_missed_polls"],
            "pending_response": False,
            "health_flags": 1 if self.crc_errors else 0,
            "tx_aborts": stats["tx_aborts"],
            "collisions": stats["collisions"],
            "loop_overruns": 0,
            "rx_starvations": 0,
            "stuck_de_recoveries": 0,
            "max_de_hold_us": 6200,
        }
        return {
            "verdict": "ok" if ok else "fail",
            "safe_for_ota_restart": ok,
            "reasons": reasons,
            "checks": checks,
            "stats": stats,
            "door": self.door(),
            "lp": self.lp(),
            "hp": self.hp(),
        }

    def support(self) -> dict[str, Any]:
        return {
            "device": "hcp2bridge",
            "target": "supramatic_series_4",
            "stats": self.stats(),
            "health": self.health(),
            "door": self.door(),
            "lp": self.lp(),
            "hp": self.hp(),
        }


class WsClient:
    def __init__(self, conn: socket.socket) -> None:
        self.conn = conn
        self.stop = threading.Event()


def ws_text_frame(payload: str) -> bytes:
    data = payload.encode("utf-8")
    header = bytearray([0x81])
    n = len(data)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header += bytes([126, (n >> 8) & 0xFF, n & 0xFF])
    else:
        header += bytes([127]) + n.to_bytes(8, "big")
    return bytes(header) + data


class DebugHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "hcp2-debug-mock/1"

    # Silence default per-request logging; keep it quiet for E2E runs.
    def log_message(self, *args: Any) -> None:  # noqa: D401
        return

    @property
    def state(self) -> MockState:
        return self.server.mock_state  # type: ignore[attr-defined]

    def _send_json(self, obj: Any, status: int = 200) -> None:
        body = json.dumps(obj).encode("utf-8")
        self._send_bytes(body, status, "application/json")

    def _send_text(self, text: str, status: int = 200, content_type: str = "text/plain; charset=utf-8") -> None:
        self._send_bytes(text.encode("utf-8"), status, content_type)

    def _send_bytes(self, body: bytes, status: int, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path in ("/hcp2_log/ws",):
            self._handle_ws()
            return
        if path in ("/", "/index.html"):
            self._send_text(self.server.debug_html, 200, "text/html; charset=utf-8")  # type: ignore[attr-defined]
            return
        if path == "/favicon.ico":
            self._send_bytes(b"", 204, "image/x-icon")
            return
        if path == "/stats":
            self._send_json(self.state.stats())
            return
        if path in ("/health", "/preflight"):
            health = self.state.health()
            self._send_json(health, 200 if health["verdict"] == "ok" else 503)
            return
        if path == "/support":
            self._send_json(self.state.support())
            return
        if path == "/hcp2_log/start":
            self.state.log_enabled = True
            self._send_json(self.state.stats()["protocol_log"])
            return
        if path == "/hcp2_log/stop":
            self.state.log_enabled = False
            self._send_json(self.state.stats()["protocol_log"])
            return
        if path == "/hcp2_log/clear":
            with self.state.lock:
                self.state.recent = []
            self._send_json(self.state.stats()["protocol_log"])
            return
        if path == "/hcp2_log":
            self._send_text(self.state.recent_ndjson(), 200, "application/x-ndjson; charset=utf-8")
            return
        if path == "/hcp2_log.bin":
            self._send_bytes(self.state.recent_ndjson().encode("utf-8"), 200, "application/octet-stream")
            return
        self._send_text("not found\n", 404)

    # -- websocket ----------------------------------------------------------

    def _handle_ws(self) -> None:
        upgrade = (self.headers.get("Upgrade") or "").lower()
        key = self.headers.get("Sec-WebSocket-Key")
        if upgrade != "websocket" or not key:
            self._send_text("bad websocket upgrade\n", 400)
            return
        replace = bool(re.search(r"[?&]replace=(1|true)\b", self.path))
        server = self.server  # type: ignore[assignment]
        with server.ws_lock:  # type: ignore[attr-defined]
            if server.ws_active is not None:  # type: ignore[attr-defined]
                if replace:
                    old: WsClient = server.ws_active  # type: ignore[attr-defined]
                    old.stop.set()
                    try:
                        old.conn.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    server.ws_active = None  # type: ignore[attr-defined]
                else:
                    server.ws_rejects += 1  # type: ignore[attr-defined]
                    self._send_text("log websocket already connected\n", 409)
                    return
            client = WsClient(self.connection)
            server.ws_active = client  # type: ignore[attr-defined]

        accept = base64.b64encode(
            hashlib.sha1((key + WEBSOCKET_GUID).encode("ascii")).digest()
        ).decode("ascii")
        handshake = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n"
        )
        try:
            self.connection.sendall(handshake.encode("ascii"))
        except OSError:
            self._release_ws(client)
            return
        self.close_connection = True
        self._push_loop(client)

    def _push_loop(self, client: WsClient) -> None:
        state = self.state
        conn = self.connection
        conn.settimeout(0.05)
        last_health = 0.0
        last_log = 0.0
        loop_started = time.monotonic()
        try:
            while not client.stop.is_set():
                now = time.monotonic()
                run_age = now - loop_started

                if state.scenario == "disconnect" and run_age > 4.0:
                    break
                feed_live = not (state.scenario == "stale" and run_age > 3.0)

                if feed_live and now - last_health >= 0.5:
                    if not self._ws_send(conn, json.dumps(
                        {"type": "health", "health": state.health()}
                    )):
                        break
                    last_health = now
                if feed_live and now - last_log >= 0.25:
                    lines = state.generate_batch()
                    if lines:
                        if not self._ws_send(conn, json.dumps(
                            {"type": "log", "text": "\n".join(lines) + "\n"}
                        )):
                            break
                    last_log = now

                # Detect client close; discard any (masked) client frames.
                try:
                    data = conn.recv(1024)
                    if data == b"":
                        break
                    if len(data) >= 1 and (data[0] & 0x0F) == 0x8:  # close opcode
                        break
                except socket.timeout:
                    pass
                except OSError:
                    break
                time.sleep(0.02)
        finally:
            self._release_ws(client)

    def _ws_send(self, conn: socket.socket, payload: str) -> bool:
        try:
            conn.sendall(ws_text_frame(payload))
            return True
        except OSError:
            return False

    def _release_ws(self, client: WsClient) -> None:
        server = self.server  # type: ignore[assignment]
        with server.ws_lock:  # type: ignore[attr-defined]
            if server.ws_active is client:  # type: ignore[attr-defined]
                server.ws_active = None  # type: ignore[attr-defined]
        try:
            self.connection.close()
        except OSError:
            pass


def build_server(host: str, port: int, scenario: str) -> ThreadingHTTPServer:
    httpd = ThreadingHTTPServer((host, port), DebugHandler)
    httpd.debug_html = extract_debug_html()  # type: ignore[attr-defined]
    httpd.mock_state = MockState(scenario)  # type: ignore[attr-defined]
    httpd.ws_lock = threading.Lock()  # type: ignore[attr-defined]
    httpd.ws_active = None  # type: ignore[attr-defined]
    httpd.ws_rejects = 0  # type: ignore[attr-defined]
    return httpd


def canonical_records() -> list[dict[str, Any]]:
    """A deterministic record set covering all 6 types + the fault variants.

    Used as the committed fixture so decoders/tests have a stable sample with
    realistic ``tx.event_us ~= rx.event_us + 4200us`` jitter signal.
    """
    seq = 1
    ms = 1000
    us = 1_000_000
    out: list[dict[str, Any]] = []

    def add(**fields: Any) -> None:
        nonlocal seq, ms
        rec = {"seq": seq, "ms": ms}
        rec.update(fields)
        out.append(rec)
        seq += 1
        ms += 12

    add(type="control", action="boot", enabled=True)
    add(type="control", action="start", enabled=True)
    # Nominal poll cycles: status_poll rx then broadcast tx at +4200us +/- jitter.
    for i in range(6):
        us += 70_000
        jitter = (-40, 15, 60, -10, 120, 5)[i]
        add(type="protocol", source="lp", event_us=us, event="rx",
            frame=FRAME_STATUS_POLL, len=5, hex="02170A0000")
        add(type="protocol", source="lp", event_us=us + RESPONSE_DELAY_US + jitter,
            event="tx", frame=FRAME_BROADCAST, len=14,
            hex="0217" + _hex(struct.pack("<H", i * 8)) + "059C4100030600")
    # Decoded state transition.
    add(type="state", source="broadcast", target_position=200, current_position=40,
        state="opening", state_raw=2, light=True, bus_online=True, obstruction=False)
    # Command queue/execute + LP breadcrumb.
    add(type="command", phase="queue", button="open", ok=True, reason="lp_mailbox")
    add(type="lp_trace", event_us=us + 50, event="command", event_id=2, value=1)
    # Fault variants.
    add(type="protocol", source="lp", event_us=us + 90_000, event="bad_crc",
        frame=FRAME_BROADCAST, len=9, hex="02170A0000020504DEAD")
    add(type="protocol", source="lp", event_us=us + 95_000, event="rx_error",
        frame="none", len=0, hex="")
    add(type="lp_trace", event_us=us + 96_000, event="collision", event_id=9, value=1)
    add(type="lp_trace", event_us=us + 97_000, event="tx_abort", event_id=8, value=1)
    add(type="lp_trace_overflow", dropped=3)
    add(type="command", phase="execute", button="close", ok=False, reason="busy")
    return out


def dump_fixture(path: Path, count: int, scenario: str) -> int:
    """Write the canonical NDJSON fixture covering all record types + faults."""
    lines = [json.dumps(r, separators=(",", ":")) for r in canonical_records()]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(lines)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--scenario", choices=SCENARIOS, default="nominal")
    parser.add_argument("--dump-fixture", type=Path, help="Write an NDJSON fixture and exit")
    parser.add_argument("--fixture-count", type=int, default=60)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.dump_fixture:
        n = dump_fixture(args.dump_fixture, args.fixture_count, "nominal")
        print(f"wrote {n} records to {args.dump_fixture}")
        return 0
    httpd = build_server(args.host, args.port, args.scenario)
    print(f"HCP2 debug mock on http://{args.host}:{args.port}/ scenario={args.scenario} (Ctrl-C to stop)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
