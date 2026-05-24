# Safety Notes

- Be physically present at the door during first open, stop, light, venting, and close tests.
- Keep `allow_remote_close: false` until broadcast state decoding and obstruction protection have been verified.
- Do not expose the cover to HomeKit until Home Assistant state is reliable.
- Do not build automations that repeatedly call close.
- Prefer reporting unknown/open over falsely closed. This component only reports closed when the decoded closed bit is present.
- Remote closing should only be enabled with working photocell or equivalent obstruction protection.
- If the drive reports errors after communication loss, power-cycle the opener and capture logs from startup.
