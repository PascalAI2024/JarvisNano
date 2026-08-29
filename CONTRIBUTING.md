# Contributing

JarvisNano’s primary product is the 32 MB Waveshare 1.75C firmware. Small,
focused changes with evidence from the affected surface are easiest to review.
The original 1.75, XIAO/camera, dashboard, and Android trees are compatibility or
future tracks; identify that scope explicitly.

## Ground rules

- Never commit credentials, pairing material, NVS/flash dumps, OAuth/browser
  state, device logs, addresses, SSIDs, private endpoints, personal assistant
  content, or machine-specific paths.
- Work in `main/`, `components/jr_*`, the selected `boards/` definition,
  `scripts/`, and canonical `docs/` for the live image. `firmware/`,
  `esp-claw/`, and the older dashboard do not define the v5 release composition.
- Preserve one owner per realtime resource and one display/interaction grammar.
  Do not add a second renderer, transport authority, or input recognizer beside
  the shipped path.
- Name the board revision for every hardware-dependent change. A board swap
  invalidates measured gain, orientation, timing, current, and thermals.
- Match evidence to the claim: software mirrors are not panel readback; PCM taps
  are not audible output; synthetic input is not physical authority.

## Development setup

```bash
./scripts/build-v5.sh
./scripts/flash-v5.sh
```

`esp32s3_touch_amoled_1_75c` is the only release build. Original-board sources
remain a compatibility reference, but `build-v5.sh` refuses them until the
16 MB partition/flash and physical acceptance circuits are revalidated.

With CMake 3.16+ (or inside the pinned IDF container), portable host suites run
separately while PLAN N6.6 tracks one canonical command:

```bash
cmake -S host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure

cmake -S components/jr_tools/host -B build-tools-host
cmake --build build-tools-host
ctest --test-dir build-tools-host --output-on-failure

python3 -m unittest scripts/test_jarvis_desk.py
bash -n scripts/*.sh
```

Use [`docs/LIVE_DEVICE_DEBUG.md`](docs/LIVE_DEVICE_DEBUG.md) for runtime evidence
and [`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md) for release-shaped
verification.

## Pull requests

Before opening a PR:

1. Rebase on the current target branch without discarding unrelated work.
2. Run the narrow tests for the changed contract, then the applicable host suite
   and ESP-IDF build.
3. Run `git diff --check` and `./scripts/check-secrets.sh`.
4. Exercise the real surface: firmware on the 1.75C, browser for a web surface,
   or the actual CLI for tooling.
5. Read the final diff for generated files, stale compatibility claims, debug
   leftovers, and accidental secret-bearing artifacts.

Include:

- the problem and chosen behavior;
- affected files/components;
- exact commands and results;
- exact board and physical checks, or an explicit “not physically verified”;
- remaining blockers or release implications;
- screenshots/log excerpts only after required redaction.
