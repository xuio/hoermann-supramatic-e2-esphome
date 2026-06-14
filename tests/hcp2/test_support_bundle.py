from __future__ import annotations

from tools.hcp2_support_bundle import endpoint_plan, result_is_collectable


def test_support_bundle_collects_health_first() -> None:
    assert list(endpoint_plan(include_log=False)) == [
        ("/health", "health.json"),
        ("/support", "support.json"),
        ("/stats", "stats.json"),
    ]


def test_support_bundle_accepts_red_health_verdict_as_collected_evidence() -> None:
    assert result_is_collectable({"endpoint": "/health", "status": 503, "bytes": 64})
    assert not result_is_collectable({"endpoint": "/health", "status": 503, "bytes": 0})
    assert not result_is_collectable({"endpoint": "/support", "status": 503, "bytes": 64})
