# Protocol Notes

These notes summarize the HCP1/UAP1 behavior used by this firmware.

## Physical Layer

- RS-485-like half-duplex bus
- 19200 baud
- 8 data bits, no parity, 1 stop bit
- Opener is the bus master
- ESP32 behaves as a UAP1-like slave

## Known Values

| Meaning | Value |
| --- | --- |
| Broadcast address | `0x00` |
| Drive/master address | `0x80` |
| UAP1 address | `0x28` |
| Slave scan command | `0x01` |
| Slave status request | `0x20` |
| UAP1/slave status response | `0x29` |
| CRC polynomial | `0x07` |
| CRC initial value | `0xF3` |

## Bus Flow

1. The opener broadcasts status.
2. After power-up, the opener scans for slaves.
3. The ESP answers as UAP1 address `0x28`.
4. The opener sends status requests.
5. The ESP responds with the UAP1 status response and momentary command bits.

Command bits are pulsed and then cleared. They must not be latched indefinitely.

## E2 Caveats

The tested SupraMatic E2 emits one-byte status broadcasts. The firmware accepts that shape and treats missing second-byte fields conservatively. Unknown state is never treated as closed.

The observed obstruction behavior did not expose a clean protocol error bit. The firmware therefore detects close failure by waiting for the HCP closed bit after a calibrated full-close estimate.
