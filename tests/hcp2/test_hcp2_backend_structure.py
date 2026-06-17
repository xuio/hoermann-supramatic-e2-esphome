from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMMON_SOURCE = ROOT / "components" / "hcp2bridge" / "hcp2bridge.cpp"
C6_LP_BACKEND_SOURCE = ROOT / "components" / "hcp2bridge" / "hcp2bridge_backend_lp.cpp"


def test_c6_lp_loader_dependencies_are_isolated() -> None:
    common = COMMON_SOURCE.read_text()
    backend = C6_LP_BACKEND_SOURCE.read_text()

    forbidden_in_common = (
        '#include "hcp2_lp_blob.h"',
        '#include "lp_core_uart.h"',
        '#include "ulp_lp_core.h"',
        "HCP2_LP_MAILBOX_ADDR",
        "lp_core_uart_init",
        "ulp_lp_core_load_binary",
        "ulp_lp_core_run",
        "ulp_lp_core_stop",
    )
    for token in forbidden_in_common:
        assert token not in common

    required_in_backend = (
        '#include "hcp2_lp_blob.h"',
        '#include "lp_core_uart.h"',
        '#include "ulp_lp_core.h"',
        "HCP2_LP_MAILBOX_ADDR",
        "lp_core_uart_init",
        "ulp_lp_core_load_binary",
        "ulp_lp_core_run",
    )
    for token in required_in_backend:
        assert token in backend


def test_common_component_routes_through_backend_capabilities() -> None:
    common = COMMON_SOURCE.read_text()

    assert "switch (this->backend_kind_)" in common
    assert "this->backend_survives_hp_restart_() && this->is_continuity_healthy()" in common
    assert "this->backend_uses_mailbox_()" in common
