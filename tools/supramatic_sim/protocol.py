"""HCP2 frame helpers shared by the simulator and tests."""

from __future__ import annotations

from dataclasses import dataclass


SLAVE_ID = 0x02
FC_WRITE_MULTIPLE_REGISTERS = 0x10
FC_READ_WRITE_MULTIPLE_REGISTERS = 0x17
REG_STATUS_READ = 0x9CB9
REG_COMMAND_WRITE = 0x9C41
REG_BROADCAST_STATUS = 0x9D31

SCAN_RESPONSE = bytes.fromhex("02170A00000205043010FFA8550F13")
COMMAND_RESPONSE_BY_COUNTER = {
    0x02: bytes.fromhex("021704020004FD08DE"),
    0x1C: bytes.fromhex("0217041C0004FD0EF6"),
}
BUTTON_STATUS_PHASES = {
    "open": {"press": (0x02, 0x10, 0x00), "release": (0x01, 0x10, 0x00)},
    "close": {"press": (0x02, 0x20, 0x00), "release": (0x01, 0x20, 0x00)},
    "stop": {"press": (0x02, 0x40, 0x00), "release": (0x01, 0x40, 0x00)},
    "vent": {"press": (0x02, 0x00, 0x40), "release": (0x01, 0x00, 0x40)},
    "half": {"press": (0x02, 0x00, 0x04), "release": (0x01, 0x00, 0x04)},
    "light": {"press": (0x10, 0x00, 0x02), "release": (0x08, 0x00, 0x02)},
}
BUTTON_STATUS_ENCODINGS = {
    name: set(phases.values())
    for name, phases in BUTTON_STATUS_PHASES.items()
}


@dataclass(frozen=True)
class BroadcastState:
    target: int
    current: int
    state: int
    light: int = 0x14


BROADCAST_CLOSED = BroadcastState(target=0, current=0, state=0x40)
BROADCAST_OPEN = BroadcastState(target=200, current=200, state=0x20)


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
            crc &= 0xFFFF
    return crc


def append_crc(payload: bytes) -> bytes:
    crc = crc16_modbus(payload)
    return payload + bytes((crc & 0xFF, (crc >> 8) & 0xFF))


def crc_ok(frame: bytes) -> bool:
    return len(frame) >= 3 and crc16_modbus(frame) == 0


def bus_scan_request(slave_id: int = SLAVE_ID) -> bytes:
    payload = bytes(
        [
            slave_id,
            FC_READ_WRITE_MULTIPLE_REGISTERS,
            0x9C,
            0xB9,
            0x00,
            0x05,
            0x9C,
            0x41,
            0x00,
            0x03,
            0x06,
            0x00,
            0x02,
            0x00,
            0x00,
            0x01,
            slave_id,
        ]
    )
    return append_crc(payload)


def status_poll(counter: int, slave_id: int = SLAVE_ID) -> bytes:
    payload = bytes(
        [
            slave_id,
            FC_READ_WRITE_MULTIPLE_REGISTERS,
            0x9C,
            0xB9,
            0x00,
            0x08,
            0x9C,
            0x41,
            0x00,
            0x02,
            0x04,
            counter & 0xFF,
            0x03,
            0x00,
            0x00,
        ]
    )
    return append_crc(payload)


def light_command(counter: int, enabled: bool, slave_id: int = SLAVE_ID) -> bytes:
    payload = bytes(
        [
            slave_id,
            FC_READ_WRITE_MULTIPLE_REGISTERS,
            0x9C,
            0xB9,
            0x00,
            0x02,
            0x9C,
            0x41,
            0x00,
            0x02,
            0x04,
            counter & 0xFF,
            0x04,
            0x17,
            0x01 if not enabled else 0x00,
        ]
    )
    return append_crc(payload)


def broadcast_status(state: BroadcastState) -> bytes:
    data = bytearray(18)
    data[0] = 0x16
    data[1] = 0x00
    data[2] = state.target & 0xFF
    data[3] = state.current & 0xFF
    data[4] = state.state & 0xFF
    data[5] = 0x60
    data[13] = state.light & 0xFF
    data[15] = 0x01
    payload = bytes([0x00, FC_WRITE_MULTIPLE_REGISTERS, 0x9D, 0x31, 0x00, 0x09, 0x12]) + bytes(data)
    return append_crc(payload)


def response_expected_len(buffer: bytes) -> int | None:
    if len(buffer) < 3:
        return None
    if buffer[1] != FC_READ_WRITE_MULTIPLE_REGISTERS:
        return -1
    byte_count = buffer[2]
    expected = byte_count + 5
    if expected < 5 or expected > 32:
        return -1
    return expected


def decode_response_kind(frame: bytes) -> str:
    if len(frame) < 3 or frame[1] != FC_READ_WRITE_MULTIPLE_REGISTERS:
        return "unknown"
    byte_count = frame[2]
    if byte_count == 10:
        return "scan"
    if byte_count == 16:
        return "status"
    if byte_count == 4:
        return "command"
    return "unknown"


def response_counter(frame: bytes) -> int | None:
    if decode_response_kind(frame) not in {"status", "command"} or len(frame) < 4:
        return None
    return frame[3]


def decode_status_button(frame: bytes) -> str | None:
    decoded = decode_status_button_phase(frame)
    return decoded[0] if decoded is not None else None


def decode_status_button_phase(frame: bytes) -> tuple[str, str] | None:
    if decode_response_kind(frame) != "status" or len(frame) < 10:
        return None
    encoding = (frame[7], frame[8], frame[9])
    for name, phases in BUTTON_STATUS_PHASES.items():
        for phase, phase_encoding in phases.items():
            if encoding == phase_encoding:
                return name, phase
    return None
