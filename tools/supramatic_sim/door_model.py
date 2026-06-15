"""Behavioral SupraMatic 4 door model used by the HCP2 simulator.

Unknown motor behavior is deliberately parameterized. The defaults are
conservative and traceable to the current public captures, but edge behavior
such as reversal dwell and obstruction reporting must be calibrated on a real
motor.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .protocol import BroadcastState


STATE_STOPPED = 0x00
STATE_OPENING = 0x01
STATE_CLOSING = 0x02
STATE_MOVING_HALF = 0x05
STATE_MOVING_VENT = 0x09
STATE_VENT = 0x0A
STATE_OPEN = 0x20
STATE_CLOSED = 0x40
STATE_HALF = 0x80

POSITION_CLOSED = 0
POSITION_OPEN = 200
DEFAULT_HALF_POSITION = 0x18
DEFAULT_VENT_POSITION = 0x08

REVERSAL_STOP_THEN_REVERSE = "stop_then_reverse"
REVERSAL_IMMEDIATE = "immediate_reverse"
REVERSAL_IGNORE_UNTIL_STOP = "ignore_until_stop"
REVERSAL_PROFILES = {
    REVERSAL_STOP_THEN_REVERSE,
    REVERSAL_IMMEDIATE,
    REVERSAL_IGNORE_UNTIL_STOP,
}


STATE_NAMES = {
    STATE_STOPPED: "stopped",
    STATE_OPENING: "opening",
    STATE_CLOSING: "closing",
    STATE_MOVING_HALF: "moving_half",
    STATE_MOVING_VENT: "moving_vent",
    STATE_VENT: "vent",
    STATE_OPEN: "open",
    STATE_CLOSED: "closed",
    STATE_HALF: "half",
}


@dataclass
class DoorModel:
    """Small position integrator for simulator broadcasts.

    The model is intentionally cycle-based. One simulator cycle is one HCP2
    broadcast/poll loop, so the configured travel cycle count maps directly to
    the ~70 ms poll grid that the accessory can observe.
    """

    position: int = POSITION_CLOSED
    target: int = POSITION_CLOSED
    state: int = STATE_CLOSED
    light_word: int = 0x14
    travel_cycles: int = 260
    reversal_profile: str = REVERSAL_STOP_THEN_REVERSE
    reverse_dwell_cycles: int = 4
    stop_latency_cycles: int = 1
    overshoot_raw_ticks: int = 1
    half_position: int = DEFAULT_HALF_POSITION
    vent_position: int = DEFAULT_VENT_POSITION
    obstruction_cycle: int | None = None
    obstruction_reverses: bool = True
    speculative_obstruction_flags: bool = False
    cycle: int = 0
    pending_stop_cycles: int = 0
    pending_reverse_button: str | None = None
    reverse_wait_cycles: int = 0
    incomplete_cycles: int = 0
    obstruction_active: bool = False
    last_command: str | None = None
    events: list[dict[str, object]] = field(default_factory=list)

    def __post_init__(self) -> None:
        if self.travel_cycles < 1:
            self.travel_cycles = 1
        if self.reversal_profile not in REVERSAL_PROFILES:
            raise ValueError(f"unknown reversal profile: {self.reversal_profile}")
        self.position = clamp_position(self.position)
        self.target = clamp_position(self.target)
        self.half_position = clamp_position(self.half_position)
        self.vent_position = clamp_position(self.vent_position)

    @classmethod
    def open(cls, **kwargs: object) -> "DoorModel":
        return cls(position=POSITION_OPEN, target=POSITION_OPEN, state=STATE_OPEN, **kwargs)

    @classmethod
    def closed(cls, **kwargs: object) -> "DoorModel":
        return cls(position=POSITION_CLOSED, target=POSITION_CLOSED, state=STATE_CLOSED, **kwargs)

    def broadcast_state(self) -> BroadcastState:
        return BroadcastState(
            target=clamp_position(self.target),
            current=clamp_position(self.position),
            state=self.state & 0xFF,
            light=self.light_word & 0xFF,
        )

    def tick(self) -> None:
        self.cycle += 1
        self._maybe_inject_obstruction()

        if self.pending_stop_cycles > 0:
            self.pending_stop_cycles -= 1
            self._move_one_step(overshoot=True)
            if self.pending_stop_cycles == 0:
                self._stop("stop-latency-expired")
            return

        if self.reverse_wait_cycles > 0:
            self.reverse_wait_cycles -= 1
            if self.reverse_wait_cycles == 0 and self.pending_reverse_button is not None:
                button = self.pending_reverse_button
                self.pending_reverse_button = None
                self.command(button, source="reverse-dwell")
            return

        self._move_one_step()

    def command(self, button: str, *, source: str = "accessory") -> None:
        self.last_command = button
        self.events.append(
            {
                "cycle": self.cycle,
                "event": "command",
                "button": button,
                "source": source,
                "state_before": state_name(self.state),
                "position_before": self.position,
            }
        )
        if button == "open":
            self._direction_command(button, POSITION_OPEN, STATE_OPENING, STATE_OPEN)
        elif button == "close":
            self._direction_command(button, POSITION_CLOSED, STATE_CLOSING, STATE_CLOSED)
        elif button == "stop":
            self.pending_stop_cycles = max(0, self.stop_latency_cycles)
            if self.pending_stop_cycles == 0:
                self._stop("stop-command")
        elif button == "half":
            self._move_to(self.half_position, STATE_MOVING_HALF, STATE_HALF)
        elif button == "vent":
            self._move_to(self.vent_position, STATE_MOVING_VENT, STATE_VENT)
        elif button == "light":
            self.toggle_light()

    def toggle_light(self) -> None:
        self.light_word = 0x00 if self.light_word else 0x14
        self.events.append({"cycle": self.cycle, "event": "light", "light_word": self.light_word})

    def _direction_command(self, button: str, target: int, moving_state: int, done_state: int) -> None:
        if self._is_opposite_motion(moving_state):
            if self.reversal_profile == REVERSAL_IGNORE_UNTIL_STOP:
                self.events.append({"cycle": self.cycle, "event": "reverse_ignored", "button": button})
                return
            if self.reversal_profile == REVERSAL_STOP_THEN_REVERSE:
                self._stop("reverse-dwell")
                self.pending_reverse_button = button
                self.reverse_wait_cycles = max(1, self.reverse_dwell_cycles)
                return
        self._move_to(target, moving_state, done_state)

    def _is_opposite_motion(self, requested_moving_state: int) -> bool:
        return (
            self.state == STATE_OPENING and requested_moving_state == STATE_CLOSING
        ) or (
            self.state == STATE_CLOSING and requested_moving_state == STATE_OPENING
        )

    def _move_to(self, target: int, moving_state: int, done_state: int) -> None:
        self.target = clamp_position(target)
        self.pending_stop_cycles = 0
        self.pending_reverse_button = None
        self.reverse_wait_cycles = 0
        if self.position == self.target:
            self.state = done_state
            return
        self.state = moving_state

    def _move_one_step(self, *, overshoot: bool = False) -> None:
        if self.state not in {STATE_OPENING, STATE_CLOSING, STATE_MOVING_HALF, STATE_MOVING_VENT}:
            return
        if self.position == self.target:
            self._arrive()
            return

        step = max(1, round(POSITION_OPEN / self.travel_cycles))
        if overshoot:
            step = max(step, self.overshoot_raw_ticks)
        if self.position < self.target:
            self.position = min(self.target, self.position + step)
        else:
            self.position = max(self.target, self.position - step)
        if self.position == self.target and not overshoot:
            self._arrive()

    def _arrive(self) -> None:
        if self.target == POSITION_CLOSED:
            self.state = STATE_CLOSED
        elif self.target == POSITION_OPEN:
            self.state = STATE_OPEN
        elif self.target == self.half_position:
            self.state = STATE_HALF
        elif self.target == self.vent_position:
            self.state = STATE_VENT
        else:
            self.state = STATE_STOPPED

    def _stop(self, reason: str) -> None:
        self.target = self.position
        self.state = STATE_STOPPED
        self.events.append({"cycle": self.cycle, "event": "stopped", "reason": reason, "position": self.position})

    def _maybe_inject_obstruction(self) -> None:
        if self.obstruction_cycle is None or self.cycle != self.obstruction_cycle:
            return
        if self.state != STATE_CLOSING:
            self.events.append(
                {
                    "cycle": self.cycle,
                    "event": "obstruction_ignored",
                    "state": state_name(self.state),
                    "synthetic": True,
                }
            )
            return
        self.incomplete_cycles += 1
        self.obstruction_active = True
        self.events.append(
            {
                "cycle": self.cycle,
                "event": "obstruction",
                "position": self.position,
                "synthetic": True,
                "speculative_flags": self.speculative_obstruction_flags,
            }
        )
        if self.obstruction_reverses:
            self._move_to(POSITION_OPEN, STATE_OPENING, STATE_OPEN)
        else:
            self._stop("obstruction")

    def as_dict(self) -> dict[str, object]:
        return {
            "position": clamp_position(self.position),
            "target": clamp_position(self.target),
            "state": self.state,
            "state_name": state_name(self.state),
            "light_word": self.light_word & 0xFF,
            "last_command": self.last_command,
            "incomplete_cycles": self.incomplete_cycles,
            "obstruction_active": self.obstruction_active,
            "events": list(self.events),
            "parameters": {
                "travel_cycles": self.travel_cycles,
                "reversal_profile": self.reversal_profile,
                "reverse_dwell_cycles": self.reverse_dwell_cycles,
                "stop_latency_cycles": self.stop_latency_cycles,
                "overshoot_raw_ticks": self.overshoot_raw_ticks,
                "half_position": self.half_position,
                "vent_position": self.vent_position,
                "obstruction_cycle": self.obstruction_cycle,
                "obstruction_reverses": self.obstruction_reverses,
                "speculative_obstruction_flags": self.speculative_obstruction_flags,
            },
        }


def clamp_position(value: int) -> int:
    return max(POSITION_CLOSED, min(POSITION_OPEN, int(value)))


def state_name(value: int) -> str:
    return STATE_NAMES.get(value, f"unknown_0x{value:02x}")
