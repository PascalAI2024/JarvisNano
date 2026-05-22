# Build Toolchain

**What it is** — The build environment for JarvisRobot firmware: ESP-IDF `v5.5.1` in the `espressif/idf:v5.5.1` Docker image, patched with `tools/esp-idf.patch`, with two version pins that must be applied before every build.

**How we use it here** — `scripts/bootstrap.sh` orchestrates the build: it clones or validates `esp-claw`, applies patches, installs Python deps, and calls `idf.py build` inside the container. The flash path invokes `esptool.py` via the same container.

---

## Findings & gotchas

**[2026-05-21] Pin `idf-component-manager==2.4.10` before every build**

`espressif/idf:v5.5.1` ships with `idf-component-manager==2.3.0`, which introduced a strict "Already defined root local requirement" check. This codebase intentionally declares some local components in both parent and child `idf_component.yml` files. `2.3.0` rejects this and aborts the build.

Fix: always prepend the pip upgrade before `idf.py build`:
```bash
docker run --rm -v "$(pwd):/project" -e IDF_TARGET=esp32s3 \
  -w /project/esp-claw/application/edge_agent espressif/idf:v5.5.1 \
  bash -c "pip install -q 'idf-component-manager==2.4.10' && idf.py build"
```

`2.4.10` comes from the `release-v5.5` image; use the versioned `v5.5.1` image (not the rolling `release-v5.5` tag) because `tools/esp-idf.patch` applies cleanly only to the exact IDF in `v5.5.1`.

Source: `scripts/bootstrap.sh` (build step); memory file `feedback_build_idf_component_manager.md`.

**[2026-05-21] Pin `esp-bmgr-assist==0.5.0`**

`esp-bmgr-assist` v0.8.x dropped the `-c <boards_dir>` flag and the `boards/<vendor>/<name>/` scan path. Versions 0.6 and 0.7 also broke `-c` / `-b` flags. Only `0.5.x` finds this project's `boards/waveshare/` and `boards/seeed/` directories.

Apply to `scripts/bootstrap.sh` (around lines 62 and 104) and any ad-hoc docker invocations:
```bash
pip install "esp-bmgr-assist==0.5.0"
```

This affects all boards including the XIAO path. Failure mode: the build completes but generates wrong or empty board code without a clear error.

Source: `scripts/bootstrap.sh`; memory file `feedback_esp_bmgr_assist_pin.md`.

**[2026-05-21] Building with a dirty `esp-claw` clone: use `ESP_CLAW_REF`**

`scripts/bootstrap.sh` has a `clone_or_update_esp_claw` guard that calls `die` if the clone HEAD differs from the pinned `ESP_CLAW_REF` (default: `6a211756`). During active development the clone will have local commits. Override it:

```bash
ESP_CLAW_REF=$(cd esp-claw && git rev-parse HEAD) \
  BOARD_VENDOR=waveshare \
  BOARD_NAME=esp32s3_touch_amoled_1_75 \
  /abs/path/to/scripts/bootstrap.sh build
```

Use an absolute path for `bootstrap.sh` — background shells reset `cwd` and a relative path exits with code 127. All `apply_*_patch` steps are idempotent (guard-checked), so re-running is safe.

Recommended: commit the clone to a wip branch first (durable snapshot), then use that SHA as `ESP_CLAW_REF`.

Source: `scripts/bootstrap.sh`; memory file `feedback_build_and_flash_recipe.md`.

**[2026-05-21] New conditional components need two `idf.py build` passes on a clean tree**

On a completely clean build (after `rm -rf build/`), a new conditional component (e.g. `cap_gemini_live`) may require two successive `idf.py build` invocations. The first pass discovers the component and writes the sdkconfig; the second builds it. This is an IDF cmake artifact, not a bug in our code.

Also: new conditional components must initially be declared unconditionally in `idf_component.yml` — the chicken-and-egg: IDF cannot detect the component exists until it is included.

**[2026-05-21] sdkconfig regeneration for new board devices**

To enable a new device type in `esp_board_manager`:

1. Add the device to `board_devices.yaml`.
2. Add `CONFIG_ESP_BOARD_DEV_*_SUPPORT=y` to `board_manager.defaults` AND `sdkconfig.defaults` (belt and suspenders).
3. **Delete the project-root `sdkconfig`**.
4. Run `idf.py build` — `idf_ext.py` injects `board_manager.defaults`, generates a fresh `sdkconfig`, cmake picks up new include paths, build succeeds.

Why: `idf_ext.py` only injects `board_manager.defaults` when no `sdkconfig` file exists. A stale `sdkconfig` always wins over `sdkconfig.defaults` and silently drops the new flags.

Source: memory file `feedback_sdkconfig_regeneration.md`.

**[2026-05-21] USB-JTAG console is single-owner — boot-loop gotcha**

If any component calls `usb_serial_jtag_driver_install()` before `app_claw_cli_start`, then `app_claw_cli_start` calls `esp_console_new_repl_usb_serial_jtag()` which aborts with `ESP_ERR_INVALID_STATE` → infinite boot loop (the emote/lobster animation flashes each reset).

Rule: keep the `usb_diag` diagnostic shell (`jarvis-usb>` prompt) disabled. The `app>` CLI is the keeper — it has `status`, `wifi`, `scan`, and `reboot` commands.

**[2026-05-21] Watchdog-reset flash recovery: BOOT hold**

If a watchdog reset traps the board in an unflashable state:
1. Unplug USB.
2. Hold the BOOT button.
3. Replug USB while holding BOOT.
4. Release BOOT — board enters ROM download mode.
5. Reflash normally.

Flash command for this board: `idf.py flash --flash-mode dio` (DIO mode required; see [gemini-live-api.md](./gemini-live-api.md) for why this matters specifically for the `cap_gemini_live` build).

---

## Primary sources

| Source | Notes |
|--------|-------|
| `scripts/bootstrap.sh` | Authoritative build orchestrator. Check lines ~62 and ~104 for pip pins. |
| `tools/esp-idf.patch` | The patch that must apply cleanly to `espressif/idf:v5.5.1`. |
| `esp-claw/application/edge_agent/components/app_config/` | NVS field registration — three-file pattern. See [jarvismcp-bridge.md](./jarvismcp-bridge.md). |

---

## Open questions

- Is there a CI matrix that validates the `esp-bmgr-assist` pin after new releases? Without it, a `pip install` without the pin will silently break builds again.
- Would pre-committing the patched `esp-claw` tree (instead of patching at bootstrap time) remove the `ESP_CLAW_REF` dance?

---

## See also

- [board-manager.md](./board-manager.md) — `esp-bmgr-assist`, sdkconfig regeneration, generated files.
- [waveshare-amoled-175.md](./waveshare-amoled-175.md) — `BOARD_VENDOR` / `BOARD_NAME` values, flash mode.
- [llm-config.md](./llm-config.md) — NVS recovery via `esptool erase-region`.
