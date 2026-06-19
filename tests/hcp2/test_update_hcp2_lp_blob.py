from __future__ import annotations

import hashlib
import json
from pathlib import Path

from tools.update_hcp2_lp_blob import main


def write_binary_with_metadata(tmp_path: Path, metadata: dict[str, str] | None) -> Path:
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    binary = build_dir / "hcp2_lp.bin"
    if metadata is not None:
        (build_dir / "project_description.json").write_text(json.dumps(metadata), encoding="utf-8")
    binary.write_bytes(b"\x01\x02\xfe\xff")
    return binary


def official_metadata() -> dict[str, str]:
    return {
        "idf_path": "/opt/esp/idf",
        "git_revision": "1576525da",
        "project_name": "hcp2_lp_loader",
    }


def test_update_hcp2_lp_blob_accepts_ci_style_idf_metadata(tmp_path: Path) -> None:
    binary = write_binary_with_metadata(tmp_path, official_metadata())
    output = tmp_path / "hcp2_lp_blob.c"
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()

    assert main(["--binary", str(binary), "--output", str(output)]) == 0

    text = output.read_text(encoding="utf-8")
    assert f"sha256={digest}" in text
    assert "const unsigned int hcp2_lp_blob_data_len = 4;" in text
    assert main(["--binary", str(binary), "--output", str(output), "--check"]) == 0


def test_update_hcp2_lp_blob_rejects_platformio_idf_metadata(tmp_path: Path, capsys) -> None:
    binary = write_binary_with_metadata(
        tmp_path,
        {
            "idf_path": "/Users/example/.platformio/packages/framework-espidf",
            "git_revision": "5.5.4",
            "project_name": "hcp2_lp_loader",
        },
    )
    output = tmp_path / "hcp2_lp_blob.c"

    assert main(["--binary", str(binary), "--output", str(output)]) == 2

    captured = capsys.readouterr()
    assert "PlatformIO" in captured.out
    assert not output.exists()


def test_update_hcp2_lp_blob_rejects_host_idf_checkout_metadata(tmp_path: Path, capsys) -> None:
    metadata = official_metadata()
    metadata["idf_path"] = "/Users/example/esp/esp-idf"
    binary = write_binary_with_metadata(tmp_path, metadata)
    output = tmp_path / "hcp2_lp_blob.c"

    assert main(["--binary", str(binary), "--output", str(output)]) == 2

    captured = capsys.readouterr()
    assert "garage-build-hcp2-lp-blob-ci" in captured.out
    assert "CI Docker image" in captured.out
    assert not output.exists()


def test_update_hcp2_lp_blob_rejects_mixed_platformio_cmake_cache(tmp_path: Path, capsys) -> None:
    binary = write_binary_with_metadata(tmp_path, official_metadata())
    (binary.parent / "CMakeCache.txt").write_text(
        "CMAKE_TOOLCHAIN_FILE=/Users/example/.platformio/packages/framework-espidf/tools/cmake/toolchain.cmake\n",
        encoding="utf-8",
    )
    output = tmp_path / "hcp2_lp_blob.c"

    assert main(["--binary", str(binary), "--output", str(output)]) == 2

    captured = capsys.readouterr()
    assert "still references PlatformIO" in captured.out
    assert not output.exists()


def test_update_hcp2_lp_blob_requires_metadata_unless_explicitly_skipped(tmp_path: Path) -> None:
    binary = write_binary_with_metadata(tmp_path, None)
    output = tmp_path / "hcp2_lp_blob.c"

    assert main(["--binary", str(binary), "--output", str(output)]) == 2
    assert main(["--binary", str(binary), "--output", str(output), "--skip-idf-origin-check"]) == 0
    assert output.exists()
