from __future__ import annotations

import time

from tools.supramatic_sim import protocol
from tools.supramatic_sim.transport import SocketPairHostTransport


REFERENCE_SIGNATURE = bytes.fromhex("00000205043010FFA845")
BUTTON_PHASES = {
    "open": ((0x02, 0x10, 0x00), (0x01, 0x10, 0x00)),
    "close": ((0x02, 0x20, 0x00), (0x01, 0x20, 0x00)),
    "stop": ((0x02, 0x40, 0x00), (0x01, 0x40, 0x00)),
    "half": ((0x02, 0x00, 0x04), (0x01, 0x00, 0x04)),
    "vent": ((0x02, 0x00, 0x40), (0x01, 0x00, 0x40)),
    "light": ((0x10, 0x00, 0x02), (0x08, 0x00, 0x02)),
}


def read_be16(data: bytes, offset: int) -> int:
    return (data[offset] << 8) | data[offset + 1]


def master_expected_len(buffer: bytes) -> int | None:
    if len(buffer) < 2:
        return None
    if buffer[1] == protocol.FC_WRITE_MULTIPLE_REGISTERS:
        if len(buffer) < 7:
            return None
        return 9 + buffer[6]
    if buffer[1] == protocol.FC_READ_WRITE_MULTIPLE_REGISTERS:
        if len(buffer) < 11:
            return None
        return 13 + buffer[10]
    return -1


def scan_response(slave_id: int, signature: bytes) -> bytes:
    return protocol.append_crc(bytes([slave_id, protocol.FC_READ_WRITE_MULTIPLE_REGISTERS, len(signature)]) + signature)


def status_response(slave_id: int, counter: int, command: int, phase: tuple[int, int, int] | None) -> bytes:
    payload = bytearray([slave_id, protocol.FC_READ_WRITE_MULTIPLE_REGISTERS, 0x10])
    payload.extend([counter & 0xFF, 0x00, command & 0xFF, 0x01])
    if phase is None:
        payload.extend([0x00, 0x00, 0x00])
    else:
        payload.extend(phase)
    payload.extend([0x00] * 9)
    assert len(payload) == 19
    return protocol.append_crc(bytes(payload))


def command_response(slave_id: int, counter: int, command: int) -> bytes:
    return protocol.append_crc(
        bytes([slave_id, protocol.FC_READ_WRITE_MULTIPLE_REGISTERS, 0x04, counter & 0xFF, 0x00, command & 0xFF, 0xFD])
    )


class ReferenceHCP2Responder:
    """Independent behavior oracle distilled from public HCP2 behavior, not upstream code."""

    def __init__(self, *, slave_id: int = protocol.SLAVE_ID, signature: bytes = REFERENCE_SIGNATURE) -> None:
        self.slave_id = slave_id
        self.signature = bytearray(signature)
        self.rx = bytearray()
        self.pending = bytearray()
        self.button: str | None = None
        self.command_started_s: float | None = None
        self.release_pending = False

    def command(self, line: str) -> str:
        parts = line.strip().split()
        if parts[:1] == ["press"] and len(parts) == 2 and parts[1] in BUTTON_PHASES:
            if self.button is not None:
                return "ERR busy"
            self.button = parts[1]
            self.command_started_s = time.monotonic()
            self.release_pending = True
            return f"OK press {parts[1]}"
        if parts[:2] == ["set", "signature"] and len(parts) == 3:
            signature = bytes.fromhex(parts[2])
            assert len(signature) == 10
            self.signature[:] = signature
            return "OK set signature"
        raise RuntimeError(line)

    def write(self, data: bytes) -> None:
        self.rx.extend(data)
        self._process()

    def read_available(self, timeout: float) -> bytes:
        if self.pending:
            data = bytes(self.pending)
            self.pending.clear()
            return data
        time.sleep(min(timeout, 0.001))
        return b""

    def close(self) -> None:
        pass

    def _process(self) -> None:
        while self.rx:
            expected = master_expected_len(bytes(self.rx))
            if expected is None:
                return
            if expected < 0 or expected > 32:
                del self.rx[0]
                continue
            if len(self.rx) < expected:
                return
            frame = bytes(self.rx[:expected])
            if not protocol.crc_ok(frame):
                del self.rx[0]
                continue
            response = self._handle_frame(frame)
            del self.rx[:expected]
            if response is not None:
                self.pending.extend(response)

    def _handle_frame(self, frame: bytes) -> bytes | None:
        if frame[1] == protocol.FC_WRITE_MULTIPLE_REGISTERS:
            return None
        if frame[0] != self.slave_id:
            return None
        read_addr = read_be16(frame, 2)
        read_qty = read_be16(frame, 4)
        write_addr = read_be16(frame, 6)
        write_qty = read_be16(frame, 8)
        if read_addr != protocol.REG_STATUS_READ or write_addr != protocol.REG_COMMAND_WRITE:
            return None
        if read_qty == 5 and write_qty == 3 and frame[10] == 6 and frame[11] == 0 and frame[12] == 2:
            return scan_response(self.slave_id, bytes(self.signature))
        if read_qty == 8 and write_qty == 2 and frame[10] == 4:
            return status_response(self.slave_id, frame[11], frame[12], self._next_button_phase())
        if read_qty == 2 and write_qty == 2 and frame[10] == 4:
            return command_response(self.slave_id, frame[11], frame[12])
        return None

    def _next_button_phase(self) -> tuple[int, int, int] | None:
        if self.button is None:
            return None
        assert self.command_started_s is not None
        press, release = BUTTON_PHASES[self.button]
        if time.monotonic() - self.command_started_s < 0.100:
            return press
        if self.release_pending:
            self.release_pending = False
            self.button = None
            self.command_started_s = None
            return release
        self.button = None
        self.command_started_s = None
        return None


def read_response(transport: object, timeout: float = 0.25) -> bytes | None:
    deadline = time.monotonic() + timeout
    rx = bytearray()
    while time.monotonic() < deadline:
        chunk = transport.read_available(max(deadline - time.monotonic(), 0.0))  # type: ignore[attr-defined]
        if chunk:
            rx.extend(chunk)
        while rx:
            expected = protocol.response_expected_len(bytes(rx))
            if expected is None:
                break
            if expected < 0:
                del rx[0]
                continue
            if len(rx) < expected:
                break
            frame = bytes(rx[:expected])
            if protocol.crc_ok(frame):
                return frame
            del rx[0]
    return None


def compare_transaction(dut: SocketPairHostTransport, ref: ReferenceHCP2Responder, frame: bytes) -> bytes | None:
    dut.write(frame)
    ref.write(frame)
    actual = read_response(dut)
    expected = read_response(ref)
    assert actual == expected
    return actual


def test_reference_emulator_matches_our_responder_scan_and_idle_polls() -> None:
    ref = ReferenceHCP2Responder()
    with SocketPairHostTransport(response_delay_us=0) as dut:
        assert dut.command(f"set signature {REFERENCE_SIGNATURE.hex()}") == "OK set signature"
        assert ref.command(f"set signature {REFERENCE_SIGNATURE.hex()}") == "OK set signature"

        scan = compare_transaction(dut, ref, protocol.bus_scan_request())
        assert scan == bytes.fromhex("02170A00000205043010FFA8450EDF")

        for counter in (0x00, 0x01, 0x3E, 0x70, 0xFF):
            response = compare_transaction(dut, ref, protocol.status_poll(counter))
            assert response is not None
            assert protocol.decode_response_kind(response) == "status"
            assert protocol.response_counter(response) == counter
            assert protocol.decode_status_button(response) is None


def test_reference_emulator_matches_our_responder_button_press_release_sequences() -> None:
    ref = ReferenceHCP2Responder()
    with SocketPairHostTransport(response_delay_us=0) as dut:
        assert dut.command(f"set signature {REFERENCE_SIGNATURE.hex()}") == "OK set signature"
        assert ref.command(f"set signature {REFERENCE_SIGNATURE.hex()}") == "OK set signature"

        counter = 0x10
        for button in BUTTON_PHASES:
            assert dut.command(f"press {button}") == f"OK press {button}"
            assert ref.command(f"press {button}") == f"OK press {button}"

            press = compare_transaction(dut, ref, protocol.status_poll(counter))
            assert protocol.decode_status_button(press or b"") == button

            time.sleep(0.110)
            release = compare_transaction(dut, ref, protocol.status_poll((counter + 1) & 0xFF))
            assert protocol.decode_status_button(release or b"") == button

            idle = compare_transaction(dut, ref, protocol.status_poll((counter + 2) & 0xFF))
            assert protocol.decode_status_button(idle or b"") is None
            counter = (counter + 3) & 0xFF


def test_reference_emulator_matches_our_responder_command_ack_and_fault_recovery() -> None:
    ref = ReferenceHCP2Responder()
    with SocketPairHostTransport(response_delay_us=0) as dut:
        light_ack = compare_transaction(dut, ref, protocol.light_command(0x02, enabled=True))
        assert light_ack == bytes.fromhex("021704020004FD08DE")

        broadcast = protocol.broadcast_status(protocol.BroadcastState(target=200, current=200, state=0x20, light=0x14))
        assert compare_transaction(dut, ref, broadcast) is None

        bad_crc = bytearray(protocol.status_poll(0x44))
        bad_crc[-1] ^= 0x55
        assert compare_transaction(dut, ref, bytes(bad_crc)) is None

        recovered = compare_transaction(dut, ref, protocol.status_poll(0x45))
        assert recovered is not None
        assert protocol.response_counter(recovered) == 0x45
