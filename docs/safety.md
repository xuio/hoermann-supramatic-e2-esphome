# Safety Notes

- Be physically present at the door during first open, stop, light, venting, and close tests.
- Keep `allow_remote_close: false` until broadcast state decoding and obstruction protection have been verified.
- Keep `allow_remote_impulse: false` unless you are physically present and deliberately testing the raw impulse behavior.
- Keep `use_unverified_stop_command: false` unless the E2 raw stop word has been confirmed on your opener.
- Movement commands require a fresh valid HCP broadcast, a known non-stopped state, and no active error/prewarn.
- `listen_only: true` rejects all Home Assistant commands immediately.
- Do not expose the cover to HomeKit until Home Assistant state is reliable.
- Do not build automations that repeatedly call close.
- Prefer reporting unknown/open over falsely closed. This component only reports closed when the decoded closed bit is present.
- Remote closing should only be enabled with working photocell or equivalent obstruction protection.
- If the drive reports errors after communication loss, power-cycle the opener and capture logs from startup.
