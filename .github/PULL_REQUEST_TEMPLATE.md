## Problem and decision

- Problem:
- Chosen behavior:
- Key tradeoff:

## Verification

- [ ] 1.75C firmware build: `./scripts/build-v5.sh`
- [ ] Core/transport/display suites: `ctest --test-dir build-host --output-on-failure`
- [ ] Tool-template suite: `ctest --test-dir build-tools-host --output-on-failure`
- [ ] Python desk tests: `python3 -m unittest scripts/test_jarvis_desk.py`
- [ ] Cockpit JavaScript: `node scripts/check-dashboard-js.mjs main/diagnostics.html`
- [ ] Shell syntax: `bash -n scripts/*.sh`
- [ ] Hygiene: `./scripts/check-secrets.sh && git diff --check`
- [ ] Real changed surface exercised; evidence attached or explicitly marked unavailable

Check only commands relevant to the change. Never mark a physical, audible, or
panel-output claim complete from a host test, HTTP response, software mirror, or
PCM tap alone.

## Hardware and evidence

- Board/revision:
- Flash or OTA path:
- Physical checks:
- Software/counter evidence:
- Not verified:

## Security and compatibility

- [ ] No credentials, NVS/flash dumps, private endpoints, identifiers, or device logs added
- [ ] Physical privacy/consent authority is unchanged or explicitly tested
- [ ] Original 1.75, XIAO, Android, dashboard, and ESP-Claw material is labeled compatibility/history

## Remaining blockers

-
