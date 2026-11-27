# Push audit, 2026-11-20 to 2026-11-27

Scope: all 16 commits on `main` since 2026-11-20 (`3c37d09` through `89de631`),
every added line of the full diffs. Method: pattern sweep for provider keys and
tokens, private or internal hostnames, private/LAN/CGNAT addresses, absolute
local paths, MAC addresses, e-mail addresses, credential-bearing URLs, internal
project or client names, and provider account or routing config; followed by a
manual read of every URL and host-like string. Values are not reproduced here.

**Result: no real secret found. Nothing to ROTATE.** History rewriting is out of scope.

| Commit | File:line | Category | Severity | Fix |
|---|---|---|---|---|
| `cafb540` | `components/jr_power/include/jr_power/analytics.h:2`, `components/jr_power/src/analytics.c:2`, `host/test_power_analytics.c:2` | Personal name in copyright header | Info | None: intentional maintainer attribution, same as the LICENSE |
| `3c37d09`, `523125a` | `scripts/gesture-doctor.py:50`, `scripts/screens.py:39`, `docs/BUILD.md:93` | Reverse-DNS keychain service id | Info | None: public app id, already in the Android package; not a credential |
| `523125a`, `cafb540`, `ff939fd` | `scripts/test_jarvis_desk.py` (many) | mDNS test hostname (`device.local`) | Info | None: generic placeholder; allowlisted in the guard |
| `cafb540` | `scripts/jarvisctl.py:518-520` | Windows install paths (Program Files, not a user profile) | Info | None: no user name in path |
| `523125a` | `scripts/test_jarvis_desk.py:750-776` | Test pairing-token variable | Info | None: value is generated in-test, not a literal credential |

Categories with zero findings: provider API keys/tokens, private/VPS/internal
hostnames, Tailscale or LAN addresses (fixtures use documentation-range 192.0.2.x),
absolute home-directory paths, MAC addresses, e-mail addresses, internal
company/client/agent/repo names, model-routing tables, provider account ids.
The Gemini model id change in `56d4583` is a public model name.

## Guard added

- `.gitleaks.toml`: stock provider rules plus custom rules for private host
  suffixes, CGNAT and RFC1918 addresses, personal-home absolute paths, and
  credentials embedded in URLs. Allowlist: documentation-range IPs, reserved
  TLDs, `device.local`, `YOUR_*` placeholders, and the files that define patterns.
- `.github/workflows/secret-scan.yml`: gitleaks on every push and PR.
- `scripts/hooks/install.sh`: opt-in local pre-push hook (`check-secrets.sh`, plus gitleaks if installed).

## History before 2026-11-20

Full-history scan (`gitleaks detect --log-opts=HEAD`, v8.21.2, this repo's
config before the allowlist below): **101 hits in 7 groups. No provider key,
token or credential found. Nothing to ROTATE.** None of these values is in the
current tree. Values are not reproduced; history rewriting is out of scope.

| Rule / group | Count | Example (file:line, commit) | Verdict |
|---|---|---|---|
| `lan-ip`: ESP32 SoftAP default address | 29 (15 files) | `scripts/check-secrets.sh:26`, `289f8bd` | PUBLIC-BY-DESIGN: the documented factory SoftAP address that every setup guide tells users to open |
| `lan-ip`: RFC1918 fixtures in a C test | 15 (1 file) | `components/jr_display/tests/test_shell.c:427`, `ca3a430` | FALSE POSITIVE: made-up addresses fed to the shell test parser |
| `lan-ip`: a real home-LAN device address | 7 (3 files) | `scripts/jarvis-desk.py:23`, `2e1eab4`; `CURRENT_WORK.md:26`, `ee2b177` | REAL, not a credential: private LAN address of a developer's device, default host in an early script and a handoff note. Unroutable from outside; no rotation. Owner decides on history rewrite |
| `absolute-local-path`: Windows profile path | 45 (6 files) | `docs/evidence/t107-companion-device/C-after-device.log:2`, `82b3102` | REAL, not a credential: OS account name in evidence logs (build output). Personal identifier only; no rotation. Owner decides on history rewrite |
| `absolute-local-path`: macOS home path | 2 (2 files) | `CURRENT_WORK.md:6`, `ee2b177`; `docs/reference/aec-barge-in.md:371`, `f453c9c` | REAL, not a credential: same category as above |
| `generic-api-key`: test fixture and RFC sample | 3 (3 files) | `scripts/test_tools_budget.py:191`, `70e7fbf`; `scripts/http-matrix.sh:84`, `226a965` | FALSE POSITIVE: placeholder test key; the RFC 6455 sample WebSocket nonce; a SHA-256 digest of a captured response in an evidence json (`docs/evidence/t107-privacy-restored.json:140`, `6f9f1cf`) |

The scan also reports 3 `gcp-api-key` hits on remote-tracking refs of the
throwaway proof branches (deliberate fake key, deleted); they are not on `main`.

Allowlist added to `.gitleaks.toml` (exact-value, so new look-alikes still fail):
the SoftAP address, the three fixture addresses, and the three false-positive
`generic-api-key` strings (46 of the 101 hits). The remaining 54 hits (7 LAN
address, 47 home paths) are deliberately left flagged: they are real personal
identifiers, so a full-history scan stays non-clean until the owner either
rewrites history or accepts them by adding fingerprints to a `.gitleaksignore`.
CI scans only new commits, so it is unaffected.
