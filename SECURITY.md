# Security Policy

## Supported Status

JarvisNano is pre-1.0 hardware/firmware software. Security-sensitive behavior is
still evolving, especially around trusted-LAN HTTP writes, pairing, and future
BLE privacy mode.

## Reporting A Vulnerability

Please do not open a public issue for vulnerabilities that expose secrets,
allow unauthorized control of a device, or weaken local-network trust
boundaries.

Report privately by emailing `security@ingeniousdigital.com` with:

- A clear description of the issue
- Affected commit or release
- Reproduction steps
- Whether physical access, LAN access, BLE range, or cloud credentials are
  required

We will acknowledge reports as quickly as practical and coordinate disclosure
before publishing fixes.

## Secrets And Local Data

Never commit:

- Wi-Fi SSIDs/passwords
- LLM API keys
- Bot tokens
- Serial logs containing private configuration
- Local paths to private memory, notes, or credential stores

The dashboard stores connection state in browser `localStorage`; treat browser
profiles used for hardware bring-up as local development state, not release
artifacts.
