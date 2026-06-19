from __future__ import annotations

import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HCP2_DIR = ROOT / "tests" / "hcp2"


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, cwd=ROOT, check=True)


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
            crc &= 0xFFFF
    return crc


def test_committed_vectors_are_crc_clean() -> None:
    vector_files = sorted((HCP2_DIR / "vectors").glob("*.jsonl"))
    assert vector_files
    total_count = 0
    for vector_path in vector_files:
        file_count = 0
        for line in vector_path.read_text().splitlines():
            record = json.loads(line)
            raw = bytes.fromhex(record["raw"])
            assert crc16_modbus(raw) == 0, record["name"]
            file_count += 1
            total_count += 1
        assert file_count > 0, vector_path.name
    assert total_count >= 34


def test_c_unit_and_fuzz_smoke() -> None:
    run(["make", "-C", "tests/hcp2", "clean", "all", "FUZZ_ITERATIONS=5000"])
