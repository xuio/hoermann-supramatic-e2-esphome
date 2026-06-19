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
from tools.supramatic_sim import protocol


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
OFF_COMMAND_ACK_RESULT = 42
OFF_COMMAND_DEADLINE_US = 44
OFF_LP_TIME_US = 48
OFF_POLLS_SEEN = 52
OFF_POLLS_ANSWERED = 56
OFF_TX_ABORT_COUNT = 60
OFF_COLLISION_COUNT = 64
OFF_MAX_DE_HOLD_US = 68
OFF_LP_RESET_COUNT = 72
OFF_LAST_POLL_US = 76
OFF_CRC_ERROR_COUNT = 80
OFF_RX_ERROR_COUNT = 84
OFF_STOP_TRIGGER_EPOCH = 88
OFF_STOP_TRIGGER_DEADLINE_US = 92
OFF_STOP_TRIGGER_TARGET_POSITION = 96
OFF_STOP_TRIGGER_ARMED = 97
OFF_STOP_TRIGGER_FIRE_COUNT = 98
OFF_HEALTH_FLAGS = 100
OFF_MAX_RX_FIFO_COUNT = 102
OFF_MAX_LOOP_US = 104
OFF_LOOP_OVERRUN_COUNT = 108
OFF_RX_STARVATION_COUNT = 112
OFF_STUCK_DE_COUNT = 116
OFF_MAILBOX_REPAIR_COUNT = 120
OFF_MAX_POLL_RX_TO_SCHEDULE_US = 124
OFF_MAX_RESPONSE_SCHEDULE_TO_TX_START_US = 128
OFF_MAX_RESPONSE_TX_US = 132

COMMAND_OPEN = 1
COMMAND_RESULT_EXECUTED = 1
COMMAND_RESULT_EXPIRED = 2
HEALTH_FLAG_MAILBOX_REPAIR = 0x0008


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


def read_u16(emu: LPEmulator, offset: int) -> int:
    return struct.unpack("<H", emu.uc.mem_read(MAILBOX_ADDR + offset, 2))[0]


def broadcast_scan_request(candidate_slave_id: int) -> bytes:
    return protocol.bus_scan_request(0, scan_state=0x01, scan_value=candidate_slave_id)


def inject_command(emu: LPEmulator, *, epoch: int, sequence: int, command_id: int) -> None:
    write_u32(emu, OFF_COMMAND_EPOCH, epoch)
    write_u8(emu, OFF_COMMAND_ID, command_id)
    write_u8(emu, OFF_COMMAND_ARGUMENT, 0)
    write_u8(emu, OFF_COMMAND_ACK_RESULT, 0)
    write_u32(emu, OFF_COMMAND_DEADLINE_US, read_u32(emu, OFF_LP_TIME_US) + 250_000)
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


def test_lp_emulator_health_counters_have_headroom_on_normal_poll() -> None:
    emu = require_emulator()
    emu.boot()
    emu.write_uart(bytes.fromhex("02179CB900089C410002043E030000EBCC"))
    assert emu.run_until(lambda: read_u32(emu, OFF_POLLS_ANSWERED) > 0, instruction_budget=4_000_000)

    assert read_u32(emu, OFF_MAX_LOOP_US) > 0
    assert read_u32(emu, OFF_LOOP_OVERRUN_COUNT) == 0
    assert read_u32(emu, OFF_TX_ABORT_COUNT) == 0
    assert read_u32(emu, OFF_STUCK_DE_COUNT) == 0
    assert read_u32(emu, OFF_MAX_RESPONSE_SCHEDULE_TO_TX_START_US) >= 4200
    assert read_u32(emu, OFF_MAX_RESPONSE_TX_US) > 0


def test_lp_emulator_uses_mailbox_slave_id_one_for_scan_and_polls() -> None:
    emu = require_emulator()
    emu.configure_hcp2(slave_id=1)
    emu.boot()

    emu.write_uart(protocol.bus_scan_request(1))
    response = emu.read_uart_available(0.05)
    assert response == protocol.scan_response(1)

    emu.write_uart(protocol.status_poll(0x21, slave_id=1))
    assert emu.run_until(lambda: read_u32(emu, OFF_POLLS_ANSWERED) >= 1, instruction_budget=4_000_000)
    response = emu.read_uart_available(0.05)
    assert protocol.crc_ok(response)
    assert protocol.decode_response_kind(response) == "status"
    assert protocol.response_counter(response) == 0x21
    assert response[0] == 1

    polls_seen = read_u32(emu, OFF_POLLS_SEEN)
    polls_answered = read_u32(emu, OFF_POLLS_ANSWERED)
    emu.write_uart(protocol.status_poll(0x22, slave_id=2))
    emu.run(250_000)
    assert emu.read_uart_available(0.01) == b""
    assert read_u32(emu, OFF_POLLS_SEEN) == polls_seen
    assert read_u32(emu, OFF_POLLS_ANSWERED) == polls_answered

    emu.write_uart(broadcast_scan_request(2))
    assert emu.read_uart_available(0.02) == b""

    emu.write_uart(broadcast_scan_request(1))
    assert emu.read_uart_available(0.02) == b""


def test_lp_emulator_accepts_reported_bus_scan_tail_variants() -> None:
    emu = require_emulator()
    emu.boot()

    for frame in (
        protocol.bus_scan_request(2),
        protocol.bus_scan_request(2, scan_state=0x02, scan_value=0x00),
        protocol.bus_scan_request(2, scan_state=0x01, scan_value=0x00),
    ):
        emu.write_uart(frame)
        assert emu.read_uart_available(0.05) == protocol.scan_response(2)

    emu.write_uart(protocol.bus_scan_request(1, scan_state=0x02, scan_value=0x00))
    assert emu.read_uart_available(0.02) == b""


def test_lp_emulator_repairs_mailbox_header_without_clearing_session() -> None:
    emu = require_emulator()
    emu.boot()
    write_u32(emu, OFF_COMMAND_EPOCH, 0xAABBCCDD)
    before_repairs = read_u32(emu, OFF_MAILBOX_REPAIR_COUNT)

    write_u16(emu, OFF_ABI_VERSION, MAILBOX_ABI_VERSION + 1)
    assert emu.reload_decision(emu.heartbeat() - 1, emu.heartbeat()) == "reload"

    assert emu.run_until(
        lambda: read_u16(emu, OFF_ABI_VERSION) == MAILBOX_ABI_VERSION
        and read_u32(emu, OFF_MAILBOX_REPAIR_COUNT) > before_repairs,
        instruction_budget=1_000_000,
    )
    assert read_u32(emu, OFF_COMMAND_EPOCH) == 0xAABBCCDD
    assert read_u32(emu, OFF_MAGIC) == MAILBOX_MAGIC
    assert read_u32(emu, OFF_FIRMWARE_VERSION) == MAILBOX_FIRMWARE_VERSION
    assert read_u16(emu, OFF_HEALTH_FLAGS) & HEALTH_FLAG_MAILBOX_REPAIR


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
    assert command_result == "OK ack=1 result=1"
    assert read_u32(emu, OFF_COMMAND_ACK_SEQUENCE) == 1
    assert emu.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ACK_RESULT, 1) == bytes([COMMAND_RESULT_EXECUTED])


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
    assert command_result == "OK ack=1 result=1"


def test_lp_emulator_expires_stale_command() -> None:
    emu = require_emulator()
    emu.boot()
    emu.hp_reboot()
    write_u8(emu, OFF_COMMAND_ID, COMMAND_OPEN)
    write_u8(emu, OFF_COMMAND_ARGUMENT, 0)
    write_u8(emu, OFF_COMMAND_ACK_RESULT, 0)
    write_u32(emu, OFF_COMMAND_DEADLINE_US, max(1, read_u32(emu, OFF_LP_TIME_US) - 1))
    write_u32(emu, OFF_COMMAND_SEQUENCE, 1)

    assert emu.run_until(lambda: read_u32(emu, OFF_COMMAND_ACK_SEQUENCE) == 1, instruction_budget=1_000_000)
    assert emu.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ACK_RESULT, 1) == bytes([COMMAND_RESULT_EXPIRED])


def test_lp_emulator_lp_reset_expires_pending_command_without_replay() -> None:
    emu = require_emulator()
    emu.boot()
    emu.hp_reboot()
    reset_before = read_u32(emu, OFF_LP_RESET_COUNT)
    inject_command(emu, epoch=emu.epoch, sequence=7, command_id=COMMAND_OPEN)

    emu.lp_reset()

    assert read_u32(emu, OFF_LP_RESET_COUNT) > reset_before
    assert read_u32(emu, OFF_COMMAND_ACK_SEQUENCE) == 7
    assert emu.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ACK_RESULT, 1) == bytes([COMMAND_RESULT_EXPIRED])
    emu.run(250_000)
    assert read_u32(emu, OFF_COMMAND_ACK_SEQUENCE) == 7
    assert emu.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ACK_RESULT, 1) == bytes([COMMAND_RESULT_EXPIRED])

    assert emu.command("open") == "OK ack=1 result=1"


def test_lp_emulator_tx_deadman_releases_de_when_fifo_wedged() -> None:
    emu = require_emulator()
    emu.boot()
    emu.tx_fifo_wedged = True
    emu.write_uart(bytes.fromhex("02179CB900089C410002043E030000EBCC"))
    emu.run_until(lambda: read_u32(emu, OFF_TX_ABORT_COUNT) > 0, instruction_budget=4_000_000)

    assert read_u32(emu, OFF_TX_ABORT_COUNT) > 0
    assert read_u32(emu, OFF_MAX_DE_HOLD_US) <= 9000
    assert emu.de_events[-1]["enabled"] is False


def test_lp_emulator_ignores_tx_echo_when_receiver_disabled() -> None:
    emu = require_emulator()
    emu.boot()
    emu.echo_mismatch_at = 3
    emu.write_uart(bytes.fromhex("02179CB900089C410002043E030000EBCC"))
    emu.run_until(lambda: read_u32(emu, OFF_POLLS_ANSWERED) > 0, instruction_budget=4_000_000)

    assert read_u32(emu, OFF_POLLS_ANSWERED) == 1
    assert read_u32(emu, OFF_TX_ABORT_COUNT) == 0
    assert emu.de_events[-1]["enabled"] is False


def test_lp_emulator_stop_trigger_fires_once_on_crossing_broadcast() -> None:
    emu = require_emulator()
    emu.boot()
    emu.hp_reboot()
    now_us = read_u32(emu, OFF_LP_TIME_US)
    write_u32(emu, OFF_STOP_TRIGGER_EPOCH, emu.epoch)
    write_u32(emu, OFF_STOP_TRIGGER_DEADLINE_US, now_us + 1_000_000)
    write_u8(emu, OFF_STOP_TRIGGER_TARGET_POSITION, 100)
    write_u8(emu, OFF_STOP_TRIGGER_ARMED, 1)

    emu.write_uart(protocol.broadcast_status(protocol.BroadcastState(target=200, current=104, state=0x01)))
    emu.run(200_000)
    emu.write_uart(protocol.status_poll(0x33))
    assert emu.run_until(lambda: read_u32(emu, OFF_POLLS_ANSWERED) >= 1, instruction_budget=4_000_000)
    response = emu.read_uart_available(0.05)

    assert protocol.crc_ok(response)
    assert protocol.decode_response_kind(response) == "status"
    assert protocol.response_counter(response) == 0x33
    assert protocol.decode_status_button(response) == "stop"
    assert emu.uc.mem_read(MAILBOX_ADDR + OFF_STOP_TRIGGER_ARMED, 1) == b"\x00"
    assert read_u16(emu, OFF_STOP_TRIGGER_FIRE_COUNT) == 1

    emu.write_uart(protocol.status_poll(0x34))
    assert emu.run_until(lambda: read_u32(emu, OFF_POLLS_ANSWERED) >= 2, instruction_budget=4_000_000)
    response = emu.read_uart_available(0.05)
    assert protocol.decode_status_button(response) == "stop"

    emu.run(2_500_000)
    emu.write_uart(protocol.status_poll(0x35))
    assert emu.run_until(lambda: read_u32(emu, OFF_POLLS_ANSWERED) >= 3, instruction_budget=4_000_000)
    response = emu.read_uart_available(0.05)
    assert protocol.decode_status_button(response) == "stop"

    emu.write_uart(protocol.status_poll(0x36))
    assert emu.run_until(lambda: read_u32(emu, OFF_POLLS_ANSWERED) >= 4, instruction_budget=4_000_000)
    response = emu.read_uart_available(0.05)
    assert protocol.decode_status_button(response) is None
    assert read_u16(emu, OFF_STOP_TRIGGER_FIRE_COUNT) == 1


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
    assert dual.lp.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ACK_RESULT, 1) == b"\x00"
    assert dual.lp.uc.mem_read(MAILBOX_ADDR + OFF_STOP_TRIGGER_ARMED, 1) == b"\x00"

    dual.lp.run(250_000)
    assert read_u32(dual.lp, OFF_COMMAND_ACK_SEQUENCE) == 0

    sequence = dual.send_command(COMMAND_OPEN, slice_instructions=slice_instructions)
    assert sequence == 1
    assert dual.run_lp_until(lambda: read_u32(dual.lp, OFF_COMMAND_ACK_SEQUENCE) == sequence)
    assert dual.ack_received(sequence, slice_instructions=slice_instructions)
    assert dual.ack_result(sequence, slice_instructions=slice_instructions) == COMMAND_RESULT_EXECUTED
