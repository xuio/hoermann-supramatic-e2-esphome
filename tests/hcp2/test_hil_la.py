from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path

from tools.hcp2_hil_la import analyze_samples, build_sigrok_capture_command, load_samples


def write_csv(path: Path, rows: list[tuple[float, int, int, int, int]]) -> None:
    path.write_text(
        "time_s,de,re,tx,rx\n"
        + "".join(f"{time_s:.6f},{de},{re},{tx},{rx}\n" for time_s, de, re, tx, rx in rows),
        encoding="utf-8",
    )


def test_logic_analyzer_report_accepts_bounded_de_window(tmp_path: Path) -> None:
    capture = tmp_path / "ok.csv"
    write_csv(
        capture,
        [
            (0.0000, 0, 0, 1, 1),
            (0.0010, 1, 0, 1, 1),
            (0.0011, 1, 0, 0, 1),
            (0.0012, 1, 0, 1, 1),
            (0.0015, 0, 0, 1, 1),
            (0.0020, 0, 0, 1, 1),
        ],
    )

    samples = load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"})
    report = analyze_samples(samples, max_de_high_us=9000.0)

    assert report["verdict"] == "ok"
    assert report["de_windows"] == 1
    assert report["tx_transitions_outside_de"] == 0


def test_logic_analyzer_report_rejects_de_deadman_violation(tmp_path: Path) -> None:
    capture = tmp_path / "long-de.csv"
    write_csv(
        capture,
        [
            (0.0000, 0, 0, 1, 1),
            (0.0010, 1, 0, 1, 1),
            (0.0011, 1, 0, 0, 1),
            (0.0110, 0, 0, 1, 1),
        ],
    )

    report = analyze_samples(load_samples(capture, {"de": "de", "re": "re", "tx": "tx"}), max_de_high_us=8000.0)

    assert report["verdict"] == "fail"
    assert "exceeds" in report["failures"][0]


def test_logic_analyzer_report_rejects_tx_activity_while_de_low(tmp_path: Path) -> None:
    capture = tmp_path / "tx-low.csv"
    write_csv(
        capture,
        [
            (0.0000, 0, 0, 1, 1),
            (0.0002, 0, 0, 0, 1),
            (0.0003, 0, 0, 1, 1),
        ],
    )

    report = analyze_samples(load_samples(capture, {"de": "de", "re": "re", "tx": "tx"}))

    assert report["verdict"] == "fail"
    assert any("TX transitions" in failure for failure in report["failures"])


def test_logic_analyzer_loads_json_samples(tmp_path: Path) -> None:
    capture = tmp_path / "ok.json"
    capture.write_text(
        json.dumps(
            {
                "samples": [
                    {"time_s": 0.0, "DE": 0, "RE": 0, "TX": 1},
                    {"time_s": 0.001, "DE": 1, "RE": 0, "TX": 1},
                    {"time_s": 0.0011, "DE": 1, "RE": 0, "TX": 0},
                    {"time_s": 0.0012, "DE": 1, "RE": 0, "TX": 1},
                    {"time_s": 0.0015, "DE": 0, "RE": 0, "TX": 1},
                ]
            }
        ),
        encoding="utf-8",
    )

    samples = load_samples(capture, {"de": "DE", "re": "RE", "tx": "TX"})
    report = analyze_samples(samples)

    assert report["verdict"] == "ok"


def test_sigrok_capture_command_uses_hcp2_channel_mapping() -> None:
    args = Namespace(
        sigrok_cli="sigrok-cli",
        driver="fx2lafw",
        samplerate="2m",
        duration="10s",
        channels="de=D4,re=D3,tx=D5,rx=D2",
        output=Path("capture.sr"),
        output_format="srzip",
    )

    command = build_sigrok_capture_command(args)

    assert command == [
        "sigrok-cli",
        "--driver",
        "fx2lafw",
        "--config",
        "samplerate=2m",
        "--channels",
        "D4,D3,D5,D2",
        "--time",
        "10s",
        "--output-file",
        "capture.sr",
        "--output-format",
        "srzip",
    ]
