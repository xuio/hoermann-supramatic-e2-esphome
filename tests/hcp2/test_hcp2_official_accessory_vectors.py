from __future__ import annotations

import json
from pathlib import Path

from tools.supramatic_sim import protocol


ROOT = Path(__file__).resolve().parents[2]
VECTOR_PATH = ROOT / "tests" / "hcp2" / "vectors" / "official_accessory_series4_frames.jsonl"


def load_records() -> list[dict[str, object]]:
    return [json.loads(line) for line in VECTOR_PATH.read_text(encoding="utf-8").splitlines()]


def test_official_accessory_vectors_are_semantically_decoded() -> None:
    records = load_records()
    assert records

    seen_kinds: set[str] = set()
    seen_buttons: set[str] = set()
    seen_broadcast_states: set[int] = set()
    seen_light_words: set[int] = set()

    for record in records:
        frame = bytes.fromhex(str(record["raw"]))
        assert protocol.crc_ok(frame), record["name"]
        kind = str(record["kind"])
        seen_kinds.add(kind)

        slave_id = record.get("slave_id")
        if slave_id is not None:
            assert frame[0] == slave_id, record["name"]

        if kind == "status_poll":
            assert frame[1] == protocol.FC_READ_WRITE_MULTIPLE_REGISTERS
            assert frame[2:6] == protocol.REG_STATUS_READ.to_bytes(2, "big") + b"\x00\x08"
            assert frame[6:10] == protocol.REG_COMMAND_WRITE.to_bytes(2, "big") + b"\x00\x02"
            assert frame[11] == record["counter"]
            assert frame[12] == 0x03
        elif kind == "status_response":
            assert protocol.decode_response_kind(frame) == "status"
            assert protocol.response_counter(frame) == record["counter"]
            assert protocol.decode_status_button_phase(frame) is None
        elif kind == "button_response":
            assert protocol.decode_response_kind(frame) == "status"
            assert protocol.response_counter(frame) == record["counter"]
            decoded = protocol.decode_status_button_phase(frame)
            assert decoded == (record["button"], record["phase"]), record["name"]
            seen_buttons.add(str(record["button"]))
        elif kind == "command_arg_request":
            assert frame[1] == protocol.FC_READ_WRITE_MULTIPLE_REGISTERS
            assert frame[11] == record["counter"]
            assert frame[12] == 0x04
            assert frame[13:15].hex() == record["arg"]
        elif kind == "command_arg_response":
            assert protocol.decode_response_kind(frame) == "command"
            assert protocol.response_counter(frame) == record["counter"]
            assert frame[5] == 0x04
            assert frame[6] == 0xFD
        elif kind == "broadcast_status":
            assert frame[0] == 0x00
            assert frame[1] == protocol.FC_WRITE_MULTIPLE_REGISTERS
            assert frame[2:4] == protocol.REG_BROADCAST_STATUS.to_bytes(2, "big")
            data = frame[7:25]
            expected = record["expect"]
            assert isinstance(expected, dict)
            assert data[2] == expected["target"]
            assert data[3] == expected["current"]
            assert data[4] == expected["state"]
            assert data[5] == expected["detail"]
            assert data[13] == expected["light"]
            seen_broadcast_states.add(int(expected["state"]))
            seen_light_words.add(int(expected["light"]))
        else:
            raise AssertionError(f"unhandled vector kind {kind!r}")

    assert {"status_poll", "status_response", "button_response", "command_arg_request", "command_arg_response", "broadcast_status"} <= seen_kinds
    assert {"light", "open", "close"} <= seen_buttons
    assert {0x40, 0x09, 0x00, 0x01, 0x20, 0x02} <= seen_broadcast_states
    assert {0x00, 0x04, 0x14} <= seen_light_words
