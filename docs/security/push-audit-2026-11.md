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
