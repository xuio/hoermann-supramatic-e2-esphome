"""Generate the checked-in HCP2 LP blob C source from the ESP-IDF LP binary."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "firmware" / "hcp2-lp" / "build" / "hcp2_lp.bin"
DEFAULT_OUTPUT = ROOT / "components" / "hcp2bridge" / "lp_blob" / "hcp2_lp_blob.c"
BYTES_PER_LINE = 12


def render_blob(data: bytes) -> str:
    lines = [
        '#include "hcp2_lp_blob.h"',
        "",
        "const unsigned char hcp2_lp_blob_data[] = {",
    ]
    for offset in range(0, len(data), BYTES_PER_LINE):
        chunk = data[offset : offset + BYTES_PER_LINE]
        values = ", ".join(f"0x{byte:02x}" for byte in chunk)
        lines.append(f"  {values},")
    lines.extend(
        [
            "};",
            f"const unsigned int hcp2_lp_blob_data_len = {len(data)};",
            "",
        ]
    )
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> bool:
    existing = path.read_text(encoding="utf-8") if path.exists() else None
    if existing == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Update or check components/hcp2bridge/lp_blob/hcp2_lp_blob.c from hcp2_lp.bin.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY, help="Built LP binary to embed")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Generated C source file")
    parser.add_argument("--check", action="store_true", help="fail if the checked-in blob is stale")
    args = parser.parse_args(argv)

    if not args.binary.exists():
        print(f"{args.binary} does not exist; build firmware/hcp2-lp first")
        return 2

    data = args.binary.read_bytes()
    content = render_blob(data)
    digest = hashlib.sha256(data).hexdigest()

    if args.check:
        existing = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
        if existing != content:
            print(f"{args.output} is stale; run garage-update-hcp2-lp-blob")
            print(f"source: {args.binary} ({len(data)} bytes, sha256={digest})")
            return 1
        print(f"{args.output} is current ({len(data)} bytes, sha256={digest})")
        return 0

    changed = write_if_changed(args.output, content)
    action = "updated" if changed else "already current"
    print(f"{args.output} {action} ({len(data)} bytes, sha256={digest})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
