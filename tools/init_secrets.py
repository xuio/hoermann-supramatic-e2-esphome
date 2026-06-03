"""Generate local ESPHome secrets for the garage-door controller."""

from __future__ import annotations

import argparse
import base64
import os
import re
import secrets
from pathlib import Path


DEFAULT_EXAMPLE = Path("configs/secrets.example.yaml")
DEFAULT_OUTPUT = Path("configs/secrets.yaml")

SECRET_KEYS = (
    "api_key_supramatic_e2",
    "ota_password_supramatic_e2",
    "proxy_auth_token",
)

PLACEHOLDER_VALUES = {
    "",
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
    "replace-with-a-real-esphome-api-key",
    "replace-with-a-long-random-ota-password",
    "replace-with-a-long-random-token",
}


def generate_values() -> dict[str, str]:
    """Return fresh secrets in the formats expected by the ESPHome configs."""

    return {
        # ESPHome API encryption keys are base64-encoded 32-byte Noise PSKs.
        "api_key_supramatic_e2": base64.b64encode(secrets.token_bytes(32)).decode("ascii"),
        "ota_password_supramatic_e2": secrets.token_urlsafe(32),
        "proxy_auth_token": secrets.token_urlsafe(32),
    }


def is_placeholder(value: str | None) -> bool:
    if value is None:
        return True
    stripped = value.strip().strip("\"'")
    return stripped in PLACEHOLDER_VALUES or stripped.startswith("replace-with-")


def existing_values(text: str) -> dict[str, str | None]:
    values: dict[str, str | None] = {}
    for key in SECRET_KEYS:
        match = re.search(rf"^\s*{re.escape(key)}\s*:\s*([\"']?)([^\"'\n#]*)\1", text, re.MULTILINE)
        values[key] = match.group(2).strip() if match else None
    return values


def render_secrets(template: str, values: dict[str, str]) -> str:
    lines: list[str] = []
    seen: set[str] = set()

    for line in template.splitlines():
        replaced = False
        for key in SECRET_KEYS:
            match = re.match(rf"^(\s*{re.escape(key)}\s*:\s*).*$", line)
            if match:
                lines.append(f'{match.group(1)}"{values[key]}"')
                seen.add(key)
                replaced = True
                break
        if not replaced:
            lines.append(line)

    if lines and lines[-1].strip():
        lines.append("")
    for key in SECRET_KEYS:
        if key not in seen:
            lines.append(f'{key}: "{values[key]}"')

    return "\n".join(lines).rstrip() + "\n"


def write_private_file(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_text(text, encoding="utf-8")
    try:
        os.chmod(tmp, 0o600)
    except OSError:
        pass
    tmp.replace(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create configs/secrets.yaml with generated ESPHome secrets.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--example", type=Path, default=DEFAULT_EXAMPLE, help="Template secrets file")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Secrets file to create")
    parser.add_argument(
        "--force",
        action="store_true",
        help="Replace existing non-placeholder secrets with newly generated values",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    if not args.example.exists():
        raise SystemExit(f"Template not found: {args.example}")

    template = args.example.read_text(encoding="utf-8")
    if args.output.exists() and not args.force:
        current = existing_values(args.output.read_text(encoding="utf-8"))
        real_keys = [key for key, value in current.items() if not is_placeholder(value)]
        if real_keys:
            keys = ", ".join(real_keys)
            raise SystemExit(
                f"{args.output} already contains non-placeholder values for {keys}. "
                "Use --force only if you intentionally want to regenerate them."
            )
        template = args.output.read_text(encoding="utf-8")

    values = generate_values()
    write_private_file(args.output, render_secrets(template, values))
    print(f"Created {args.output} with generated ESPHome API, OTA, and proxy secrets.")
    print("Keep this file private. It is intentionally ignored by git.")


if __name__ == "__main__":
    main()
