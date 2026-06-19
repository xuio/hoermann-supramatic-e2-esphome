from __future__ import annotations

import json
import struct
import ctypes
from collections import Counter, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from elftools.elf.elffile import ELFFile
from unicorn import (
    UC_ARCH_RISCV,
    UC_HOOK_BLOCK,
    UC_HOOK_CODE,
    UC_HOOK_MEM_INVALID,
    UC_HOOK_MEM_READ,
    UC_HOOK_MEM_WRITE,
    UC_MODE_RISCV32,
    UC_PROT_ALL,
    Uc,
    UcError,
)
from unicorn.riscv_const import (
    UC_RISCV_REG_A0,
    UC_RISCV_REG_CYCLE,
    UC_RISCV_REG_CYCLEH,
    UC_RISCV_REG_MCYCLE,
    UC_RISCV_REG_MCYCLEH,
    UC_RISCV_REG_PC,
    UC_RISCV_REG_RA,
)


ROOT = Path(__file__).resolve().parents[2]

LP_CLOCK_HZ = 20_000_000
UART_BITS_PER_BYTE = 11
UART_BAUD = 57_600
UART_BYTE_CYCLES = round(LP_CLOCK_HZ * UART_BITS_PER_BYTE / UART_BAUD)

LP_SRAM_BASE = 0x50000000
LP_SRAM_MAP_SIZE = 0x10000
MAILBOX_ADDR = 0x50002400
MAILBOX_SIZE = 1280
MAILBOX_MAGIC = 0x32435048
MAILBOX_ABI_VERSION = 8
MAILBOX_FIRMWARE_VERSION = 17
MAILBOX_CONFIG_SEQUENCE = 1156
MAILBOX_CONFIG_SLAVE_ID = 1160
MAILBOX_CONFIG_SIGNATURE = 1161
MAILBOX_CONFIG_RESPONSE_DELAY_US = 1172
MAILBOX_CONFIG_BUTTON_PRESS_US = 1176
DEFAULT_SCAN_SIGNATURE = bytes.fromhex("00000205043010FFA845")

LP_UART_BASE = 0x600B1400
LP_UART_FIFO = LP_UART_BASE + 0x00
LP_UART_INT_RAW = LP_UART_BASE + 0x04
LP_UART_INT_ST = LP_UART_BASE + 0x08
LP_UART_INT_ENA = LP_UART_BASE + 0x0C
LP_UART_INT_CLR = LP_UART_BASE + 0x10
LP_UART_STATUS = LP_UART_BASE + 0x1C
LP_UART_CONF0 = LP_UART_BASE + 0x20
LP_UART_CONF1 = LP_UART_BASE + 0x24
LP_UART_TOUT_CONF = LP_UART_BASE + 0x64
LP_UART_FSM_STATUS = LP_UART_BASE + 0x70
LP_UART_REG_UPDATE = LP_UART_BASE + 0x98

UART_INTR_RXFIFO_FULL = 1 << 0
UART_INTR_TXFIFO_EMPTY = 1 << 1
UART_INTR_PARITY_ERR = 1 << 2
UART_INTR_FRAM_ERR = 1 << 3
UART_INTR_RXFIFO_OVF = 1 << 4
UART_INTR_RXFIFO_TOUT = 1 << 8
UART_INTR_TX_DONE = 1 << 14

LP_IO_BASE = 0x600B2000
LP_IO_OUT_DATA = LP_IO_BASE + 0x00
LP_IO_OUT_DATA_W1TS = LP_IO_BASE + 0x04
LP_IO_OUT_DATA_W1TC = LP_IO_BASE + 0x08
LP_IO_OUT_ENABLE_W1TS = LP_IO_BASE + 0x10
LP_IO_STATUS_W1TC = LP_IO_BASE + 0x20
LP_IO_DE_MASK = 1 << 0
LP_IO_RE_MASK = 1 << 1

LPPERI_BASE = 0x600B2800
LPPERI_CLK_EN = LPPERI_BASE + 0x00
LPPERI_RESET_EN = LPPERI_BASE + 0x04
LPPERI_INTERRUPT_SOURCE = LPPERI_BASE + 0x20
LPPERI_LP_UART_CLK_BIT = 1 << 26

PMU_BASE = 0x600B0000
LP_TIMER_BASE = 0x600B0C00
LP_I2C_ANA_MST_BASE = 0x600B1000
PCR_BASE = 0x60096000

MODELED_MMIO_PAGES = {
    base & ~0xFFF
    for base in (
        PMU_BASE,
        LP_TIMER_BASE,
        LP_I2C_ANA_MST_BASE,
        LP_UART_BASE,
        LP_IO_BASE,
        LPPERI_BASE,
        PCR_BASE,
    )
}

COMMAND_IDS = {
    "open": 1,
    "close": 2,
    "stop": 3,
    "vent": 4,
    "half": 5,
    "light": 6,
}


@dataclass(frozen=True)
class SectionRange:
    name: str
    start: int
    size: int

    @property
    def end(self) -> int:
        return self.start + self.size


@dataclass
class LayoutReport:
    entry: int
    load_end_before_mailbox: int
    mailbox_addr: int
    mailbox_size: int
    code_data_headroom_bytes: int
    stack_top: int
    stack_headroom_bytes: int
    sections: list[dict[str, int | str]]

    def as_dict(self) -> dict[str, object]:
        return {
            "entry": self.entry,
            "load_end_before_mailbox": self.load_end_before_mailbox,
            "mailbox_addr": self.mailbox_addr,
            "mailbox_size": self.mailbox_size,
            "code_data_headroom_bytes": self.code_data_headroom_bytes,
            "stack_top": self.stack_top,
            "stack_headroom_bytes": self.stack_headroom_bytes,
            "sections": self.sections,
        }


class LPEmuError(RuntimeError):
    pass


def rvc_smoke() -> bool:
    uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    base = LP_SRAM_BASE
    uc.mem_map(base, 0x1000, UC_PROT_ALL)
    uc.mem_write(base, b"\x01\x00")  # c.nop
    uc.reg_write(UC_RISCV_REG_PC, base)
    uc.emu_start(base, 0, count=1)
    return uc.reg_read(UC_RISCV_REG_PC) == base + 2


def resolve_elf(blob: Path) -> Path:
    blob = blob.resolve()
    if blob.suffix == ".elf" and blob.exists():
        return blob

    candidates = [
        blob.with_suffix(".elf"),
        blob.parent / "esp-idf" / "main" / "hcp2_lp" / "hcp2_lp.elf",
        blob.parent / "hcp2_lp.elf",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    raise FileNotFoundError(f"could not find LP ELF next to {blob}")


def _write_u32(uc: Uc, addr: int, value: int) -> None:
    uc.mem_write(addr, struct.pack("<I", value & 0xFFFFFFFF))


def _read_u32(uc: Uc, addr: int) -> int:
    return struct.unpack("<I", uc.mem_read(addr, 4))[0]


def _write_u16(uc: Uc, addr: int, value: int) -> None:
    uc.mem_write(addr, struct.pack("<H", value & 0xFFFF))


def _write_u8(uc: Uc, addr: int, value: int) -> None:
    uc.mem_write(addr, bytes([value & 0xFF]))


def _read_u32_mem(uc: Uc, addr: int) -> int:
    return struct.unpack("<I", uc.mem_read(addr, 4))[0]


class LPEmulator:
    def __init__(self, elf_path: Path, shared_lp_sram: ctypes.Array[ctypes.c_char] | None = None) -> None:
        self.elf_path = elf_path.resolve()
        self.uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        self.shared_lp_sram = shared_lp_sram
        self.entry = LP_SRAM_BASE + 0x80
        self.symbols: dict[str, int] = {}
        self.sections: list[SectionRange] = []
        self.layout = LayoutReport(
            entry=self.entry,
            load_end_before_mailbox=LP_SRAM_BASE,
            mailbox_addr=MAILBOX_ADDR,
            mailbox_size=MAILBOX_SIZE,
            code_data_headroom_bytes=0,
            stack_top=LP_SRAM_BASE,
            stack_headroom_bytes=0,
            sections=[],
        )

        self.cycles = 0
        self.instructions = 0
        self.blocks = 0
        self.fast_forward_cycles = 0
        self.last_cycle_sync = 0
        self.running = False

        self.rx_fifo: deque[int] = deque()
        self.rx_wire: deque[tuple[int, int]] = deque()
        self.rx_last_due_cycle = 0
        self.tx_fifo: deque[int] = deque()
        self.tx_next_due_cycle: int | None = None
        self.tx_output = bytearray()
        self.tx_first_cycle: int | None = None
        self.echo_tx_to_rx = True
        self.echo_mismatch_at: int | None = None
        self.echo_count = 0
        self.tx_fifo_wedged = False
        self.last_master_write_cycle: int | None = None
        self.reply_latencies_us: list[float] = []

        self.de_enabled = False
        self.re_disabled = False
        self.de_events: list[dict[str, int | bool]] = []
        self.re_events: list[dict[str, int | bool]] = []
        self.gpio_writes: list[dict[str, int | str]] = []
        self.trace_events: list[dict[str, object]] = []
        self.tx_when_de_low = 0
        self.rx_overflows = 0
        self.tx_overflows = 0
        self.rx_high_water = 0
        self.tx_high_water = 0
        self.max_tx_write_burst = 0
        self.unknown_mmio: list[str] = []
        self.mmio_manifest: Counter[str] = Counter()

        self.epoch = 1
        self.command_sequence = 0

        self._map_memory()
        self._load_elf()
        self._check_layout()
        self._init_mmio()
        self._init_mailbox()
        self._install_hooks()
        self.uc.reg_write(UC_RISCV_REG_PC, self.entry)

    @classmethod
    def from_blob(cls, blob: Path, shared_lp_sram: ctypes.Array[ctypes.c_char] | None = None) -> "LPEmulator":
        return cls(resolve_elf(blob), shared_lp_sram=shared_lp_sram)

    @property
    def time_us(self) -> float:
        return (self.cycles * 1_000_000.0) / LP_CLOCK_HZ

    def _trace(self, event_type: str, **fields: object) -> None:
        event: dict[str, object] = {
            "source": "iss",
            "type": event_type,
            "t_us": round(self.time_us),
        }
        event.update(fields)
        self.trace_events.append(event)

    def _map_memory(self) -> None:
        if self.shared_lp_sram is None:
            self.uc.mem_map(LP_SRAM_BASE, LP_SRAM_MAP_SIZE, UC_PROT_ALL)
        else:
            self.uc.mem_map_ptr(
                LP_SRAM_BASE,
                LP_SRAM_MAP_SIZE,
                UC_PROT_ALL,
                ctypes.addressof(self.shared_lp_sram),
            )
        for page in sorted(MODELED_MMIO_PAGES):
            self.uc.mem_map(page, 0x1000, UC_PROT_ALL)

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
                if addr < LP_SRAM_BASE or addr + mem_size > LP_SRAM_BASE + LP_SRAM_MAP_SIZE:
                    raise LPEmuError(f"PT_LOAD outside LP SRAM: 0x{addr:08x}+0x{mem_size:x}")
                if data:
                    self.uc.mem_write(addr, data)
                if mem_size > len(data):
                    self.uc.mem_write(addr + len(data), b"\x00" * (mem_size - len(data)))

            symtab = elf.get_section_by_name(".symtab")
            if symtab is not None:
                self.symbols = {sym.name: int(sym["st_value"]) for sym in symtab.iter_symbols() if sym.name}

            for section in elf.iter_sections():
                flags = int(section["sh_flags"])
                size = int(section["sh_size"])
                addr = int(section["sh_addr"])
                if (flags & 0x2) and size and LP_SRAM_BASE <= addr < LP_SRAM_BASE + LP_SRAM_MAP_SIZE:
                    self.sections.append(SectionRange(section.name, addr, size))

    def _check_layout(self) -> None:
        mailbox_end = MAILBOX_ADDR + MAILBOX_SIZE
        low_end = LP_SRAM_BASE
        section_rows: list[dict[str, int | str]] = []
        for section in self.sections:
            section_rows.append({"name": section.name, "start": section.start, "end": section.end, "size": section.size})
            overlaps_mailbox = section.start < mailbox_end and section.end > MAILBOX_ADDR
            if overlaps_mailbox:
                raise LPEmuError(
                    f"LP section {section.name} overlaps mailbox: "
                    f"0x{section.start:08x}-0x{section.end:08x}"
                )
            if section.end <= MAILBOX_ADDR and section.name != ".shared_mem":
                low_end = max(low_end, section.end)

        stack_top = self.symbols.get("s_shared_mem", LP_SRAM_BASE + 0x3FB0)
        stack_headroom = stack_top - mailbox_end
        if stack_headroom < 2048:
            raise LPEmuError(f"LP stack headroom below mailbox is too small: {stack_headroom} bytes")
        self.layout = LayoutReport(
            entry=self.entry,
            load_end_before_mailbox=low_end,
            mailbox_addr=MAILBOX_ADDR,
            mailbox_size=MAILBOX_SIZE,
            code_data_headroom_bytes=MAILBOX_ADDR - low_end,
            stack_top=stack_top,
            stack_headroom_bytes=stack_headroom,
            sections=section_rows,
        )

    def _init_mmio(self) -> None:
        _write_u32(self.uc, LPPERI_CLK_EN, LPPERI_LP_UART_CLK_BIT)
        _write_u32(self.uc, LPPERI_RESET_EN, 0)
        _write_u32(self.uc, LP_UART_CONF0, (3 << 2) | (2 << 1))  # 8 data bits, parity enabled
        _write_u32(self.uc, LP_UART_CONF1, 10 << 3)
        _write_u32(self.uc, LP_UART_REG_UPDATE, 0)
        self._sync_uart_regs()

    def _init_mailbox(self) -> None:
        self.uc.mem_write(MAILBOX_ADDR, b"\x00" * MAILBOX_SIZE)
        _write_u32(self.uc, MAILBOX_ADDR + 0, MAILBOX_MAGIC)
        _write_u16(self.uc, MAILBOX_ADDR + 4, MAILBOX_ABI_VERSION)
        _write_u16(self.uc, MAILBOX_ADDR + 6, MAILBOX_SIZE)
        _write_u32(self.uc, MAILBOX_ADDR + 8, MAILBOX_FIRMWARE_VERSION)
        _write_u32(self.uc, MAILBOX_ADDR + 28, self.epoch)
        _write_u32(self.uc, MAILBOX_ADDR + 32, 0)

    def configure_hcp2(
        self,
        *,
        slave_id: int = 2,
        signature: bytes = DEFAULT_SCAN_SIGNATURE,
        response_delay_us: int = 4200,
        button_press_us: int = 100_000,
    ) -> None:
        if len(signature) != 10:
            raise ValueError("signature must contain exactly 10 bytes")

        sequence = _read_u32(self.uc, MAILBOX_ADDR + MAILBOX_CONFIG_SEQUENCE) + 1
        if sequence % 2 == 0:
            sequence += 1
        _write_u32(self.uc, MAILBOX_ADDR + MAILBOX_CONFIG_SEQUENCE, sequence)
        _write_u8(self.uc, MAILBOX_ADDR + MAILBOX_CONFIG_SLAVE_ID, slave_id)
        self.uc.mem_write(MAILBOX_ADDR + MAILBOX_CONFIG_SIGNATURE, signature)
        _write_u8(self.uc, MAILBOX_ADDR + MAILBOX_CONFIG_SIGNATURE + len(signature), 0)
        _write_u32(self.uc, MAILBOX_ADDR + MAILBOX_CONFIG_RESPONSE_DELAY_US, response_delay_us)
        _write_u32(self.uc, MAILBOX_ADDR + MAILBOX_CONFIG_BUTTON_PRESS_US, button_press_us)
        _write_u32(self.uc, MAILBOX_ADDR + MAILBOX_CONFIG_SEQUENCE, sequence + 1)

    def _install_hooks(self) -> None:
        self.uc.hook_add(UC_HOOK_MEM_READ, self._hook_mem_read)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._hook_mem_write)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self._hook_invalid_mem)
        self.uc.hook_add(UC_HOOK_BLOCK, self._hook_block)
        delay_addrs = [
            self.symbols.get("ulp_lp_core_delay_us"),
            self.symbols.get("ulp_lp_core_delay_cycles"),
            self.symbols.get("hcp2_lp_now_us"),
        ]
        delay_addrs = [addr for addr in delay_addrs if addr is not None]
        if delay_addrs:
            self.uc.hook_add(UC_HOOK_CODE, self._hook_delay_code, begin=min(delay_addrs), end=max(delay_addrs))

    def _hook_block(self, uc: Uc, address: int, size: int, user_data: object | None = None) -> None:
        self.blocks += 1
        approx_instructions = max(1, size // 2)
        self.instructions += approx_instructions
        self.cycles += approx_instructions
        self._write_cycle_regs()
        if self.cycles - self.last_cycle_sync >= 128:
            self._service_serial()
            self.last_cycle_sync = self.cycles

    def _hook_delay_code(self, uc: Uc, address: int, size: int, user_data: object | None = None) -> None:
        if address == self.symbols.get("ulp_lp_core_delay_us"):
            requested_us = uc.reg_read(UC_RISCV_REG_A0)
            self._fast_forward(requested_us * 16)
            uc.reg_write(UC_RISCV_REG_PC, uc.reg_read(UC_RISCV_REG_RA))
        elif address == self.symbols.get("ulp_lp_core_delay_cycles"):
            self._fast_forward(uc.reg_read(UC_RISCV_REG_A0))
            uc.reg_write(UC_RISCV_REG_PC, uc.reg_read(UC_RISCV_REG_RA))
        elif address == self.symbols.get("hcp2_lp_now_us"):
            uc.reg_write(UC_RISCV_REG_A0, int(self.time_us) & 0xFFFFFFFF)
            uc.reg_write(UC_RISCV_REG_PC, uc.reg_read(UC_RISCV_REG_RA))

    def _fast_forward(self, cycles: int) -> None:
        if cycles <= 0:
            return
        self.cycles += cycles
        self.fast_forward_cycles += cycles
        self._service_serial()
        self._write_cycle_regs()

    def _write_cycle_regs(self) -> None:
        self.uc.reg_write(UC_RISCV_REG_CYCLE, self.cycles & 0xFFFFFFFF)
        self.uc.reg_write(UC_RISCV_REG_CYCLEH, (self.cycles >> 32) & 0xFFFFFFFF)
        self.uc.reg_write(UC_RISCV_REG_MCYCLE, self.cycles & 0xFFFFFFFF)
        self.uc.reg_write(UC_RISCV_REG_MCYCLEH, (self.cycles >> 32) & 0xFFFFFFFF)

    def _service_serial(self) -> None:
        self._move_wire_to_rx()
        self._drain_tx()
        self._sync_uart_regs()

    def _serial_quiescent(self) -> bool:
        self._service_serial()
        return (
            not self.rx_wire
            and not self.rx_fifo
            and not self.tx_fifo
            and self.tx_next_due_cycle is None
            and not self.de_enabled
        )

    def _move_wire_to_rx(self) -> None:
        while self.rx_wire and self.rx_wire[0][0] <= self.cycles:
            _, byte = self.rx_wire.popleft()
            if len(self.rx_fifo) >= 16:
                self.rx_overflows += 1
                continue
            self.rx_fifo.append(byte)
            self.rx_high_water = max(self.rx_high_water, len(self.rx_fifo))

    def _drain_tx(self) -> None:
        if self.tx_fifo_wedged:
            return
        while self.tx_fifo:
            if self.tx_next_due_cycle is None:
                self.tx_next_due_cycle = self.cycles + UART_BYTE_CYCLES
            if self.tx_next_due_cycle > self.cycles:
                break
            byte = self.tx_fifo.popleft()
            if not self.de_enabled:
                self.tx_when_de_low += 1
            if self.tx_first_cycle is None:
                self.tx_first_cycle = self.tx_next_due_cycle
                if self.last_master_write_cycle is not None:
                    latency_us = (self.tx_first_cycle - self.last_master_write_cycle) * 1_000_000.0 / LP_CLOCK_HZ
                    self.reply_latencies_us.append(latency_us)
                    self._trace("reply_latency", latency_us=round(latency_us))
            self.tx_output.append(byte)
            if self.echo_tx_to_rx and self.de_enabled and not self.re_disabled:
                echo = byte
                self.echo_count += 1
                if self.echo_mismatch_at is not None and self.echo_count == self.echo_mismatch_at:
                    echo ^= 0xFF
                if len(self.rx_fifo) >= 16:
                    self.rx_overflows += 1
                else:
                    self.rx_fifo.append(echo)
                    self.rx_high_water = max(self.rx_high_water, len(self.rx_fifo))
            self.tx_next_due_cycle = self.tx_next_due_cycle + UART_BYTE_CYCLES if self.tx_fifo else None

    def _sync_uart_regs(self) -> None:
        status = (
            (len(self.rx_fifo) & 0x1F) << 3
            | (1 << 14)
            | (1 << 15)
            | (len(self.tx_fifo) & 0x1F) << 19
            | (1 << 29)
            | (1 << 30)
            | (1 << 31)
        )
        _write_u32(self.uc, LP_UART_STATUS, status)
        fsm = 0 if not self.tx_fifo else (1 << 4)
        _write_u32(self.uc, LP_UART_FSM_STATUS, fsm)

        intr = 0
        if len(self.rx_fifo) >= 10:
            intr |= UART_INTR_RXFIFO_FULL
        if self.rx_fifo:
            intr |= UART_INTR_RXFIFO_TOUT
        if self.rx_overflows:
            intr |= UART_INTR_RXFIFO_OVF
        if len(self.tx_fifo) < 10:
            intr |= UART_INTR_TXFIFO_EMPTY
        if not self.tx_fifo:
            intr |= UART_INTR_TX_DONE
        _write_u32(self.uc, LP_UART_INT_RAW, intr)
        int_ena = _read_u32_mem(self.uc, LP_UART_INT_ENA)
        _write_u32(self.uc, LP_UART_INT_ST, intr & int_ena)
        _write_u32(self.uc, LP_UART_REG_UPDATE, 0)

    def _hook_mem_read(self, uc: Uc, access: int, address: int, size: int, value: int, user_data: object | None) -> None:
        if address >= 0x60000000:
            self.mmio_manifest[f"R 0x{address:08x}/{size}"] += 1
        self._service_serial()
        if address == LP_UART_FIFO:
            byte = self.rx_fifo.popleft() if self.rx_fifo else 0
            _write_u32(uc, LP_UART_FIFO, byte)
            self._sync_uart_regs()
        elif address in {LP_UART_STATUS, LP_UART_INT_RAW, LP_UART_INT_ST, LP_UART_FSM_STATUS, LP_UART_REG_UPDATE}:
            self._sync_uart_regs()
        elif address == LPPERI_CLK_EN:
            _write_u32(uc, LPPERI_CLK_EN, _read_u32_mem(uc, LPPERI_CLK_EN) | LPPERI_LP_UART_CLK_BIT)
        elif address == LPPERI_RESET_EN:
            _write_u32(uc, LPPERI_RESET_EN, _read_u32_mem(uc, LPPERI_RESET_EN) & ~LPPERI_LP_UART_CLK_BIT)

    def _hook_mem_write(self, uc: Uc, access: int, address: int, size: int, value: int, user_data: object | None) -> None:
        if address >= 0x60000000:
            self.mmio_manifest[f"W 0x{address:08x}/{size}"] += 1
        self._service_serial()
        if address == LP_UART_FIFO:
            byte = value & 0xFF
            if len(self.tx_fifo) >= 16:
                self.tx_overflows += 1
            else:
                self.tx_fifo.append(byte)
                self.tx_high_water = max(self.tx_high_water, len(self.tx_fifo))
                self.max_tx_write_burst = max(self.max_tx_write_burst, len(self.tx_fifo))
                if self.tx_next_due_cycle is None:
                    self.tx_next_due_cycle = self.cycles + UART_BYTE_CYCLES
        elif address == LP_UART_INT_CLR:
            if value & UART_INTR_RXFIFO_OVF:
                self.rx_overflows = 0
        elif address == LP_UART_REG_UPDATE:
            _write_u32(uc, LP_UART_REG_UPDATE, 0)
        elif address == LP_IO_OUT_DATA_W1TS:
            self.gpio_writes.append({"cycle": self.cycles, "op": "set", "value": value})
            if value & LP_IO_DE_MASK:
                self._set_de(True)
            if value & LP_IO_RE_MASK:
                self._set_re_disabled(True)
            _write_u32(uc, LP_IO_OUT_DATA, _read_u32_mem(uc, LP_IO_OUT_DATA) | value)
        elif address == LP_IO_OUT_DATA_W1TC:
            self.gpio_writes.append({"cycle": self.cycles, "op": "clear", "value": value})
            if value & LP_IO_DE_MASK:
                self._set_de(False)
            if value & LP_IO_RE_MASK:
                self._set_re_disabled(False)
            _write_u32(uc, LP_IO_OUT_DATA, _read_u32_mem(uc, LP_IO_OUT_DATA) & ~value)
        elif address == LP_IO_OUT_ENABLE_W1TS:
            pass
        self._sync_uart_regs()

    def _set_de(self, enabled: bool) -> None:
        if self.de_enabled == enabled:
            return
        self.de_enabled = enabled
        self.de_events.append({"cycle": self.cycles, "time_us": round(self.time_us), "enabled": enabled})
        self._trace("de", enabled=enabled)

    def _set_re_disabled(self, disabled: bool) -> None:
        if self.re_disabled == disabled:
            return
        self.re_disabled = disabled
        self.re_events.append({"cycle": self.cycles, "time_us": round(self.time_us), "disabled": disabled})
        self._trace("re", disabled=disabled)

    def _hook_invalid_mem(self, uc: Uc, access: int, address: int, size: int, value: int, user_data: object | None) -> bool:
        page = address & ~0xFFF
        item = f"0x{address:08x}/{size}"
        self.unknown_mmio.append(item)
        if page in MODELED_MMIO_PAGES:
            uc.mem_map(page, 0x1000, UC_PROT_ALL)
            return True
        return False

    def run(self, instruction_budget: int) -> None:
        if instruction_budget <= 0:
            return
        pc = self.uc.reg_read(UC_RISCV_REG_PC)
        try:
            self.uc.emu_start(pc, 0, count=instruction_budget)
        except UcError as exc:
            raise LPEmuError(f"Unicorn stopped at pc=0x{self.uc.reg_read(UC_RISCV_REG_PC):08x}: {exc}") from exc
        self._service_serial()

    def run_until(self, condition: Callable[[], bool], instruction_budget: int, slice_instructions: int = 4096) -> bool:
        remaining = instruction_budget
        while remaining > 0:
            if condition():
                return True
            step = min(slice_instructions, remaining)
            self.run(step)
            remaining -= step
        return condition()

    def advance_time_us(self, gap_us: int) -> None:
        if gap_us <= 0:
            return
        self.boot()
        target_cycle = self.cycles + int(gap_us * LP_CLOCK_HZ / 1_000_000)
        if not self._serial_quiescent():
            if not self.run_until(self._serial_quiescent, instruction_budget=4_000_000, slice_instructions=4096):
                raise LPEmuError("LP UART work did not quiesce before idle fast-forward")
            self.run(4096)
        if self.cycles < target_cycle:
            self.fast_forward_cycles += target_cycle - self.cycles
            self.cycles = target_cycle
            self._write_cycle_regs()
            self._service_serial()

    def boot(self) -> None:
        if self.running:
            return
        ok = self.run_until(lambda: self.heartbeat() > 0, instruction_budget=2_000_000)
        if not ok:
            raise LPEmuError("LP firmware did not publish a heartbeat during boot")
        self.running = True

    def write_uart(self, data: bytes) -> None:
        self.boot()
        self._trace("master_frame", raw=data.hex())
        start = max(self.cycles, self.rx_last_due_cycle)
        self.tx_first_cycle = None
        for byte in data:
            start += UART_BYTE_CYCLES
            self.rx_wire.append((start, byte))
        self.rx_last_due_cycle = start
        self.last_master_write_cycle = self.rx_last_due_cycle

    def read_uart_available(self, timeout_s: float) -> bytes:
        self.boot()
        deadline = self.cycles + max(1, int(timeout_s * LP_CLOCK_HZ))
        while self.cycles < deadline:
            if self.tx_output and not self.tx_fifo and not self.de_enabled:
                break
            if self.tx_output and len(self.tx_output) >= 32:
                break
            self.run(8192)
            if self.tx_output and not self.tx_fifo and not self.de_enabled:
                break
        return self._take_tx_output()

    def _take_tx_output(self) -> bytes:
        data = bytes(self.tx_output)
        self.tx_output.clear()
        if data:
            self._trace("slave_frame", raw=data.hex())
        return data

    def heartbeat(self) -> int:
        return _read_u32(self.uc, MAILBOX_ADDR + 12)

    def command(self, name: str) -> str:
        self.boot()
        command_id = COMMAND_IDS.get(name)
        if command_id is None:
            return f"ERR unknown command {name}"

        self.epoch += 1
        self.command_sequence = 0
        _write_u32(self.uc, MAILBOX_ADDR + 28, self.epoch)
        _write_u32(self.uc, MAILBOX_ADDR + 32, 0)
        _write_u32(self.uc, MAILBOX_ADDR + 36, 0)
        _write_u8(self.uc, MAILBOX_ADDR + 40, 0)
        _write_u8(self.uc, MAILBOX_ADDR + 42, 0)
        _write_u32(self.uc, MAILBOX_ADDR + 44, 0)
        _write_u8(self.uc, MAILBOX_ADDR + 97, 0)
        self.run(50_000)

        self.command_sequence += 1
        now_us = _read_u32(self.uc, MAILBOX_ADDR + 48)
        _write_u8(self.uc, MAILBOX_ADDR + 40, command_id)
        _write_u8(self.uc, MAILBOX_ADDR + 41, 0)
        _write_u8(self.uc, MAILBOX_ADDR + 42, 0)
        _write_u32(self.uc, MAILBOX_ADDR + 44, now_us + 250_000)
        _write_u32(self.uc, MAILBOX_ADDR + 32, self.command_sequence)

        ok = self.run_until(
            lambda: _read_u32(self.uc, MAILBOX_ADDR + 36) == self.command_sequence,
            instruction_budget=1_000_000,
        )
        result = int(self.uc.mem_read(MAILBOX_ADDR + 42, 1)[0]) if ok else 0
        return f"OK ack={self.command_sequence} result={result}" if ok else "ERR command ack timeout"

    def hp_reboot(self) -> None:
        self.epoch += 1
        self.command_sequence = 0
        _write_u32(self.uc, MAILBOX_ADDR + 28, self.epoch)
        _write_u32(self.uc, MAILBOX_ADDR + 32, 0)
        _write_u32(self.uc, MAILBOX_ADDR + 36, 0)
        _write_u8(self.uc, MAILBOX_ADDR + 40, 0)
        _write_u8(self.uc, MAILBOX_ADDR + 42, 0)
        _write_u32(self.uc, MAILBOX_ADDR + 44, 0)
        _write_u8(self.uc, MAILBOX_ADDR + 97, 0)
        self.run(50_000)

    def lp_reset(self) -> None:
        self.boot()
        before = _read_u32(self.uc, MAILBOX_ADDR + 72)
        self._trace("lp_reset")
        self.tx_fifo.clear()
        self.tx_next_due_cycle = None
        self.tx_first_cycle = None
        self._set_de(False)
        self.uc.reg_write(UC_RISCV_REG_PC, self.entry)
        self.uc.reg_write(UC_RISCV_REG_RA, 0)
        self.running = False
        ok = self.run_until(lambda: _read_u32(self.uc, MAILBOX_ADDR + 72) > before, instruction_budget=2_000_000)
        if not ok:
            raise LPEmuError("LP firmware did not restart after LP reset")
        self.running = True

    def reload_decision(self, heartbeat_before: int, heartbeat_after: int) -> str:
        magic = _read_u32(self.uc, MAILBOX_ADDR + 0)
        abi = struct.unpack("<H", self.uc.mem_read(MAILBOX_ADDR + 4, 2))[0]
        size = struct.unpack("<H", self.uc.mem_read(MAILBOX_ADDR + 6, 2))[0]
        version = _read_u32(self.uc, MAILBOX_ADDR + 8)
        if magic != MAILBOX_MAGIC or abi != MAILBOX_ABI_VERSION or size != MAILBOX_SIZE:
            return "reload"
        if version != MAILBOX_FIRMWARE_VERSION:
            return "reload"
        return "skip" if heartbeat_after != heartbeat_before else "reload"

    def tx_abort_count(self) -> int:
        return _read_u32(self.uc, MAILBOX_ADDR + 60)

    def collision_count(self) -> int:
        return _read_u32(self.uc, MAILBOX_ADDR + 64)

    def max_de_hold_us(self) -> int:
        return _read_u32(self.uc, MAILBOX_ADDR + 68)

    def report(self) -> dict[str, object]:
        latencies = sorted(self.reply_latencies_us)
        latency_p99 = None
        if latencies:
            index = min(len(latencies) - 1, int(round(0.99 * (len(latencies) - 1))))
            latency_p99 = latencies[index]
        return {
            "elf": str(self.elf_path),
            "entry": self.entry,
            "cycles": self.cycles,
            "instructions": self.instructions,
            "instruction_count_mode": "basic-block size estimate plus exact fast-forwarded LP delay cycles",
            "blocks": self.blocks,
            "fast_forward_cycles": self.fast_forward_cycles,
            "simulated_time_us": self.time_us,
            "heartbeat": self.heartbeat(),
            "rx_fifo_high_water": self.rx_high_water,
            "tx_fifo_high_water": self.tx_high_water,
            "max_tx_write_burst": self.max_tx_write_burst,
            "rx_overflows": self.rx_overflows,
            "tx_overflows": self.tx_overflows,
            "tx_when_de_low": self.tx_when_de_low,
            "mailbox_polls_seen": _read_u32(self.uc, MAILBOX_ADDR + 52),
            "mailbox_polls_answered": _read_u32(self.uc, MAILBOX_ADDR + 56),
            "mailbox_tx_abort_count": self.tx_abort_count(),
            "mailbox_collision_count": self.collision_count(),
            "mailbox_max_de_hold_us": self.max_de_hold_us(),
            "mailbox_last_poll_us": _read_u32(self.uc, MAILBOX_ADDR + 76),
            "mailbox_crc_error_count": _read_u32(self.uc, MAILBOX_ADDR + 80),
            "mailbox_rx_error_count": _read_u32(self.uc, MAILBOX_ADDR + 84),
            "mailbox_stop_trigger_fire_count": struct.unpack("<H", self.uc.mem_read(MAILBOX_ADDR + 98, 2))[0],
            "mailbox_health_flags": struct.unpack("<H", self.uc.mem_read(MAILBOX_ADDR + 100, 2))[0],
            "mailbox_max_rx_fifo_count": struct.unpack("<H", self.uc.mem_read(MAILBOX_ADDR + 102, 2))[0],
            "mailbox_max_loop_us": _read_u32(self.uc, MAILBOX_ADDR + 104),
            "mailbox_loop_overrun_count": _read_u32(self.uc, MAILBOX_ADDR + 108),
            "mailbox_rx_starvation_count": _read_u32(self.uc, MAILBOX_ADDR + 112),
            "mailbox_stuck_de_count": _read_u32(self.uc, MAILBOX_ADDR + 116),
            "mailbox_repair_count": _read_u32(self.uc, MAILBOX_ADDR + 120),
            "mailbox_max_poll_rx_to_schedule_us": _read_u32(self.uc, MAILBOX_ADDR + 124),
            "mailbox_max_response_schedule_to_tx_start_us": _read_u32(self.uc, MAILBOX_ADDR + 128),
            "mailbox_max_response_tx_us": _read_u32(self.uc, MAILBOX_ADDR + 132),
            "mailbox_protocol_sequence": _read_u32(self.uc, MAILBOX_ADDR + 136),
            "mailbox_protocol_at_us": _read_u32(self.uc, MAILBOX_ADDR + 140),
            "mailbox_protocol_event_type": self.uc.mem_read(MAILBOX_ADDR + 144, 1)[0],
            "mailbox_protocol_frame_type": self.uc.mem_read(MAILBOX_ADDR + 145, 1)[0],
            "mailbox_protocol_len": self.uc.mem_read(MAILBOX_ADDR + 146, 1)[0],
            "mailbox_protocol_hex": self.uc.mem_read(
                MAILBOX_ADDR + 148, min(self.uc.mem_read(MAILBOX_ADDR + 146, 1)[0], 32)
            ).hex().upper(),
            "mailbox_protocol_head": _read_u32(self.uc, MAILBOX_ADDR + 180),
            "mailbox_protocol_tail": _read_u32(self.uc, MAILBOX_ADDR + 184),
            "de_events": self.de_events[:20],
            "re_events": self.re_events[:20],
            "gpio_writes": self.gpio_writes[:40],
            "trace_event_count": len(self.trace_events),
            "trace": self.trace_events,
            "reply_latency_min_us": latencies[0] if latencies else None,
            "reply_latency_p99_us": latency_p99,
            "reply_latency_max_us": latencies[-1] if latencies else None,
            "mmio_manifest": dict(sorted(self.mmio_manifest.items())),
            "unmodeled_mmio": self.unknown_mmio,
            "layout": self.layout.as_dict(),
            "reload_decision_live": self.reload_decision(max(0, self.heartbeat() - 1), self.heartbeat()),
        }


class LPEmuTransport:
    def __init__(self, blob: Path) -> None:
        self.emulator = LPEmulator.from_blob(blob)
        self.emulator.boot()

    def write(self, data: bytes) -> None:
        self.emulator.write_uart(data)

    def read_available(self, timeout: float) -> bytes:
        return self.emulator.read_uart_available(timeout)

    def advance_time_us(self, gap_us: int) -> None:
        self.emulator.advance_time_us(gap_us)

    def command(self, line: str) -> str:
        parts = line.strip().split()
        if len(parts) == 2 and parts[0] == "press":
            return self.emulator.command(parts[1])
        if line.strip() == "hp reboot":
            self.emulator.hp_reboot()
            return "OK reboot"
        return f"ERR unsupported command {line!r}"

    def close(self) -> None:
        pass


def write_report(path: Path, payload: dict[str, object]) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
