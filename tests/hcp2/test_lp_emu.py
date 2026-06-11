from __future__ import annotations

from pathlib import Path

import pytest

from tools.hcp2_lp_emu.emulator import (
    MAILBOX_ADDR,
    MAILBOX_MAGIC,
    LPEmulator,
    rvc_smoke,
)


ROOT = Path(__file__).resolve().parents[2]
LP_BLOB = ROOT / "firmware" / "hcp2-lp" / "build" / "hcp2_lp.bin"


def test_rvc_smoke() -> None:
    assert rvc_smoke()


def test_lp_emulator_mailbox_and_hp_reboot_when_built() -> None:
    if not LP_BLOB.exists():
        pytest.skip("LP blob has not been built")

    emu = LPEmulator.from_blob(LP_BLOB)
    emu.boot()
    before = emu.heartbeat()
    assert before > 0
    assert int.from_bytes(emu.uc.mem_read(MAILBOX_ADDR, 4), "little") == MAILBOX_MAGIC

    emu.run(100_000)
    after = emu.heartbeat()
    assert after > before
    assert emu.reload_decision(before, after) == "skip"

    emu.hp_reboot()
    command_result = emu.command("open")
    assert command_result.startswith("OK")
    assert emu.report()["unmodeled_mmio"] == []
