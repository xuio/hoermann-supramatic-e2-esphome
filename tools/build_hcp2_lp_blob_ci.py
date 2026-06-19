"""Build and refresh the HCP2 LP blob with the same ESP-IDF image used by CI."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from tools import update_hcp2_lp_blob


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGE = "espressif/idf:release-v5.5"


def _default_user_arg() -> list[str]:
    if os.name != "posix":
        return []
    try:
        return ["--user", f"{os.getuid()}:{os.getgid()}"]
    except AttributeError:
        return []


def _run(cmd: list[str], *, cwd: Path) -> None:
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Build firmware/hcp2-lp in the CI-matching ESP-IDF Docker image and regenerate "
            "components/hcp2bridge/lp_blob/hcp2_lp_blob.c."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--image", default=os.environ.get("HCP2_LP_IDF_IMAGE", DEFAULT_IMAGE), help="ESP-IDF Docker image")
    parser.add_argument(
        "--no-user",
        action="store_true",
        help="run the container as its default user instead of the host uid/gid",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="after building, verify the checked-in blob instead of updating it",
    )
    args = parser.parse_args(argv)

    user_args = [] if args.no_user else _default_user_arg()
    docker_cmd = [
        "docker",
        "run",
        "--rm",
        *user_args,
        "-e",
        "HOME=/tmp",
        "-e",
        "PYTHONDONTWRITEBYTECODE=1",
        "-v",
        f"{ROOT}:/work",
        "-w",
        "/work",
        args.image,
        "bash",
        "-lc",
        "idf.py -C firmware/hcp2-lp fullclean && idf.py -C firmware/hcp2-lp build",
    ]

    try:
        _run(["docker", "info"], cwd=ROOT)
        _run(docker_cmd, cwd=ROOT)
    except FileNotFoundError:
        print("docker is not installed; install Docker or use the CI LP artifact deliberately", file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as exc:
        print(f"CI-matching LP build failed with exit code {exc.returncode}", file=sys.stderr)
        return exc.returncode or 1

    update_args = []
    if args.check:
        update_args.append("--check")
    return update_hcp2_lp_blob.main(update_args)


if __name__ == "__main__":
    raise SystemExit(main())
