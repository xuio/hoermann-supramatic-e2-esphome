from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass
class FetchResult:
    path: str
    status: int | None
    content_type: str | None
    body: bytes
    error: str | None = None


def fetch(base_url: str, path: str, timeout: float) -> FetchResult:
    url = f"{base_url}{path}"
    request = urllib.request.Request(url, headers={"User-Agent": "garage-hcp2-support-bundle/1"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return FetchResult(
                path=path,
                status=response.status,
                content_type=response.headers.get("content-type"),
                body=response.read(),
            )
    except urllib.error.HTTPError as exc:
        return FetchResult(
            path=path,
            status=exc.code,
            content_type=exc.headers.get("content-type"),
            body=exc.read(),
            error=str(exc),
        )
    except OSError as exc:
        return FetchResult(path=path, status=None, content_type=None, body=b"", error=str(exc))


def write_result(output_dir: Path, result: FetchResult, filename: str) -> dict[str, object]:
    path = output_dir / filename
    path.write_bytes(result.body)
    return {
        "endpoint": result.path,
        "file": filename,
        "status": result.status,
        "content_type": result.content_type,
        "bytes": len(result.body),
        "error": result.error,
    }


def result_is_collectable(entry: dict[str, object]) -> bool:
    endpoint = entry.get("endpoint")
    status = entry.get("status")
    byte_count = entry.get("bytes")
    if endpoint == "/health" and status in (200, 503) and isinstance(byte_count, int) and byte_count > 0:
        return True
    return status == 200


def endpoint_plan(include_log: bool) -> Iterable[tuple[str, str]]:
    yield "/health", "health.json"
    yield "/support", "support.json"
    yield "/stats", "stats.json"
    if include_log:
        yield "/hcp2_log", "hcp2-log.ndjson"
        yield "/hcp2_log.bin", "hcp2-log.bin"


def run_control_sequence(base_url: str, action: str, timeout: float, output_dir: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    if action == "start":
        for path, filename in (("/hcp2_log/clear", "control-clear.json"), ("/hcp2_log/start", "control-start.json")):
            records.append(write_result(output_dir, fetch(base_url, path, timeout), filename))
    elif action == "stop-and-collect":
        records.append(write_result(output_dir, fetch(base_url, "/hcp2_log/stop", timeout), "control-stop.json"))
    elif action == "clear":
        records.append(write_result(output_dir, fetch(base_url, "/hcp2_log/clear", timeout), "control-clear.json"))
    return records


def make_output_dir(root: Path | None) -> Path:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    output_dir = root or Path("captures") / "hcp2" / f"support-bundle-{stamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    return output_dir


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Collect an HCP2 tester support bundle from the ESP HTTP debug port")
    parser.add_argument("--host", required=True, help="ESP hostname or IP address")
    parser.add_argument("--port", type=int, default=80, help="HCP2 HTTP debug port")
    parser.add_argument("--timeout", type=float, default=10.0, help="HTTP timeout per request in seconds")
    parser.add_argument("--output-dir", type=Path, help="Bundle output directory")
    parser.add_argument(
        "--action",
        choices=("collect", "start", "stop-and-collect", "clear"),
        default="collect",
        help="collect current state, start a fresh log window, stop then collect, or clear the RAM log",
    )
    parser.add_argument("--no-log", action="store_true", help="Skip /hcp2_log downloads during collect actions")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    output_dir = make_output_dir(args.output_dir)
    base_url = f"http://{args.host}:{args.port}"
    manifest: dict[str, object] = {
        "tool": "garage-hcp2-support-bundle",
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "base_url": base_url,
        "action": args.action,
        "files": [],
    }

    files: list[dict[str, object]] = []
    files.extend(run_control_sequence(base_url, args.action, args.timeout, output_dir))

    if args.action in ("collect", "stop-and-collect"):
        for path, filename in endpoint_plan(include_log=not args.no_log):
            files.append(write_result(output_dir, fetch(base_url, path, args.timeout), filename))

    manifest["files"] = files
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(output_dir)

    failed = [entry for entry in files if not result_is_collectable(entry)]
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
