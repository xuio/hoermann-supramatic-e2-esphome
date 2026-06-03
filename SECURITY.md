# Security and Safety Policy

This project controls a physical garage door. Treat security issues and safety issues as high impact even when the software bug looks small.

## Reporting Issues

For vulnerabilities that could allow unauthorized door movement, bypass command gating, leak Home Assistant credentials, or expose protocol access, please open a private report if the hosting platform supports it. If private reporting is not available, open a minimal public issue without exploit details and ask for a private contact path.

For normal bugs, sanitized protocol logs and reproduction steps are welcome in public issues.

## Do Not Share

Do not publish:

- Home Assistant long-lived access tokens
- ESPHome API encryption keys
- OTA passwords
- Publicly routable IP addresses
- Exact home address, garage photos, or serial numbers unless you intentionally choose to disclose them
- Raw captures that contain unrelated private network or device information

## Safety Baseline

Remote closing should only be enabled after physical safety hardware and state reporting are verified. During development, keep the door in view and use a configuration that blocks remote close and impulse commands.
