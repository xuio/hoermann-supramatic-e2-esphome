# Contributing

Contributions are welcome when they improve an existing feature, clarify documentation, or add careful support for another HCP1/UAP1-era opener.

## Before Opening a Pull Request

1. Keep the change focused.
2. Avoid adding new abstractions unless they remove real complexity.
3. Run the validation commands:

```bash
uv sync
cp configs/secrets.example.yaml configs/secrets.yaml
uv run esphome config configs/supramatic-e2.yaml
uv run esphome config configs/supramatic-e2-minimal.yaml
uv run esphome config configs/supramatic-e2-monitor.yaml
uv run esphome config configs/supramatic-e2-proxy.yaml
uv run garage-decode-phone-sync-video --self-test
```

Compile the main firmware before submitting protocol or component changes:

```bash
uv run esphome compile configs/supramatic-e2.yaml
```

## Sharing Logs

Protocol logs are useful, but sanitize them first:

- Remove Home Assistant tokens, ESPHome API keys, OTA passwords, and private URLs.
- Do not include exact home address, public IPs, serial numbers, or photos that identify the location.
- Include opener model, series, visible index letter, board, RS-485 adapter, and whether termination is installed.

## Useful Bug Reports

Good reports include:

- Firmware commit hash
- ESPHome version
- YAML variant used
- Wiring summary
- Whether the opener sees the emulated UAP1
- Sanitized `/stats`, `/broadcast_status`, and persistent-log binary if protocol behavior is involved
- Exact Home Assistant entity states for UI or HomeKit issues

## Documentation Style

Keep user documentation practical and installation-oriented. Put reverse-engineering details under `docs/research/` or `docs/protocol-notes.md` so the main setup path stays readable.
