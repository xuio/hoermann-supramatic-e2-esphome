from __future__ import annotations

import struct
from pathlib import Path

import pytest

from tools.hcp2_lp_emu.emulator import (
    MAILBOX_ADDR,
    MAILBOX_ABI_VERSION,
    MAILBOX_FIRMWARE_VERSION,
    MAILBOX_MAGIC,
    MAILBOX_SIZE,
    LPEmulator,
    rvc_smoke,
)
from tools.hcp2_lp_emu.dual_iss import (
    DualISSHarness,
    HPIssToolchainMissing,
    RELOAD_REQUIRED,
    RELOAD_SKIP,
)


ROOT = Path(__file__).resolve().parents[2]
LP_BLOB = ROOT / "firmware" / "hcp2-lp" / "build" / "hcp2_lp.bin"

OFF_MAGIC = 0
OFF_ABI_VERSION = 4
OFF_STRUCT_SIZE = 6
OFF_FIRMWARE_VERSION = 8
OFF_HEARTBEAT = 12
OFF_STATE_SEQ = 16
OFF_TARGET_POSITION = 20
OFF_CURRENT_POSITION = 21
OFF_STATE = 22
OFF_LIGHT_ON = 23
OFF_STATE_UPDATED_US = 24
OFF_COMMAND_EPOCH = 28
OFF_COMMAND_SEQUENCE = 32
OFF_COMMAND_ACK_SEQUENCE = 36
OFF_COMMAND_ID = 40
OFF_COMMAND_ARGUMENT = 41

COMMAND_OPEN = 1


def require_emulator() -> LPEmulator:
    if not LP_BLOB.exists():
        pytest.skip("LP blob has not been built")
    return LPEmulator.from_blob(LP_BLOB)


def require_dual_iss() -> DualISSHarness:
    if not LP_BLOB.exists():
        pytest.skip("LP blob has not been built")
    try:
        return DualISSHarness.from_blob(LP_BLOB)
    except HPIssToolchainMissing as exc:
        pytest.skip(str(exc))


def read_u32(emu: LPEmulator, offset: int) -> int:
    return struct.unpack("<I", emu.uc.mem_read(MAILBOX_ADDR + offset, 4))[0]


def write_u32(emu: LPEmulator, offset: int, value: int) -> None:
    emu.uc.mem_write(MAILBOX_ADDR + offset, struct.pack("<I", value & 0xFFFFFFFF))


def write_u16(emu: LPEmulator, offset: int, value: int) -> None:
    emu.uc.mem_write(MAILBOX_ADDR + offset, struct.pack("<H", value & 0xFFFF))


def write_u8(emu: LPEmulator, offset: int, value: int) -> None:
    emu.uc.mem_write(MAILBOX_ADDR + offset, bytes([value & 0xFF]))


def inject_command(emu: LPEmulator, *, epoch: int, sequence: int, command_id: int) -> None:
    write_u32(emu, OFF_COMMAND_EPOCH, epoch)
    write_u8(emu, OFF_COMMAND_ID, command_id)
    write_u8(emu, OFF_COMMAND_ARGUMENT, 0)
    write_u32(emu, OFF_COMMAND_SEQUENCE, sequence)


def test_rvc_smoke() -> None:
    assert rvc_smoke()


def test_lp_emulator_mailbox_and_hp_reboot_when_built() -> None:
    emu = require_emulator()
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


def test_lp_emulator_reload_decision_rejects_stale_or_incompatible_mailbox() -> None:
    emu = require_emulator()
    emu.boot()
    before = emu.heartbeat()
    emu.run(100_000)
    after = emu.heartbeat()
    assert after > before
    assert emu.reload_decision(before, after) == "skip"
    assert emu.reload_decision(after, after) == "reload"

    write_u32(emu, OFF_MAGIC, 0)
    assert emu.reload_decision(before, after) == "reload"
    write_u32(emu, OFF_MAGIC, MAILBOX_MAGIC)

    write_u16(emu, OFF_ABI_VERSION, MAILBOX_ABI_VERSION + 1)
    assert emu.reload_decision(before, after) == "reload"
    write_u16(emu, OFF_ABI_VERSION, MAILBOX_ABI_VERSION)

    write_u16(emu, OFF_STRUCT_SIZE, MAILBOX_SIZE - 4)
    assert emu.reload_decision(before, after) == "reload"
    write_u16(emu, OFF_STRUCT_SIZE, MAILBOX_SIZE)

    write_u32(emu, OFF_FIRMWARE_VERSION, MAILBOX_FIRMWARE_VERSION + 1)
    assert emu.reload_decision(before, after) == "reload"


def test_lp_emulator_rejects_stale_epoch_command_after_hp_reboot() -> None:
    emu = require_emulator()
    emu.boot()
    stale_epoch = emu.epoch

    emu.hp_reboot()
    assert emu.epoch != stale_epoch
    assert read_u32(emu, OFF_COMMAND_ACK_SEQUENCE) == 0

    inject_command(emu, epoch=stale_epoch, sequence=1, command_id=COMMAND_OPEN)
    emu.run(250_000)
    assert read_u32(emu, OFF_COMMAND_ACK_SEQUENCE) == 0

    command_result = emu.command("open")
    assert command_result == "OK ack=1"
    assert read_u32(emu, OFF_COMMAND_ACK_SEQUENCE) == 1


def test_lp_emulator_repeated_hp_reboots_clear_pending_commands() -> None:
    emu = require_emulator()
    emu.boot()

    for _ in range(3):
        before = emu.heartbeat()
        inject_command(emu, epoch=emu.epoch, sequence=99, command_id=COMMAND_OPEN)
        emu.hp_reboot()
        assert emu.heartbeat() > before
        assert read_u32(emu, OFF_COMMAND_SEQUENCE) == 0
        assert read_u32(emu, OFF_COMMAND_ACK_SEQUENCE) == 0
        assert emu.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ID, 1) == b"\x00"

        command_result = emu.command("light")
        assert command_result == "OK ack=1"


def test_dual_iss_shared_mailbox_health_and_torn_state_reads() -> None:
    dual = require_dual_iss()
    dual.boot_lp()

    before = read_u32(dual.lp, OFF_HEARTBEAT)
    dual.lp.run(100_000)
    after = read_u32(dual.lp, OFF_HEARTBEAT)
    assert after > before
    assert dual.probe_reload(before, after) == RELOAD_SKIP
    assert dual.probe_reload(after, after) == RELOAD_REQUIRED

    write_u32(dual.lp, OFF_STATE_SEQ, 1)
    assert dual.read_state(interleave_lp=False).result0 == 0

    write_u8(dual.lp, OFF_TARGET_POSITION, 200)
    write_u8(dual.lp, OFF_CURRENT_POSITION, 100)
    write_u8(dual.lp, OFF_STATE, 0x20)
    write_u8(dual.lp, OFF_LIGHT_ON, 1)
    write_u32(dual.lp, OFF_STATE_UPDATED_US, 123456)
    write_u32(dual.lp, OFF_STATE_SEQ, 2)
    state = dual.read_state(interleave_lp=False)
    assert state.result0 == 1
    assert state.result1 == 0x012064C8
    assert state.result2 == 123456


@pytest.mark.parametrize("slice_instructions", [1, 2, 8, 64])
def test_dual_iss_hp_reboot_clears_pending_command_before_new_epoch(slice_instructions: int) -> None:
    dual = require_dual_iss()
    dual.boot_lp()

    old_epoch = dual.begin_session(0x12340000, slice_instructions=slice_instructions)
    dual.lp.run(50_000)
    dual.inject_command(epoch=old_epoch, sequence=99, command_id=COMMAND_OPEN)

    new_epoch = dual.begin_session(0x56780000, slice_instructions=slice_instructions)
    assert new_epoch == 0x56780000
    assert read_u32(dual.lp, OFF_COMMAND_SEQUENCE) == 0
    assert read_u32(dual.lp, OFF_COMMAND_ACK_SEQUENCE) == 0
    assert dual.lp.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ID, 1) == b"\x00"

    dual.lp.run(250_000)
    assert read_u32(dual.lp, OFF_COMMAND_ACK_SEQUENCE) == 0

    sequence = dual.send_command(COMMAND_OPEN, slice_instructions=slice_instructions)
    assert sequence == 1
    assert dual.run_lp_until(lambda: read_u32(dual.lp, OFF_COMMAND_ACK_SEQUENCE) == sequence)
    assert dual.ack_received(sequence, slice_instructions=slice_instructions)
