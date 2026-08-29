# Security policy

## Supported status

JarvisNano is pre-1.0 hardware/firmware. The primary 1.75C build is suitable for
a controlled development LAN, not an untrusted network or public firmware
release.

Current boundaries:

- The root page plus coarse display, touch, and sensor counters are available
  over local HTTP. Content-bearing diagnostics—cockpit/session detail, logs,
  audio taps, transcripts, Agent Link, and display pixels—require pairing.
- Protected writes require a pairing token; control-intent routes also require
  `X-JarvisNano-Control: 1`.
- HTTP is plaintext. Tokens authorize requests but do **not** encrypt them.
- Pairing tokens are stored as SHA-256 hashes in NVS and are never readable back.
- A 1.5–5 second runtime BOOT hold opens the visible, one-shot 60-second pairing
  window. BOOT held during reset remains the ROM downloader path.
- Wi-Fi, Gemini, and JarvisMCP credentials are currently recoverable plaintext
  in unencrypted NVS. Physical flash/NVS access or a shared dump exposes them.
- Trusted-LAN dual-slot OTA and rollback probation work. Signed application
  verification, authenticated encrypted update transport, and attended
  NVS/flash encryption provisioning remain public-release blockers.
- Secure Boot, anti-rollback, flash encryption, and eFuse changes require a
  separately attended hardware procedure.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the route/auth matrix and
[`docs/BUILD.md`](docs/BUILD.md#ota-release-gate) for the release security gate.

## Reporting a vulnerability

Do not open a public issue for a vulnerability that exposes secrets, enables
unauthorized device control, bypasses physical consent/privacy, or weakens the
local-network boundary.

Prefer a [private GitHub security advisory](https://github.com/PascalAI2024/JarvisNano/security/advisories/new)
or email `security@ingeniousdigital.com`. Include:

- a clear description;
- affected commit or release;
- minimal reproduction steps;
- whether physical access, LAN access, cloud credentials, or an existing pairing
  token is required;
- any logs after removing tokens, keys, addresses, SSIDs, MACs, and personal
  content.

We will acknowledge the report as quickly as practical and coordinate disclosure
before publishing a fix.

## Secrets and local data

Never commit or paste into issues:

- Wi-Fi SSIDs or passwords;
- Gemini or JarvisMCP credentials;
- pairing tokens, token hashes, or host keychain exports;
- NVS/flash dumps or factory backups;
- device IP/MAC addresses or private endpoints;
- OAuth state, browser sessions, local databases, or provider caches;
- serial/device logs containing any of the above;
- private memory, notes, or assistant content.

Use a dedicated, revocable JarvisMCP device credential. Never provision a general
desktop/company bearer onto the board. Keep credentials in device NVS or the
host keychain and out of command arguments, screenshots, diagnostics, and build
configuration.

The built-in developer cockpit can accept an existing token in a non-persistent
password field for same-origin requests; reloading clears it. Prefer the
Keychain-backed clients and never use the page on an untrusted LAN.

Run `./scripts/check-secrets.sh` before committing. It scans tracked and
untracked nonignored files, including printable metadata in binaries/media, and
excludes only the exact detector implementation, synthetic guard fixtures, and
scanner source. It cannot inspect image pixels or rewrite already-published Git
history; release review must cover both separately. Every reported hit blocks
publication until reviewed.

## Physical authority

Synthetic or remote input cannot clear hold/flip privacy, approve memory writes,
answer an on-device ask, or escape operator ownership. Remote resume is
privacy-safe and must refuse when physical privacy still applies. Treat any
bypass of those invariants as a security issue, not a UI defect.
