from __future__ import annotations

import ctypes
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_RISCV, UC_MODE_RISCV32, UC_PROT_ALL, Uc, UcError
from unicorn.riscv_const import UC_RISCV_REG_PC, UC_RISCV_REG_SP

from .emulator import (
    LPEmuError,
    LPEmulator,
    LP_SRAM_BASE,
    LP_SRAM_MAP_SIZE,
    MAILBOX_ADDR,
    _read_u32,
    _write_u8,
    _write_u32,
    resolve_elf,
)


ROOT = Path(__file__).resolve().parents[2]
CORE_DIR = ROOT / "components" / "hcp2bridge" / "core"
BUILD_DIR = ROOT / "tests" / "hcp2" / "build" / "hp_iss"
HP_ISS_SOURCE = Path(__file__).with_name("hp_supervisor_iss.c")
HP_ISS_LINKER = Path(__file__).with_name("hp_supervisor_iss.ld")
HP_ISS_ELF = BUILD_DIR / "hcp2_hp_supervisor_iss.elf"

HP_RAM_BASE = 0x40800000
HP_RAM_SIZE = 0x10000
HP_CONTROL_ADDR = HP_RAM_BASE
HP_STACK_TOP = HP_RAM_BASE + HP_RAM_SIZE

REQ_BEGIN_SESSION = 1
REQ_PROBE_RELOAD = 2
REQ_SEND_COMMAND = 3
REQ_ACK_RECEIVED = 4
REQ_READ_STATE = 5

OFF_REQUEST = 0
OFF_RESPONSE = 4
OFF_ARG0 = 8
OFF_ARG1 = 12
OFF_ARG2 = 16
OFF_RESULT0 = 20
OFF_RESULT1 = 24
OFF_RESULT2 = 28
OFF_RESULT3 = 32

RELOAD_REQUIRED = 0
RELOAD_SKIP = 1

OFF_HEARTBEAT = 12
OFF_STATE_SEQ = 16
OFF_TARGET_POSITION = 20
OFF_CURRENT_POSITION = 21
OFF_STATE = 22
OFF_LIGHT_ON = 23
OFF_STATE_UPDATED_US = 24
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

COMMAND_OPEN = 1
COMMAND_RESULT_EXECUTED = 1


class HPIssToolchainMissing(LPEmuError):
    pass


def _candidate_compilers() -> list[str]:
    names = [
        "riscv32-esp-elf-gcc",
        "riscv64-unknown-elf-gcc",
        "riscv32-unknown-elf-gcc",
    ]
    candidates = [path for name in names if (path := shutil.which(name))]
    home = Path.home()
    candidates.extend(
        str(path)
        for path in (
            home / ".platformio" / "tools" / "toolchain-riscv32-esp" / "bin" / "riscv32-esp-elf-gcc",
            home / ".platformio" / "packages" / "toolchain-riscv32-esp" / "bin" / "riscv32-esp-elf-gcc",
        )
        if path.exists()
    )
    candidates.extend(str(path) for path in (home / ".espressif").glob("tools/**/riscv32-esp-elf-gcc"))
    seen: set[str] = set()
    deduped: list[str] = []
    for candidate in candidates:
        if candidate not in seen:
            deduped.append(candidate)
            seen.add(candidate)
    return deduped


def find_riscv_compiler() -> str | None:
    for candidate in _candidate_compilers():
        try:
            subprocess.run([candidate, "--version"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except (OSError, subprocess.CalledProcessError):
            continue
        return candidate
    return None


def build_hp_supervisor_elf(force: bool = False) -> Path:
    sources = [
        HP_ISS_SOURCE,
        CORE_DIR / "hcp2_mailbox.c",
        CORE_DIR / "hcp2_supervisor.c",
    ]
    if not force and HP_ISS_ELF.exists():
        output_mtime = HP_ISS_ELF.stat().st_mtime
        if all(path.stat().st_mtime <= output_mtime for path in sources + [HP_ISS_LINKER]):
            return HP_ISS_ELF

    compiler = find_riscv_compiler()
    if compiler is None:
        raise HPIssToolchainMissing("no RISC-V bare-metal GCC found for HP supervisor ISS")

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        compiler,
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Os",
        "-g",
        "-ffreestanding",
        "-fno-builtin",
        "-nostdlib",
        "-nostartfiles",
        "-march=rv32imac",
        "-mabi=ilp32",
        "-I",
        str(CORE_DIR),
        str(HP_ISS_SOURCE),
        str(CORE_DIR / "hcp2_mailbox.c"),
        str(CORE_DIR / "hcp2_supervisor.c"),
        "-T",
        str(HP_ISS_LINKER),
        "-Wl,--gc-sections",
        "-Wl,-Map," + str(HP_ISS_ELF.with_suffix(".map")),
        "-o",
        str(HP_ISS_ELF),
    ]
    subprocess.run(command, check=True)
    return HP_ISS_ELF


def _read_hp_u32(uc: Uc, offset: int) -> int:
    return _read_u32(uc, HP_CONTROL_ADDR + offset)


def _write_hp_u32(uc: Uc, offset: int, value: int) -> None:
    _write_u32(uc, HP_CONTROL_ADDR + offset, value)


@dataclass
class HPRequestResult:
    response: int
    result0: int
    result1: int
    result2: int
    result3: int


class HPSupervisorISS:
    def __init__(self, elf_path: Path, shared_lp_sram: ctypes.Array[ctypes.c_char]) -> None:
        self.elf_path = elf_path.resolve()
        self.shared_lp_sram = shared_lp_sram
        self.uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        self.entry = 0
        self.instructions = 0
        self._map_memory()
        self._load_elf()
        self.uc.reg_write(UC_RISCV_REG_SP, HP_STACK_TOP)
        self.uc.reg_write(UC_RISCV_REG_PC, self.entry)
        self.run(4096)

    @classmethod
    def build(cls, shared_lp_sram: ctypes.Array[ctypes.c_char]) -> "HPSupervisorISS":
        return cls(build_hp_supervisor_elf(), shared_lp_sram)

    def _map_memory(self) -> None:
        self.uc.mem_map(HP_RAM_BASE, HP_RAM_SIZE, UC_PROT_ALL)
        self.uc.mem_map_ptr(LP_SRAM_BASE, LP_SRAM_MAP_SIZE, UC_PROT_ALL, ctypes.addressof(self.shared_lp_sram))

    def _load_elf(self) -> None:
        with self.elf_path.open("rb") as f:
            elf = ELFFile(f)
            self.entry = int(elf.header.e_entry)
            for segment in elf.iter_segments():
                if segment["p_type"] != "PT_LOAD":
                    continue
                addr = int(segment["p_paddr"] or segment["p_vaddr"])
                data = segment.data()
                mem_size = int(segment["p_memsz"])
                if addr < HP_RAM_BASE or addr + mem_size > HP_RAM_BASE + HP_RAM_SIZE:
                    raise LPEmuError(f"HP PT_LOAD outside RAM: 0x{addr:08x}+0x{mem_size:x}")
                if data:
                    self.uc.mem_write(addr, data)
                if mem_size > len(data):
                    self.uc.mem_write(addr + len(data), b"\x00" * (mem_size - len(data)))

    def run(self, instruction_budget: int) -> None:
        if instruction_budget <= 0:
            return
        pc = self.uc.reg_read(UC_RISCV_REG_PC)
        try:
            self.uc.emu_start(pc, 0, count=instruction_budget)
        except UcError as exc:
            raise LPEmuError(f"HP Unicorn stopped at pc=0x{self.uc.reg_read(UC_RISCV_REG_PC):08x}: {exc}") from exc
        self.instructions += instruction_budget

    def request(self, request: int, arg0: int = 0, arg1: int = 0, arg2: int = 0) -> None:
        _write_hp_u32(self.uc, OFF_ARG0, arg0)
        _write_hp_u32(self.uc, OFF_ARG1, arg1)
        _write_hp_u32(self.uc, OFF_ARG2, arg2)
        _write_hp_u32(self.uc, OFF_RESULT0, 0)
        _write_hp_u32(self.uc, OFF_RESULT1, 0)
        _write_hp_u32(self.uc, OFF_RESULT2, 0)
        _write_hp_u32(self.uc, OFF_RESULT3, 0)
        _write_hp_u32(self.uc, OFF_RESPONSE, 0)
        _write_hp_u32(self.uc, OFF_REQUEST, request)

    def response(self) -> HPRequestResult | None:
        response = _read_hp_u32(self.uc, OFF_RESPONSE)
        if response == 0:
            return None
        return HPRequestResult(
            response=response,
            result0=_read_hp_u32(self.uc, OFF_RESULT0),
            result1=_read_hp_u32(self.uc, OFF_RESULT1),
            result2=_read_hp_u32(self.uc, OFF_RESULT2),
            result3=_read_hp_u32(self.uc, OFF_RESULT3),
        )


class DualISSHarness:
    def __init__(self, lp_blob: Path) -> None:
        self.shared_lp_sram = ctypes.create_string_buffer(LP_SRAM_MAP_SIZE)
        self.lp = LPEmulator(resolve_elf(lp_blob), shared_lp_sram=self.shared_lp_sram)
        self.hp = HPSupervisorISS.build(self.shared_lp_sram)

    @classmethod
    def from_blob(cls, lp_blob: Path) -> "DualISSHarness":
        return cls(lp_blob)

    def boot_lp(self) -> None:
        self.lp.boot()

    def run_interleaved_until_response(
        self,
        request: int,
        arg0: int = 0,
        arg1: int = 0,
        arg2: int = 0,
        *,
        slice_instructions: int = 64,
        lp_slice_instructions: int | None = None,
        rounds: int = 200000,
    ) -> HPRequestResult:
        lp_slice = slice_instructions if lp_slice_instructions is None else lp_slice_instructions
        self.hp.request(request, arg0, arg1, arg2)
        for _ in range(rounds):
            self.hp.run(slice_instructions)
            self.lp.run(lp_slice)
            response = self.hp.response()
            if response is not None:
                if response.response != request:
                    raise LPEmuError(f"HP ISS response mismatch: expected {request}, got {response.response}")
                return response
        raise LPEmuError(f"HP ISS request {request} timed out")

    def begin_session(self, epoch: int, *, slice_instructions: int = 64) -> int:
        return self.run_interleaved_until_response(
            REQ_BEGIN_SESSION,
            epoch,
            slice_instructions=slice_instructions,
        ).result0

    def probe_reload(
        self,
        heartbeat_before: int,
        heartbeat_after: int,
        *,
        before_polls: int = 0,
        slice_instructions: int = 64,
        lp_slice_instructions: int | None = None,
    ) -> int:
        return self.run_interleaved_until_response(
            REQ_PROBE_RELOAD,
            heartbeat_before,
            heartbeat_after,
            before_polls,
            slice_instructions=slice_instructions,
            lp_slice_instructions=lp_slice_instructions,
        ).result0

    def send_command(self, command_id: int, argument: int = 0, *, slice_instructions: int = 64) -> int:
        return self.run_interleaved_until_response(
            REQ_SEND_COMMAND,
            command_id,
            argument,
            slice_instructions=slice_instructions,
        ).result0

    def ack_received(self, sequence: int, *, slice_instructions: int = 64) -> bool:
        return (
            self.run_interleaved_until_response(
                REQ_ACK_RECEIVED,
                sequence,
                slice_instructions=slice_instructions,
            ).result0
            == 1
        )

    def ack_result(self, sequence: int, *, slice_instructions: int = 64) -> int:
        return self.run_interleaved_until_response(
            REQ_ACK_RECEIVED,
            sequence,
            slice_instructions=slice_instructions,
        ).result1

    def read_state(self, *, slice_instructions: int = 64, interleave_lp: bool = True) -> HPRequestResult:
        return self.run_interleaved_until_response(
            REQ_READ_STATE,
            slice_instructions=slice_instructions,
            lp_slice_instructions=slice_instructions if interleave_lp else 0,
        )

    def run_lp_until(self, condition, *, slice_instructions: int = 4096, rounds: int = 1000) -> bool:
        for _ in range(rounds):
            if condition():
                return True
            self.lp.run(slice_instructions)
        return condition()

    def inject_command(self, *, epoch: int, sequence: int, command_id: int, argument: int = 0) -> None:
        _write_u32(self.lp.uc, MAILBOX_ADDR + 28, epoch)
        _write_u8(self.lp.uc, MAILBOX_ADDR + 40, command_id)
        _write_u8(self.lp.uc, MAILBOX_ADDR + 41, argument)
        _write_u8(self.lp.uc, MAILBOX_ADDR + OFF_COMMAND_ACK_RESULT, 0)
        _write_u32(self.lp.uc, MAILBOX_ADDR + OFF_COMMAND_DEADLINE_US, _read_u32(self.lp.uc, MAILBOX_ADDR + OFF_LP_TIME_US) + 250_000)
        _write_u32(self.lp.uc, MAILBOX_ADDR + 32, sequence)


def _mailbox_u32(harness: DualISSHarness, offset: int) -> int:
    return _read_u32(harness.lp.uc, MAILBOX_ADDR + offset)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise LPEmuError(message)


def run_mailbox_suite(lp_blob: Path, *, interleave: int = 64) -> dict[str, object]:
    if interleave < 1:
        raise LPEmuError("--interleave must be positive")

    checks: list[str] = []

    dual = DualISSHarness.from_blob(lp_blob)
    dual.boot_lp()

    before = _mailbox_u32(dual, OFF_HEARTBEAT)
    dual.lp.run(100_000)
    after = _mailbox_u32(dual, OFF_HEARTBEAT)
    _require(after > before, "LP heartbeat did not advance")
    _require(dual.probe_reload(before, after, slice_instructions=interleave) == RELOAD_SKIP, "healthy LP was not skipped")
    _require(
        dual.probe_reload(after, after, slice_instructions=interleave) == RELOAD_REQUIRED,
        "stale heartbeat did not require reload",
    )
    checks.append("heartbeat_reload_decision")

    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_HEARTBEAT, 20)
    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_POLLS_SEEN, 4)
    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_POLLS_ANSWERED, 4)
    _require(
        dual.probe_reload(19, 20, before_polls=4, slice_instructions=interleave, lp_slice_instructions=0) ==
        RELOAD_REQUIRED,
        "stalled poll counter did not require reload",
    )
    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_HEARTBEAT, 21)
    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_POLLS_SEEN, 5)
    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_POLLS_ANSWERED, 5)
    _require(
        dual.probe_reload(20, 21, before_polls=4, slice_instructions=interleave, lp_slice_instructions=0) ==
        RELOAD_SKIP,
        "advancing poll counters did not stay healthy",
    )
    checks.append("poll_progress_health")

    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_STATE_SEQ, 1)
    _require(dual.read_state(slice_instructions=interleave, interleave_lp=False).result0 == 0, "torn state read succeeded")

    _write_u8(dual.lp.uc, MAILBOX_ADDR + OFF_TARGET_POSITION, 200)
    _write_u8(dual.lp.uc, MAILBOX_ADDR + OFF_CURRENT_POSITION, 100)
    _write_u8(dual.lp.uc, MAILBOX_ADDR + OFF_STATE, 0x20)
    _write_u8(dual.lp.uc, MAILBOX_ADDR + OFF_LIGHT_ON, 1)
    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_STATE_UPDATED_US, 123456)
    _write_u32(dual.lp.uc, MAILBOX_ADDR + OFF_STATE_SEQ, 2)
    state = dual.read_state(slice_instructions=interleave, interleave_lp=False)
    _require(state.result0 == 1, "valid state read failed")
    _require(state.result1 == 0x012064C8, f"state packing mismatch: 0x{state.result1:08x}")
    _require(state.result2 == 123456, f"state timestamp mismatch: {state.result2}")
    checks.append("state_seqlock")

    old_epoch = dual.begin_session(0x12340000, slice_instructions=interleave)
    dual.lp.run(50_000)
    dual.inject_command(epoch=old_epoch, sequence=99, command_id=COMMAND_OPEN)

    new_epoch = dual.begin_session(0x56780000, slice_instructions=interleave)
    _require(new_epoch == 0x56780000, f"new epoch mismatch: 0x{new_epoch:08x}")
    _require(_mailbox_u32(dual, OFF_COMMAND_SEQUENCE) == 0, "pending command sequence was not cleared")
    _require(_mailbox_u32(dual, OFF_COMMAND_ACK_SEQUENCE) == 0, "pending command ack was not cleared")
    _require(dual.lp.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ID, 1) == b"\x00", "pending command id was not cleared")
    _require(dual.lp.uc.mem_read(MAILBOX_ADDR + OFF_COMMAND_ACK_RESULT, 1) == b"\x00", "ack result was not cleared")

    dual.lp.run(250_000)
    _require(_mailbox_u32(dual, OFF_COMMAND_ACK_SEQUENCE) == 0, "stale epoch command was acknowledged")

    sequence = dual.send_command(COMMAND_OPEN, slice_instructions=interleave)
    _require(sequence == 1, f"first command sequence mismatch: {sequence}")
    _require(
        dual.run_lp_until(lambda: _mailbox_u32(dual, OFF_COMMAND_ACK_SEQUENCE) == sequence),
        "fresh command was not acknowledged by LP",
    )
    _require(dual.ack_received(sequence, slice_instructions=interleave), "HP did not observe fresh command ack")
    _require(
        dual.ack_result(sequence, slice_instructions=interleave) == COMMAND_RESULT_EXECUTED,
        "fresh command ack result was not executed",
    )
    checks.append("epoch_replay")

    return {
        "verdict": "ok",
        "suite": "mailbox",
        "interleave": interleave,
        "checks": checks,
        "lp_emu": dual.lp.report(),
    }
