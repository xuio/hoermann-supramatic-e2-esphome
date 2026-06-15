from __future__ import annotations

from tools.hcp2_debug_browser_e2e import DEFAULT_CACHE_AGE_MS, DEFAULT_CACHE_BYTES, base_url_from_args, parse_args, rows_to_dict


def test_browser_e2e_defaults_match_frontend_cache_contract() -> None:
    args = parse_args(["--host", "192.0.2.10"])
    assert base_url_from_args(args) == "http://192.0.2.10:80"
    assert args.expect_cache_age_ms == DEFAULT_CACHE_AGE_MS == 30 * 60 * 1000
    assert args.expect_cache_bytes == DEFAULT_CACHE_BYTES == 100 * 1024 * 1024
    assert args.start_log is True


def test_browser_e2e_url_target_takes_full_base_url() -> None:
    args = parse_args(["--url", "http://example.local:8080/"])
    assert base_url_from_args(args) == "http://example.local:8080"


def test_rows_to_dict_decodes_debug_panel_pairs() -> None:
    assert rows_to_dict("polls seen\n42\nmissed polls\n0\n") == {
        "polls seen": "42",
        "missed polls": "0",
    }
