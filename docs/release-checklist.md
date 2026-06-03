# Release Checklist

Use this before making the repository public or publishing a tagged release.

## Repository Privacy

- [ ] `secrets.yaml` is not tracked.
- [ ] `.esphome/`, `.venv/`, `.upstream/`, and top-level `captures/` are not tracked.
- [ ] No Home Assistant tokens, ESPHome API keys, OTA passwords, or proxy tokens are present.
- [ ] No private IPs, exact serial numbers, local usernames, or absolute local paths are present in the current tree.
- [ ] Git history has been checked or rewritten for the same private details.
- [ ] Commit author metadata is acceptable for public release.

## Documentation

- [ ] README quick start works from a fresh clone.
- [ ] Hardware wiring docs match the current recommended adapter.
- [ ] Safety docs clearly warn about remote garage-door movement.
- [ ] FAQ covers RJ12, termination, PoE, HomeKit Bridge, light state, and compatibility.
- [ ] Release notes explain tested hardware and known limitations.

## Build and Test

- [ ] `uv sync`
- [ ] `cp configs/secrets.example.yaml configs/secrets.yaml`
- [ ] `uv run esphome config configs/supramatic-e2.yaml`
- [ ] `uv run esphome config configs/supramatic-e2-minimal.yaml`
- [ ] `uv run esphome config configs/supramatic-e2-monitor.yaml`
- [ ] `uv run esphome config configs/supramatic-e2-proxy.yaml`
- [ ] `uv run esphome compile configs/supramatic-e2.yaml`
- [ ] `uv run garage-decode-phone-sync-video --self-test`

## GitHub

- [ ] Repository name includes `hoermann`, `supramatic`, `e2`, and `esphome`.
- [ ] Repository description includes Hörmann/Hoermann/Hormann, SupraMatic E2, HCP1, UAP1, ESPHome, RS-485, and Home Assistant.
- [ ] Topics include `home-assistant`, `esphome`, `esp32`, `esp32-s3`, `garage-door`, `hormann`, `hoermann`, `supramatic`, `supramatic-e2`, `hcp1`, `uap1`, `rs485`, `homekit`, `ethernet`, `poe`, and `waveshare`.
- [ ] GitHub Actions are green.
- [ ] License is shown as The Unlicense.
- [ ] Upstream attribution is visible in `NOTICE.md`.
