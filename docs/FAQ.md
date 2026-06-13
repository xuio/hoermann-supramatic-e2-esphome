# FAQ

## Is this an official Hörmann integration?

No. This is an unofficial reverse-engineered ESPHome integration for HCP1/UAP1-era openers.

## Does it require a physical UAP1?

No. The ESP32 emulates the UAP1 accessory on the HCP1 bus.

## Does it require a PIC, ESP8266, or second MCU?

No. The intended design is one ESP32 plus an RS-485 adapter. The RS-485 adapter is not a second MCU in this design.

## Does it work with Series 4, HCP2, UAP1-HCP, or BlueSecur?

Series 4 / HCP2 support is under development for a separate ESP32-C6 target. The current
codebase includes a protocol core, simulator, LP-core firmware, Wokwi full-firmware
harness, dual-ISS mailbox harness, and ESPHome component skeleton. It is still
simulation-first and bench-only: Wokwi is the primary no-hardware full-firmware gate on
the fixed ESP32-C6 native LP-UART backend, the local ISS covers deterministic
mailbox/FIFO/MMIO checks, and the first ESP32-C6 plus USB-RS485 HIL bench now passes
polling, fault, command, CPU-only reset, OTA, API restart, and Wi-Fi disruption
scenarios. Intermediate position moves now arm an LP-core stop trigger so the LP can
press stop if the HP side dies mid-move. The remaining known unsafe case is USB serial
flashing / download-mode reset while attached to the bus; real-motor testing must use
OTA-only operation or physically isolate the transceiver during serial flashing.

BlueSecur cloud/app integrations are out of scope.

## Can I use Wi-Fi instead of Ethernet?

The primary YAML is Ethernet-only for the Waveshare ESP32-S3-ETH board. ESPHome does not run Wi-Fi and Ethernet at the same time in one firmware config. A Wi-Fi variant would need a separate YAML and different hardware assumptions.

## Can I power the ESP from the opener's 24 V bus pin?

Only with a suitable isolated/regulating power design. The documented setup uses USB-C or IEEE 802.3af PoE and leaves BUS pin 2 disconnected.

## Is RJ11 enough?

Usually not. Use RJ12 / 6P6C and verify all needed conductors. Many telephone cables are 6P2C or 6P4C and will not expose every pin.

## Should I enable the 120 ohm terminator?

Leave it open for first tests. With all power removed, measure A/B resistance on the connected bus. Enable the adapter terminator only if the adapter is at an unterminated end of the RS-485 segment.

## Why is the light optimistic?

The tested SupraMatic E2 emits one-byte status broadcasts that do not carry a reliable light state. The Home Assistant light is therefore optimistic for manual toggles, with courtesy-light estimation during door movement.

## Why is there a separate target-position number?

Home Assistant garage-door cards may hide position sliders for `device_class: garage`. The cover still supports position, and the separate number entity gives a visible slider for percentage testing.

## Do I need visual calibration?

No. Visual calibration is fully optional. You can use the project as a normal garage-door controller for open, close, stop, light, state reporting, diagnostics, OTA, and HomeKit Bridge without filming the door or printing markers.

The visual calibration workflow is only for tuning percentage position control. It is intentionally a bit overengineered because it was built to measure and audit the timing model: printed ArUco markers track the door in video, a QR code aligns the video to ESP/HCP logs, and the analysis turns that into motion curves.

## What happens if I skip position calibration?

The door still works as a garage-door cover. You should rely on open/close/stop and confirmed endpoint state. Intermediate percentage targets may be less accurate, especially on doors whose speed profile differs from the tested SupraMatic E2.

## How does HomeKit work?

HomeKit is handled by Home Assistant's HomeKit Bridge. The ESP32 only exposes ESPHome entities to Home Assistant.

## What should I share when my opener behaves differently?

Share model, series, index letter, hardware wiring, YAML variant, and sanitized protocol captures. Do not share secrets, exact address, public IPs, or serial numbers.
