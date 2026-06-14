from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path

from tools.hcp2_hil_la import (
    Sample,
    analyze_samples,
    build_sigrok_capture_command,
    decode_uart_windows,
    load_samples,
    summarize_response_latencies,
    summarize_decoded_uart,
    verify_samples,
)
from tools.supramatic_sim import protocol


def write_csv(path: Path, rows: list[tuple[float, int, int, int, int]]) -> None:
    path.write_text(
        "time_s,de,re,tx,rx\n"
        + "".join(f"{time_s:.9f},{de},{re},{tx},{rx}\n" for time_s, de, re, tx, rx in rows),
        encoding="utf-8",
    )


def uart_8e1_rows(frame: bytes, *, start_s: float = 0.001, baud: int = 57600) -> list[tuple[float, int, int, int, int]]:
    bit_s = 1.0 / baud
    rows: list[tuple[float, int, int, int, int]] = [(0.0, 0, 0, 1, 1), (start_s - 0.0001, 1, 0, 1, 1)]
    tx = 1

    def append_if_changed(time_s: float, level: int) -> None:
        nonlocal tx
        if level == tx:
            return
        tx = level
        rows.append((time_s, 1, 0, tx, 1))

    time_s = start_s
    for byte in frame:
        append_if_changed(time_s, 0)
        time_s += bit_s
        ones = 0
        for bit in range(8):
            level = (byte >> bit) & 1
            ones += level
            append_if_changed(time_s, level)
            time_s += bit_s
        append_if_changed(time_s, ones & 1)
        time_s += bit_s
        append_if_changed(time_s, 1)
        time_s += bit_s
    rows.append((time_s + 0.0001, 0, 0, 1, 1))
    rows.append((time_s + 0.0002, 0, 0, 1, 1))
    return rows


def uart_8e1_multi_frame_rows(frames: list[bytes], *, start_s: float = 0.001) -> list[tuple[float, int, int, int, int]]:
    rows: list[tuple[float, int, int, int, int]] = []
    for index, frame in enumerate(frames):
        frame_rows = uart_8e1_rows(frame, start_s=start_s + index * 0.008)
        rows.extend(frame_rows if not rows else frame_rows[1:])
    rows.sort(key=lambda row: row[0])
    return rows


def status_response(counter: int) -> bytes:
    payload = bytes([0x02, 0x17, 0x10, counter & 0xFF, 0x00, 0x03, 0x01] + [0x00] * 12)
    return protocol.append_crc(payload)


def uart_8e1_signal_rows(
    frame: bytes,
    *,
    signal: str,
    start_s: float,
    de_start_s: float | None = None,
    de_end_s: float | None = None,
    baud: int = 57600,
) -> list[tuple[float, int, int, int, int]]:
    bit_s = 1.0 / baud
    levels = {"de": 0, "re": 0, "tx": 1, "rx": 1}
    rows: list[tuple[float, int, int, int, int]] = []

    def append(time_s: float) -> None:
        rows.append((time_s, levels["de"], levels["re"], levels["tx"], levels["rx"]))

    append(max(0.0, start_s - 0.0002))
    if de_start_s is not None:
        levels["de"] = 1
        append(de_start_s)

    def append_if_changed(time_s: float, level: int) -> None:
        if levels[signal] == level:
            return
        levels[signal] = level
        append(time_s)

    time_s = start_s
    for byte in frame:
        append_if_changed(time_s, 0)
        time_s += bit_s
        ones = 0
        for bit in range(8):
            level = (byte >> bit) & 1
            ones += level
            append_if_changed(time_s, level)
            time_s += bit_s
        append_if_changed(time_s, ones & 1)
        time_s += bit_s
        append_if_changed(time_s, 1)
        time_s += bit_s

    if de_end_s is not None:
        levels["de"] = 0
        append(de_end_s)
    append(time_s + 0.0002)
    return rows


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


def test_logic_analyzer_can_allow_re_high_only_during_de(tmp_path: Path) -> None:
    capture = tmp_path / "re-during-de.csv"
    write_csv(
        capture,
        [
            (0.0000, 0, 0, 1, 1),
            (0.0010, 1, 1, 1, 1),
            (0.0011, 1, 1, 0, 1),
            (0.0012, 1, 1, 1, 1),
            (0.0015, 0, 0, 1, 1),
            (0.0020, 0, 0, 1, 1),
        ],
    )

    samples = load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"})
    strict_report = analyze_samples(samples, max_de_high_us=9000.0)
    runtime_report = analyze_samples(samples, max_de_high_us=9000.0, allow_re_high_during_de=True)

    assert strict_report["verdict"] == "fail"
    assert strict_report["re_high_samples"] == 3
    assert runtime_report["verdict"] == "ok"
    assert runtime_report["re_high_samples"] == 3
    assert runtime_report["re_high_outside_de_samples"] == 0

    outside_samples = samples + [Sample(0.0021, {"de": 0, "re": 1, "tx": 1, "rx": 1})]
    outside_report = analyze_samples(outside_samples, max_de_high_us=9000.0, allow_re_high_during_de=True)
    assert outside_report["verdict"] == "fail"
    assert outside_report["re_high_outside_de_samples"] == 1


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

    explicit_de_report = analyze_samples(
        load_samples(capture, {"de": "de", "re": "re", "tx": "tx"}),
        require_tx_only_during_de=False,
    )
    assert explicit_de_report["verdict"] == "ok"
    assert explicit_de_report["tx_transitions_outside_de"] == 2


def test_logic_analyzer_can_crop_startup_glitch_for_runtime_report(tmp_path: Path) -> None:
    capture = tmp_path / "startup-glitch.csv"
    write_csv(
        capture,
        [
            (0.0000, 1, 1, 1, 1),
            (0.0800, 0, 0, 1, 1),
            (0.1000, 0, 0, 1, 1),
            (0.1010, 1, 0, 1, 1),
            (0.1011, 1, 0, 0, 1),
            (0.1012, 1, 0, 1, 1),
            (0.1040, 0, 0, 1, 1),
            (0.1100, 0, 0, 1, 1),
        ],
    )
    samples = load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"})

    strict_report = analyze_samples(samples, max_de_high_us=9000.0)
    runtime_report = analyze_samples(samples, max_de_high_us=9000.0, ignore_before_us=90_000.0)

    assert strict_report["verdict"] == "fail"
    assert runtime_report["verdict"] == "ok"
    assert runtime_report["ignored_before_us"] == 90_000.0
    assert runtime_report["de_windows"] == 1


def test_logic_analyzer_decodes_8e1_uart_frame(tmp_path: Path) -> None:
    capture = tmp_path / "uart.csv"
    frame = protocol.SCAN_RESPONSE
    write_csv(capture, uart_8e1_rows(frame))

    windows = decode_uart_windows(load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"}))

    assert len(windows) == 1
    assert windows[0]["frame"] == frame.hex()
    assert windows[0]["crc_ok"] is True
    assert windows[0]["kind"] == "scan"
    assert windows[0]["errors"] == []


def test_logic_analyzer_rejects_status_counter_gap(tmp_path: Path) -> None:
    capture = tmp_path / "gap.csv"
    write_csv(capture, uart_8e1_multi_frame_rows([status_response(0), status_response(2)]))

    windows = decode_uart_windows(load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"}))
    report = summarize_decoded_uart(windows, ignore_before_us=0.0, baud=57600)

    assert report["verdict"] == "fail"
    assert report["crc_ok_frames"] == 2
    assert report["status_counter_gap_count"] == 1
    assert report["status_counter_gaps"][0]["missing"] == [1]


def test_logic_analyzer_verify_combines_electrical_and_uart_verdict(tmp_path: Path) -> None:
    capture = tmp_path / "verify.csv"
    write_csv(capture, uart_8e1_multi_frame_rows([status_response(0), status_response(1)]))

    report = verify_samples(
        load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"}),
        min_status_frames=2,
    )

    assert report["verdict"] == "ok"
    assert report["electrical"]["verdict"] == "ok"
    assert report["uart"]["verdict"] == "ok"
    assert report["uart"]["status_counter_gap_count"] == 0


def test_logic_analyzer_correlates_poll_to_response_latency(tmp_path: Path) -> None:
    capture = tmp_path / "latency.csv"
    poll = protocol.status_poll(0)
    response = status_response(0)
    baud = 57600
    bit_s = 1.0 / baud
    frame_bits = 11
    poll_start_s = 0.001
    poll_end_s = poll_start_s + len(poll) * frame_bits * bit_s
    de_start_s = poll_end_s + 0.0042
    tx_start_s = de_start_s + 0.000020
    response_end_s = tx_start_s + len(response) * frame_bits * bit_s
    de_end_s = response_end_s + 0.00025
    rows = (
        uart_8e1_signal_rows(poll, signal="rx", start_s=poll_start_s)
        + uart_8e1_signal_rows(response, signal="tx", start_s=tx_start_s, de_start_s=de_start_s, de_end_s=de_end_s)
    )
    rows.sort(key=lambda row: row[0])
    write_csv(capture, rows)

    samples = load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"})
    report = summarize_response_latencies(samples)

    assert report["verdict"] == "ok"
    assert report["matched_status_pairs"] == 1
    assert abs(report["poll_end_to_de_assert"]["max_us"] - 4200.0) < 2.0
    assert abs(report["poll_end_to_first_byte"]["max_us"] - 4220.0) < 2.0
    assert report["poll_end_to_response_end"]["max_us"] > report["poll_end_to_first_byte"]["max_us"]


def test_logic_analyzer_latency_uses_nearest_prior_poll_for_reused_counter(tmp_path: Path) -> None:
    capture = tmp_path / "latency-reused-counter.csv"
    poll = protocol.status_poll(7)
    response = status_response(7)
    baud = 57600
    bit_s = 1.0 / baud
    frame_bits = 11
    poll1_start_s = 0.001
    poll2_start_s = 0.050
    poll2_end_s = poll2_start_s + len(poll) * frame_bits * bit_s
    de_start_s = poll2_end_s + 0.0042
    tx_start_s = de_start_s + 0.000020
    response_end_s = tx_start_s + len(response) * frame_bits * bit_s
    rows = (
        uart_8e1_signal_rows(poll, signal="rx", start_s=poll1_start_s)
        + uart_8e1_signal_rows(poll, signal="rx", start_s=poll2_start_s)
        + uart_8e1_signal_rows(
            response,
            signal="tx",
            start_s=tx_start_s,
            de_start_s=de_start_s,
            de_end_s=response_end_s + 0.00025,
        )
    )
    rows.sort(key=lambda row: row[0])
    write_csv(capture, rows)

    samples = load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"})
    report = summarize_response_latencies(samples)

    assert report["verdict"] == "ok"
    assert report["matched_status_pairs"] == 1
    assert abs(report["poll_end_to_de_assert"]["max_us"] - 4200.0) < 2.0


def test_logic_analyzer_verify_rejects_undersampled_uart_capture(tmp_path: Path) -> None:
    capture = tmp_path / "undersampled.csv"
    write_csv(capture, [(index / 100_000.0, 0, 0, 1, 1) for index in range(100)])

    report = verify_samples(
        load_samples(capture, {"de": "de", "re": "re", "tx": "tx", "rx": "rx"}),
        min_status_frames=1,
    )

    assert report["verdict"] == "fail"
    assert report["uart"]["sampling"]["samples_per_bit"] < 2.0
    assert any("sample rate is too low" in failure for failure in report["uart"]["failures"])


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


def test_logic_analyzer_loads_sigrok_csv_samples_without_time_column(tmp_path: Path) -> None:
    capture = tmp_path / "sigrok.csv"
    capture.write_text(
        "; CSV generated by libsigrok 0.6.0\n"
        "; Channels (4/8): D1, D3, D5, D7\n"
        "; Samplerate: 1 MHz\n"
        "logic,logic,logic,logic\n"
        "1,0,0,1\n"
        "0,1,0,1\n"
        "1,1,0,1\n"
        "FRAME-END\n",
        encoding="utf-8",
    )

    samples = load_samples(capture, {"tx": "D1", "de": "D3", "re": "D5", "rx": "D7"})

    assert [sample.time_s for sample in samples] == [0.0, 0.000001, 0.000002]
    assert samples[0].values == {"tx": 1, "de": 0, "re": 0, "rx": 1}
    assert samples[1].values == {"tx": 0, "de": 1, "re": 0, "rx": 1}


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
