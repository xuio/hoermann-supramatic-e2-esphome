"""Validate the bench-only ESP32 realtime HCP2 firmware build."""

from __future__ import annotations

import argparse
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = ROOT / "configs" / ".esphome" / "build" / "supramatic-4-esp32-realtime"
DEFAULT_ENV_NAME = "supramatic-4-esp32-realtime"
ESP32C6_BUILD_DIR = ROOT / "configs" / ".esphome" / "build" / "supramatic-4-c6-hp-realtime"
ESP32C6_ENV_NAME = "supramatic-4-c6-hp-realtime"
ELF_MACHINE_XTENSA = 94
ELF_MACHINE_RISCV = 243

IRAM_SYMBOLS = (
    "HCP2Bridge::esp32_realtime_uart_isr_",
    "HCP2Bridge::esp32_realtime_timer_alarm_",
    "HCP2Bridge::esp32_realtime_now_us_cb_",
    "HCP2Bridge::esp32_realtime_set_de_from_isr_",
    "HCP2Bridge::esp32_realtime_schedule_timer_from_isr_",
    "hcp2_crc16_modbus",
    "hcp2_crc16_check",
    "hcp2_crc16_append",
    "hcp2_frame_master_expected_len",
    "hcp2_frame_parse_master",
    "hcp2_frame_build_scan_response",
    "hcp2_frame_build_command_response",
    "hcp2_frame_build_status_response",
    "hcp2_engine_rx_byte",
    "hcp2_engine_pending_tx_ready",
    "hcp2_engine_pending_tx_due_us",
    "hcp2_engine_claim_due_tx",
    "hcp2_engine_mark_tx_started",
    "hcp2_engine_mark_tx_done",
)

C6_UHCI_IRAM_SYMBOLS = (
    "HCP2Bridge::esp32c6_realtime_uhci_rx_cb_",
    "HCP2Bridge::esp32c6_realtime_uhci_tx_cb_",
)

FORBIDDEN_SYMBOLS = (
    "hcp2_lp_blob_data",
    "hcp2_lp_blob_data_len",
)


@dataclass(frozen=True)
class Symbol:
    name: str
    address: int
    size: int


def _platformio_tool(name: str) -> Path | None:
    packages = Path.home() / ".platformio" / "packages"
    if not packages.exists():
        return None
    matches = sorted(packages.glob(f"toolchain-*/bin/{name}"))
    return matches[0] if matches else None


def _find_tool(name: str) -> str | None:
    path = shutil.which(name)
    if path:
        return path
    platformio_path = _platformio_tool(name)
    if platformio_path:
        return str(platformio_path)
    return None


def _required_tool(name: str) -> str:
    path = _find_tool(name)
    if path is None:
        raise SystemExit(f"Could not find {name}; build the firmware once so PlatformIO installs the toolchain")
    return path


def _tool_names(arch: str) -> tuple[str, str]:
    if arch == "xtensa":
        return ("xtensa-esp32-elf-nm", "xtensa-esp32-elf-size")
    if arch == "riscv":
        return ("riscv32-esp-elf-nm", "riscv32-esp-elf-size")
    raise ValueError(f"unsupported arch: {arch}")


def _elf_arch(elf: Path) -> str | None:
    header = elf.read_bytes()[:20]
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return None
    if header[5] == 1:
        machine = int.from_bytes(header[18:20], "little")
    elif header[5] == 2:
        machine = int.from_bytes(header[18:20], "big")
    else:
        return None
    if machine == ELF_MACHINE_XTENSA:
        return "xtensa"
    if machine == ELF_MACHINE_RISCV:
        return "riscv"
    return None


def _select_tools(arch: str, elf: Path) -> tuple[str, str, str]:
    if arch == "auto":
        detected = _elf_arch(elf)
        if detected is not None:
            arch = detected
    arches = ("xtensa", "riscv") if arch == "auto" else (arch,)
    errors: list[str] = []
    for candidate in arches:
        nm_name, size_name = _tool_names(candidate)
        nm = _find_tool(nm_name)
        size_tool = _find_tool(size_name)
        if nm is None or size_tool is None:
            errors.append(f"{candidate}: missing {nm_name if nm is None else size_name}")
            continue
        result = subprocess.run([nm, "-S", "--defined-only", "-C", str(elf)], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
        if result.returncode == 0:
            return candidate, nm, size_tool
        errors.append(f"{candidate}: {result.stderr.strip()}")
    if arch == "auto":
        detail = "\n".join(errors)
        raise SystemExit(f"Could not auto-detect ELF toolchain for {elf}:\n{detail}")
    raise SystemExit(errors[-1] if errors else f"Could not find toolchain for {arch}")


def _firmware_elf(build_dir: Path, env_name: str) -> Path:
    direct = build_dir / ".pioenvs" / env_name / "firmware.elf"
    if direct.exists():
        return direct
    matches = sorted(build_dir.glob(".pioenvs/*/firmware.elf"))
    if len(matches) == 1:
        return matches[0]
    raise SystemExit(f"Could not find firmware.elf under {build_dir}")


def _firmware_bin(elf: Path) -> Path:
    binary = elf.with_suffix(".bin")
    if not binary.exists():
        raise SystemExit(f"Could not find {binary}; run esphome compile first")
    return binary


def _run(command: list[str]) -> str:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{' '.join(command)} failed:\n{result.stderr}")
    return result.stdout


def _read_symbols(nm: str, elf: Path) -> list[Symbol]:
    symbols: list[Symbol] = []
    for line in _run([nm, "-S", "--defined-only", "-C", str(elf)]).splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) < 4:
            continue
        try:
            address = int(parts[0], 16)
            size = int(parts[1], 16)
        except ValueError:
            continue
        symbols.append(Symbol(name=parts[3], address=address, size=size))
    return symbols


def _read_sections(size_tool: str, elf: Path) -> dict[str, tuple[int, int]]:
    sections: dict[str, tuple[int, int]] = {}
    for line in _run([size_tool, "-A", str(elf)]).splitlines():
        parts = line.split()
        if len(parts) != 3 or not parts[0].startswith("."):
            continue
        try:
            sections[parts[0]] = (int(parts[1]), int(parts[2], 0))
        except ValueError:
            continue
    return sections


def _address_ranges(sections: dict[str, tuple[int, int]], prefixes: tuple[str, ...]) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    for name, (size, address) in sections.items():
        if size <= 0:
            continue
        if any(name.startswith(prefix) for prefix in prefixes):
            ranges.append((address, address + size))
    return ranges


def _in_ranges(address: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start <= address < end for start, end in ranges)


def _sum_sections(sections: dict[str, tuple[int, int]], names: tuple[str, ...]) -> int:
    return sum(sections.get(name, (0, 0))[0] for name in names)


def _find_required_symbol(symbols: list[Symbol], required: str) -> Symbol | None:
    matches = [symbol for symbol in symbols if required in symbol.name]
    if len(matches) == 1:
        return matches[0]
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check the ESP32 realtime HCP2 build for hot-path placement and conservative size budgets.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR, help="ESPHome build directory")
    parser.add_argument("--env-name", default=DEFAULT_ENV_NAME, help="PlatformIO environment name")
    parser.add_argument(
        "--profile",
        choices=("esp32", "esp32c6"),
        default="esp32",
        help="preselect build directory, environment, architecture, and conservative budgets",
    )
    parser.add_argument("--max-iram", type=int, default=90000, help="max bytes in IRAM sections")
    parser.add_argument("--max-dram", type=int, default=120000, help="max bytes in DRAM data/bss/noinit sections")
    parser.add_argument("--max-image", type=int, default=700000, help="max bytes in firmware.bin")
    parser.add_argument("--arch", choices=("auto", "xtensa", "riscv"), default="auto", help="ELF toolchain family")
    args = parser.parse_args(argv)

    if args.profile == "esp32c6":
        if args.build_dir == DEFAULT_BUILD_DIR:
            args.build_dir = ESP32C6_BUILD_DIR
        if args.env_name == DEFAULT_ENV_NAME:
            args.env_name = ESP32C6_ENV_NAME
        if args.arch == "auto":
            args.arch = "riscv"
        args.max_iram = max(args.max_iram, 130000)
        args.max_image = max(args.max_image, 1000000)

    elf = _firmware_elf(args.build_dir, args.env_name)
    arch, nm, size_tool = _select_tools(args.arch, elf)
    firmware_bin = _firmware_bin(elf)
    symbols = _read_symbols(nm, elf)
    sections = _read_sections(size_tool, elf)

    iram_ranges = _address_ranges(sections, (".iram0.", ".iram1."))
    if not iram_ranges:
        raise SystemExit("No IRAM sections found in firmware.elf")

    problems: list[str] = []
    required_iram_symbols = list(IRAM_SYMBOLS)
    if arch == "riscv" or args.profile == "esp32c6":
        required_iram_symbols.extend(C6_UHCI_IRAM_SYMBOLS)

    for required in required_iram_symbols:
        symbol = _find_required_symbol(symbols, required)
        if symbol is None:
            problems.append(f"missing or ambiguous required symbol: {required}")
            continue
        if not _in_ranges(symbol.address, iram_ranges):
            problems.append(f"{symbol.name} is not in IRAM: 0x{symbol.address:08x}")

    for forbidden in FORBIDDEN_SYMBOLS:
        matches = [symbol.name for symbol in symbols if forbidden in symbol.name]
        if matches:
            problems.append(f"forbidden LP blob symbol present in ESP32 realtime build: {', '.join(matches)}")

    iram = _sum_sections(sections, (".iram0.vectors", ".iram0.text", ".iram0.text_end", ".iram0.data", ".iram0.bss"))
    dram = _sum_sections(sections, (".dram0.data", ".dram0.bss", ".noinit"))
    image = firmware_bin.stat().st_size

    budgets = (
        ("IRAM", iram, args.max_iram),
        ("DRAM", dram, args.max_dram),
        ("image", image, args.max_image),
    )
    for name, used, limit in budgets:
        if used > limit:
            problems.append(f"{name} budget exceeded: {used} > {limit} bytes")

    if problems:
        for problem in problems:
            print(f"FAIL: {problem}")
        return 1

    print(f"ESP32 realtime build OK ({arch}): {elf}")
    print(f"IRAM={iram} DRAM={dram} image={image}")
    print(f"checked {len(required_iram_symbols)} IRAM hot-path symbols; LP blob symbols absent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
