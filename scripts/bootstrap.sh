#!/usr/bin/env bash
# bootstrap.sh — clone esp-claw, drop in our board, apply codegen patch, optionally build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_CLAW_DIR="$ROOT/esp-claw"
ESP_CLAW_REPO="https://github.com/espressif/esp-claw.git"
ESP_CLAW_REF="${ESP_CLAW_REF:-6a211756a6ebf8d725173e294f582a6cf30c9592}"
BOARD_NAME="${BOARD_NAME:-xiao_esp32s3_sense}"
BOARD_VENDOR="${BOARD_VENDOR:-seeed}"
# Pinned to a specific point release. `release-v5.5` is a rolling tag and
# Espressif rebases the upstream IDF source on each push (e.g. fixes get
# back-ported), which silently breaks tools/esp-idf.patch and `gen-bmgr-config`
# CLI surface across pip releases. See CHANGELOG entry for the 2026-05 incident.
IDF_IMAGE="espressif/idf:v5.5.4"

log() { printf '\033[1;36m[bootstrap]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[bootstrap]\033[0m %s\n' "$*" >&2; exit 1; }

clone_or_update_esp_claw() {
    if [ -d "$ESP_CLAW_DIR/.git" ]; then
        local current
        current="$(git -C "$ESP_CLAW_DIR" rev-parse HEAD)"
        log "esp-claw already cloned at $ESP_CLAW_DIR ($current)"
        if [ "$current" != "$ESP_CLAW_REF" ]; then
            if [ -n "$(git -C "$ESP_CLAW_DIR" status --porcelain)" ]; then
                die "esp-claw is at $current, expected $ESP_CLAW_REF, and has local changes. Commit/stash them or remove esp-claw/ before bootstrapping."
            fi
            log "checking out pinned esp-claw ref $ESP_CLAW_REF"
            git -C "$ESP_CLAW_DIR" fetch --depth 1 origin "$ESP_CLAW_REF"
            git -C "$ESP_CLAW_DIR" checkout --detach FETCH_HEAD
        fi
    else
        log "cloning esp-claw@$ESP_CLAW_REF → $ESP_CLAW_DIR"
        git init "$ESP_CLAW_DIR"
        git -C "$ESP_CLAW_DIR" remote add origin "$ESP_CLAW_REPO"
        git -C "$ESP_CLAW_DIR" fetch --depth 1 origin "$ESP_CLAW_REF"
        git -C "$ESP_CLAW_DIR" checkout --detach FETCH_HEAD
    fi
    log "esp-claw pinned ref: $(git -C "$ESP_CLAW_DIR" rev-parse HEAD)"
}

copy_board() {
    local src="$ROOT/boards/$BOARD_VENDOR/$BOARD_NAME"
    local dst="$ESP_CLAW_DIR/application/edge_agent/boards/$BOARD_VENDOR/$BOARD_NAME"
    [ -d "$src" ] || die "unknown board $BOARD_VENDOR/$BOARD_NAME (missing $src)"
    log "copying board $BOARD_VENDOR/$BOARD_NAME → upstream tree"
    mkdir -p "$dst"
    cp -f "$src"/* "$dst/"
}

copy_firmware_assets() {
    local lua_src="$ROOT/firmware/lua"
    local rules_src="$ROOT/firmware/router_rules"
    local fatfs="$ESP_CLAW_DIR/application/edge_agent/fatfs_image"

    log "copying JarvisNano Lua + router rules → FATFS image"
    mkdir -p "$fatfs/scripts/builtin" "$fatfs/router_rules"
    cp -f "$lua_src"/*.lua "$fatfs/scripts/builtin/"
    cp -f "$rules_src"/router_rules.json "$fatfs/router_rules/router_rules.json"
}

copy_cap_gemini_live() {
    # The Gemini Live voice capability is a JarvisNano-authored component that does
    # not exist upstream in esp-claw. esp-claw/ is a gitignored pinned clone, so the
    # canonical source lives (git-tracked) under firmware/components/cap_gemini_live
    # and is copied into the generated tree on every bootstrap — same idiom as
    # copy_board. Without this, the component vanishes on a clean re-clone.
    local src="$ROOT/firmware/components/cap_gemini_live"
    local dst="$ESP_CLAW_DIR/components/claw_capabilities/cap_gemini_live"
    [ -d "$src" ] || die "missing vendored cap_gemini_live source at $src"
    log "copying cap_gemini_live component → upstream tree"
    mkdir -p "$dst/src" "$dst/include"
    cp -f "$src/CMakeLists.txt" "$dst/CMakeLists.txt"
    cp -f "$src/idf_component.yml" "$dst/idf_component.yml"
    cp -f "$src"/include/*.h "$dst/include/"
    cp -f "$src"/src/*.c "$dst/src/"
}

copy_jarvis_logger() {
    # Persistent ESP_LOG tee to rotating files on the SD card. Same idiom as
    # copy_cap_gemini_live: canonical source is git-tracked under
    # firmware/components/jarvis_logger and copied into the generated tree's
    # local project components dir (always on the IDF component search path) so
    # it survives a clean re-clone. main.c calls jarvis_logger_init("/sdcard")
    # after SD mount; http_server calls jarvis_logger_register_http() for /api/logs.
    local src="$ROOT/firmware/components/jarvis_logger"
    local dst="$ESP_CLAW_DIR/application/edge_agent/components/jarvis_logger"
    [ -d "$src" ] || die "missing vendored jarvis_logger source at $src"
    log "copying jarvis_logger component → upstream tree"
    mkdir -p "$dst/src" "$dst/include"
    cp -f "$src/CMakeLists.txt" "$dst/CMakeLists.txt"
    cp -f "$src"/include/*.h "$dst/include/"
    cp -f "$src"/src/*.c "$dst/src/"
}

copy_jarvis_brain() {
    # JARVIS's persistent on-device self (identity + memory on SD). Same idiom as
    # copy_jarvis_logger: vendored under firmware/components/jarvis_brain, mirrored
    # into the generated tree's local project components dir so it survives a clean
    # re-clone. main.c calls jarvis_brain_init("/sdcard"); cap_gemini_live's
    # gl_send_setup uses jarvis_brain_load_context() for the system instruction.
    local src="$ROOT/firmware/components/jarvis_brain"
    local dst="$ESP_CLAW_DIR/application/edge_agent/components/jarvis_brain"
    [ -d "$src" ] || die "missing vendored jarvis_brain source at $src"
    log "copying jarvis_brain component → upstream tree"
    mkdir -p "$dst/src" "$dst/include"
    cp -f "$src/CMakeLists.txt" "$dst/CMakeLists.txt"
    cp -f "$src"/include/*.h "$dst/include/"
    cp -f "$src"/src/*.c "$dst/src/"
}

copy_jarvis_pmic() {
    # AXP2101 battery telemetry (read-only). Same vendoring idiom as
    # copy_jarvis_brain: canonical source under firmware/components/jarvis_pmic,
    # mirrored into the generated tree's local project components dir so it
    # survives a clean re-clone. The http_server battery handler (patched by
    # apply_battery_real_read_patch) calls jarvis_pmic_read_battery().
    local src="$ROOT/firmware/components/jarvis_pmic"
    local dst="$ESP_CLAW_DIR/application/edge_agent/components/jarvis_pmic"
    [ -d "$src" ] || die "missing vendored jarvis_pmic source at $src"
    log "copying jarvis_pmic component → upstream tree"
    mkdir -p "$dst/src" "$dst/include"
    cp -f "$src/CMakeLists.txt" "$dst/CMakeLists.txt"
    cp -f "$src"/include/*.h "$dst/include/"
    cp -f "$src"/src/*.c "$dst/src/"
}

copy_jarvis_imu() {
    # QMI8658 accelerometer telemetry (read-only). Same vendoring idiom as
    # copy_jarvis_pmic: canonical source under firmware/components/jarvis_imu,
    # mirrored into the generated tree's local project components dir so it
    # survives a clean re-clone. The http_server imu handler (patched by
    # apply_imu_read_patch) calls jarvis_imu_read().
    local src="$ROOT/firmware/components/jarvis_imu"
    local dst="$ESP_CLAW_DIR/application/edge_agent/components/jarvis_imu"
    [ -d "$src" ] || die "missing vendored jarvis_imu source at $src"
    log "copying jarvis_imu component → upstream tree"
    mkdir -p "$dst/src" "$dst/include"
    cp -f "$src/CMakeLists.txt" "$dst/CMakeLists.txt"
    cp -f "$src"/include/*.h "$dst/include/"
    cp -f "$src"/src/*.c "$dst/src/"
}

copy_emote_runtime() {
    # The emote runtime is patched enough now (reactive face, display mirror,
    # CO5300 flush sync, small internal-DMA render strips) that keeping it as scattered
    # string patches is a fine way to rediscover old bugs. Keep the verified
    # source in firmware/emote and mirror it into the generated ESP-Claw tree.
    local src="$ROOT/firmware/emote"
    local dst="$ESP_CLAW_DIR/components/common/emote"
    [ -f "$src/emote.c" ] || die "missing vendored emote.c at $src/emote.c"
    [ -f "$src/emote.h" ] || die "missing vendored emote.h at $src/emote.h"
    [ -f "$src/CMakeLists.txt" ] || die "missing vendored emote CMakeLists at $src/CMakeLists.txt"
    [ -d "$dst/include" ] || die "emote component not found at $dst"
    log "copying patched emote runtime → upstream tree"
    cp -f "$src/emote.c" "$dst/emote.c"
    cp -f "$src/emote.h" "$dst/include/emote.h"
    cp -f "$src/CMakeLists.txt" "$dst/CMakeLists.txt"
}

copy_http_display_diagnostics() {
    # Runtime display/screenshot diagnostics for the live-device harness. The
    # endpoint source is vendored because esp-claw/ is a gitignored generated
    # checkout and clean bootstrap must reproduce the flashed build.
    local src="$ROOT/firmware/http_server/http_server_display_api.c"
    local touch_src="$ROOT/firmware/http_server/http_server_touch_api.c"
    local audio_src="$ROOT/firmware/http_server/http_server_audio_level_api.c"
    local dst_dir="$ESP_CLAW_DIR/application/edge_agent/components/http_server"
    local cmake="$dst_dir/CMakeLists.txt"
    local priv="$dst_dir/http_server_priv.h"
    local core="$dst_dir/http_server_core.c"
    local public_h="$dst_dir/include/http_server.h"
    [ -f "$src" ] || die "missing vendored display HTTP API at $src"
    [ -f "$touch_src" ] || die "missing vendored touch HTTP API at $touch_src"
    [ -f "$audio_src" ] || die "missing vendored audio-level HTTP API at $audio_src"
    [ -d "$dst_dir" ] || die "http_server component not found at $dst_dir"
    log "copying display/touch/audio diagnostics HTTP API → upstream tree"
    cp -f "$src" "$dst_dir/http_server_display_api.c"
    cp -f "$touch_src" "$dst_dir/http_server_touch_api.c"
    cp -f "$audio_src" "$dst_dir/http_server_audio_level_api.c"
    python3 - "$cmake" "$priv" "$core" "$public_h" <<'PY'
import pathlib
import sys

cmake = pathlib.Path(sys.argv[1])
priv = pathlib.Path(sys.argv[2])
core = pathlib.Path(sys.argv[3])
public_h = pathlib.Path(sys.argv[4])

s = cmake.read_text()
if '"http_server_display_api.c"' not in s:
    anchor = '        "http_server_camera_api.c"\n'
    if anchor not in s:
        raise SystemExit("http_server CMake source anchor missing")
    s = s.replace(anchor, anchor + '        "http_server_display_api.c"\n', 1)
if '"http_server_touch_api.c"' not in s:
    anchor = '        "http_server_display_api.c"\n'
    if anchor not in s:
        raise SystemExit("http_server CMake touch source anchor missing")
    s = s.replace(anchor, anchor + '        "http_server_touch_api.c"\n', 1)
for dep in ("heap", "emote"):
    if f"        {dep}\n" not in s:
        anchor = "        esp_timer\n"
        if anchor not in s:
            raise SystemExit("http_server CMake REQUIRES anchor missing")
        s = s.replace(anchor, anchor + f"        {dep}\n", 1)
cmake.write_text(s)

h = priv.read_text()
decl = "esp_err_t http_server_register_display_routes(httpd_handle_t server);\n"
if decl not in h:
    anchor = "esp_err_t http_server_register_camera_routes(httpd_handle_t server);\n"
    if anchor not in h:
        raise SystemExit("http_server_priv display decl anchor missing")
    h = h.replace(anchor, anchor + decl, 1)
touch_decl = "esp_err_t http_server_register_touch_routes(httpd_handle_t server);\n"
if touch_decl not in h:
    anchor = "esp_err_t http_server_register_display_routes(httpd_handle_t server);\n"
    if anchor not in h:
        raise SystemExit("http_server_priv touch decl anchor missing")
    h = h.replace(anchor, anchor + touch_decl, 1)
priv.write_text(h)

c = core.read_text()
c = c.replace("    config.max_uri_handlers = 32;\n", "    config.max_uri_handlers = 44;\n")
c = c.replace("    config.max_uri_handlers = 40;\n", "    config.max_uri_handlers = 44;\n")
if "http_server_register_display_routes" not in c:
    anchor = '    ESP_RETURN_ON_ERROR(http_server_register_camera_routes(s_ctx.server), TAG, "Failed to register camera routes");\n'
    if anchor not in c:
        raise SystemExit("http_server_core display route anchor missing")
    c = c.replace(anchor, anchor + '    ESP_RETURN_ON_ERROR(http_server_register_display_routes(s_ctx.server), TAG, "Failed to register display routes");\n', 1)
if "http_server_register_touch_routes" not in c:
    anchor = '    ESP_RETURN_ON_ERROR(http_server_register_display_routes(s_ctx.server), TAG, "Failed to register display routes");\n'
    if anchor not in c:
        raise SystemExit("http_server_core touch route anchor missing")
    c = c.replace(anchor, anchor + '    ESP_RETURN_ON_ERROR(http_server_register_touch_routes(s_ctx.server), TAG, "Failed to register touch routes");\n', 1)
core.write_text(c)

hp = public_h.read_text()
svc_fields = (
    "    esp_err_t (*touch_get_diagnostics)(char *out, size_t out_size);\n"
    "    esp_err_t (*touch_run_scene)(const char *scene_name);\n"
)
if "touch_get_diagnostics" not in hp:
    anchor = "    bool (*gemini_live_is_active)(void);\n"
    if anchor not in hp:
        raise SystemExit("http_server services touch anchor missing")
    hp = hp.replace(anchor, anchor + svc_fields, 1)
public_h.write_text(hp)

print("display/touch diagnostics HTTP API wired")
PY
}

apply_patch() {
    local target="$ESP_CLAW_DIR/application/edge_agent/managed_components/espressif__esp_board_manager/peripherals/periph_i2s/periph_i2s.py"
    if [ ! -f "$target" ]; then
        log "esp_board_manager not pulled yet — running idf.py reconfigure inside Docker to populate managed_components"
        docker run --rm -v "$ESP_CLAW_DIR":/project -w /project/application/edge_agent "$IDF_IMAGE" \
            bash -lc 'pip install --quiet "esp-bmgr-assist==0.5.0" && idf.py set-target esp32s3 && idf.py reconfigure' \
            > "$ROOT/.build_logs/reconfigure.log" 2>&1 ||
            die "idf.py reconfigure failed; see $ROOT/.build_logs/reconfigure.log"
        [ -f "$target" ] || die "esp_board_manager component still missing after reconfigure: $target"
    fi
    if grep -q "chip_supports_pdm_rx_hp_filter" "$target" 2>/dev/null; then
        log "codegen patch already applied"
        return
    fi
    log "applying patches/0001-fix-pdm-rx-hp-filter-cap.patch"
    python3 - <<PY
import re, pathlib
p = pathlib.Path(r"$target")
s = p.read_text()
old = (
    "    # Add hardware version specific fields\n"
    "    if hw_version == 2:  # SOC_I2S_HW_VERSION_2 (all other chips) - only these chips support HP filter\n"
    "        slot_cfg['hp_en'] = bool(cfg.get('hp_en', True))\n"
    "        # Validate HP filter cut-off frequency (23.3Hz ~ 185Hz)\n"
    "        hp_freq = float(cfg.get('hp_cut_off_freq_hz', 35.5))\n"
)
new = (
    "    # PDM RX HP filter fields are gated by SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER,\n"
    "    # which is only set on ESP32-P4 (and other future chips). HW_VERSION_2\n"
    "    # alone is not enough — ESP32-S3 is HW_VERSION_2 but lacks the cap.\n"
    "    chip_supports_pdm_rx_hp_filter = get_effective_chip_name() in ('esp32p4',)\n"
    "    if hw_version == 2 and chip_supports_pdm_rx_hp_filter:\n"
    "        slot_cfg['hp_en'] = bool(cfg.get('hp_en', True))\n"
    "        hp_freq = float(cfg.get('hp_cut_off_freq_hz', 35.5))\n"
)
if old not in s:
    raise SystemExit(f"could not locate target hunk in {p} — upstream may have changed")
p.write_text(s.replace(old, new))
print("patched", p)
PY
}

build() {
    mkdir -p "$ROOT/.build_logs"
    log "building $BOARD_VENDOR/$BOARD_NAME inside $IDF_IMAGE (output streams to .build_logs/build.log)"
    docker run --rm \
        -e BOARD_NAME="$BOARD_NAME" \
        -v "$ESP_CLAW_DIR":/project \
        -w /project/application/edge_agent \
        "$IDF_IMAGE" \
        bash -lc 'set -e; pip install --quiet "idf-component-manager==2.4.10" "esp-bmgr-assist==0.5.0";
                  idf.py set-target esp32s3;
                  idf.py gen-bmgr-config -c ./boards -b "$BOARD_NAME";
                  python3 - <<'"'"'PY'"'"'
import os
from pathlib import Path

board = os.environ["BOARD_NAME"]

cfg = Path("sdkconfig")
bm = Path("components/gen_bmgr_codes/board_manager.defaults")

def force_perf_build(text: str) -> str:
    # Keep the device build out of -O0. The AMOLED renderer and audio path are
    # too timing-sensitive for debug optimization; the symptoms look like bad UX,
    # because technically they are.
    replacements = {
        "CONFIG_COMPILER_OPTIMIZATION_DEBUG=y": "# CONFIG_COMPILER_OPTIMIZATION_DEBUG is not set",
        "# CONFIG_COMPILER_OPTIMIZATION_PERF is not set": "CONFIG_COMPILER_OPTIMIZATION_PERF=y",
        "CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y": "# CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG is not set",
        "CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y": "# CONFIG_COMPILER_OPTIMIZATION_DEFAULT is not set",
        "# CONFIG_COMPILER_OPTIMIZATION_LEVEL_RELEASE is not set": "CONFIG_COMPILER_OPTIMIZATION_LEVEL_RELEASE=y",
    }
    for old, new in replacements.items():
        text = text.replace(old, new)
    if "CONFIG_COMPILER_OPTIMIZATION_PERF=y" not in text:
        text += "\nCONFIG_COMPILER_OPTIMIZATION_PERF=y\n"
    if "CONFIG_COMPILER_OPTIMIZATION_LEVEL_RELEASE=y" not in text:
        text += "CONFIG_COMPILER_OPTIMIZATION_LEVEL_RELEASE=y\n"
    return text

if cfg.exists():
    s = cfg.read_text()
else:
    base = Path("sdkconfig.defaults")
    chunks = []
    if base.exists():
        chunks.append(base.read_text())
    if bm.exists():
        chunks.append("\n# board defaults synthesized by esp_board_manager\n")
        chunks.append(bm.read_text())
    if not chunks:
        chunks.append("")
    s = "".join(chunks)

if board == "xiao_esp32s3_sense":
    s = s.replace("CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y", "# CONFIG_ESPTOOLPY_FLASHSIZE_2MB is not set")
    s = s.replace("# CONFIG_ESPTOOLPY_FLASHSIZE_8MB is not set", "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y")
    s = s.replace("CONFIG_ESPTOOLPY_FLASHSIZE=\"2MB\"", "CONFIG_ESPTOOLPY_FLASHSIZE=\"8MB\"")
    # The XIAO ESP32-S3 Sense has enough flash for the app image, but not enough
    # free internal heap to register the full serial REPL after Wi-Fi, MCP, Lua,
    # router, scheduler, and camera startup. Keep USB logs, skip the interactive CLI.
    s = s.replace("CONFIG_APP_CLAW_ENABLE_CLI=y", "# CONFIG_APP_CLAW_ENABLE_CLI is not set")
elif board == "esp32s3_touch_amoled_1_75":
    s = s.replace("CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y", "# CONFIG_ESPTOOLPY_FLASHSIZE_2MB is not set")
    s = s.replace("# CONFIG_ESPTOOLPY_FLASHSIZE_16MB is not set", "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y")
    s = s.replace("CONFIG_ESPTOOLPY_FLASHSIZE=\"2MB\"", "CONFIG_ESPTOOLPY_FLASHSIZE=\"16MB\"")

# LVGL is pulled in as a managed dependency, but its bundled examples/demos are
# never compiled into the app and fail a clean (fullclean) build with
# "sdkconfig.h: No such file or directory". Turn them off — strictly a build-time
# saving, no app code references LV_USE_*_EXAMPLE.
s = s.replace("CONFIG_LV_BUILD_EXAMPLES=y", "# CONFIG_LV_BUILD_EXAMPLES is not set")
s = s.replace("CONFIG_LV_BUILD_DEMOS=y", "# CONFIG_LV_BUILD_DEMOS is not set")
if "CONFIG_LV_BUILD_EXAMPLES=y" not in s and "# CONFIG_LV_BUILD_EXAMPLES is not set" not in s:
    s += "\n# CONFIG_LV_BUILD_EXAMPLES is not set\n"
if "CONFIG_LV_BUILD_DEMOS=y" not in s and "# CONFIG_LV_BUILD_DEMOS is not set" not in s:
    s += "# CONFIG_LV_BUILD_DEMOS is not set\n"

s = force_perf_build(s)
cfg.write_text(s)
PY
                  { idf.py build || { echo "[bootstrap] first idf.py build failed — retrying once incrementally (sdkconfig.h generation can lose a race with the first compile after set-target fullclean)"; idf.py build; }; }' \
        | tee "$ROOT/.build_logs/build.log"
    log "✓ build complete → $ESP_CLAW_DIR/application/edge_agent/build/edge_agent.bin"
}

apply_wifi_ps_patch() {
    local target="$ESP_CLAW_DIR/components/common/wifi_manager/wifi_manager.c"
    if [ ! -f "$target" ]; then
        log "wifi_manager.c not found at $target — skipping wifi PS patch"
        return
    fi
    if grep -q "provisioning AP stopped for LAN reachability" "$target" 2>/dev/null; then
        log "wifi PS patch already applied"
        return
    fi
    log "applying patches/0002-wifi-disable-modem-sleep.patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$target")
s = p.read_text()
marker = "        (void)esp_wifi_set_ps(WIFI_PS_NONE);\n"
if "Disable Wi-Fi modem-sleep AFTER association" in s and "provisioning AP stopped for LAN reachability" not in s:
    new = (
        marker +
        "        if (s_ap_active) {\n"
        "            esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);\n"
        "            if (mode_err == ESP_OK) {\n"
        "                ESP_LOGI(TAG, \"STA connected; provisioning AP stopped for LAN reachability\");\n"
        "            } else {\n"
        "                ESP_LOGW(TAG, \"Failed to stop provisioning AP after STA connect: %s\",\n"
        "                         esp_err_to_name(mode_err));\n"
        "            }\n"
        "        }\n"
    )
    if marker not in s:
        raise SystemExit(f"could not locate existing wifi PS marker in {p}")
    p.write_text(s.replace(marker, new, 1))
    print("patched", p)
    raise SystemExit(0)
old = (
    "        s_mode = s_ap_active ? WIFI_MODE_APSTA_OK : s_mode;\n"
    "        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);\n"
)
new = (
    "        s_mode = s_ap_active ? WIFI_MODE_APSTA_OK : s_mode;\n"
    "        // Disable Wi-Fi modem-sleep AFTER association — must run here\n"
    "        // (not at esp_wifi_start) because the new association resets\n"
    "        // PS to default MIN_MODEM, which delays outbound TCP packets\n"
    "        // by hundreds of ms and makes /api/camera/snapshot (~50 KB\n"
    "        // JPEG) un-deliverable to a browser client. Trades ~30 mA\n"
    "        // idle current for reliable HTTP throughput.\n"
    "        (void)esp_wifi_set_ps(WIFI_PS_NONE);\n"
    "        if (s_ap_active) {\n"
    "            esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);\n"
    "            if (mode_err == ESP_OK) {\n"
    "                ESP_LOGI(TAG, \"STA connected; provisioning AP stopped for LAN reachability\");\n"
    "            } else {\n"
    "                ESP_LOGW(TAG, \"Failed to stop provisioning AP after STA connect: %s\",\n"
    "                         esp_err_to_name(mode_err));\n"
    "            }\n"
    "        }\n"
    "        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);\n"
)
if old not in s:
    raise SystemExit(f"could not locate target hunk in {p} — upstream may have changed")
p.write_text(s.replace(old, new))
print("patched", p)
PY
}

apply_jpeg_soi_patch() {
    local target="$ESP_CLAW_DIR/application/edge_agent/managed_components/espressif__esp_cam_sensor/src/driver_dvp/esp_cam_ctlr_dvp_cam.c"
    if [ ! -f "$target" ]; then
        log "esp_cam_sensor not pulled yet — skipping JPEG SOI patch (will apply on next build)"
        return
    fi
    if grep -q "OV3660 (and likely other newer sensors) sometimes prepend" "$target" 2>/dev/null; then
        log "JPEG SOI patch already applied"
        return
    fi
    log "applying patches/0003-dvp-cam-scan-for-jpeg-soi.patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$target")
s = p.read_text()
old = (
    "static uint32_t dvp_calculate_jpeg_size(const uint8_t *buffer, uint32_t size)\n"
    "{\n"
    "    if (size < 16) {\n"
    "        DVP_CAM_ERROR(\"JPEG size\");\n"
    "        return 0;\n"
    "    }\n"
    "\n"
    "    /* Check JPEG header TAG: ff:d8 */\n"
    "\n"
    "    if (buffer[0] != 0xff || buffer[1] != 0xd8) {\n"
    "        DVP_CAM_ERROR(\"NO-SOI\");\n"
    "        return 0;\n"
    "    }\n"
)
new = (
    "static uint32_t dvp_calculate_jpeg_size(uint8_t *buffer, uint32_t size)\n"
    "{\n"
    "    if (size < 16) {\n"
    "        DVP_CAM_ERROR(\"JPEG size\");\n"
    "        return 0;\n"
    "    }\n"
    "\n"
    "    /* OV3660 (and likely other newer sensors) sometimes prepend sync /\n"
    "     * padding bytes before the JPEG SOI (0xFF 0xD8). Scan up to the\n"
    "     * first 64 bytes for the marker and shift the buffer to start at\n"
    "     * it. Without this, every frame is rejected NO-SOI on the XIAO\n"
    "     * ESP32-S3 Sense + OV3660 batch shipped in 2026. */\n"
    "\n"
    "    uint32_t prefix = 0;\n"
    "    if (buffer[0] != 0xff || buffer[1] != 0xd8) {\n"
    "        const uint32_t max_scan = (size > 64) ? 64 : (size - 1);\n"
    "        for (prefix = 1; prefix < max_scan; prefix++) {\n"
    "            if (buffer[prefix] == 0xff && buffer[prefix + 1] == 0xd8) {\n"
    "                memmove(buffer, buffer + prefix, size - prefix);\n"
    "                size -= prefix;\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "        if (buffer[0] != 0xff || buffer[1] != 0xd8) {\n"
    "            DVP_CAM_ERROR(\"NO-SOI\");\n"
    "            return 0;\n"
    "        }\n"
    "    }\n"
)
if old not in s:
    raise SystemExit(f"could not locate target hunk in {p} — upstream may have changed")
p.write_text(s.replace(old, new))
print("patched", p)
PY
}

apply_http_phase2_patch() {
    local core="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_core.c"
    local status="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_status_api.c"
    local json="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_json.c"
    if [ ! -f "$core" ] || [ ! -f "$status" ] || [ ! -f "$json" ]; then
        log "http_server sources not found — skipping Phase 2 HTTP patch"
        return
    fi
    if grep -q "Failed to register API OPTIONS route" "$core" 2>/dev/null; then
        python3 - <<PY
import pathlib

core = pathlib.Path(r"$core")
s = core.read_text()
block = (
    "    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_ctx.server, &(httpd_uri_t) {\\n"
    "                            .uri = \"/api/*\",\\n"
    "                            .method = HTTP_OPTIONS,\\n"
    "                            .handler = api_options_handler,\\n"
    "                        }), TAG, \"Failed to register API OPTIONS route\");\\n"
)
old = (
    "    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, \"Failed to start HTTP server\");\\n"
    + block
    + "    ESP_RETURN_ON_ERROR(http_server_register_assets_routes(s_ctx.server), TAG, \"Failed to register assets routes\");\\n"
)
new = (
    "    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, \"Failed to start HTTP server\");\\n"
    "    ESP_RETURN_ON_ERROR(http_server_register_assets_routes(s_ctx.server), TAG, \"Failed to register assets routes\");\\n"
)
if old in s:
    s = s.replace(old, new, 1)
    anchor = (
        "    ESP_RETURN_ON_ERROR(http_server_register_camera_routes(s_ctx.server), TAG, \"Failed to register camera routes\");\\n"
        "    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_ctx.server, HTTPD_404_NOT_FOUND, http_server_captive_404_handler),\\n"
    )
    if anchor not in s:
        raise SystemExit(f"could not locate API OPTIONS route reorder point in {core}")
    s = s.replace(anchor, "    ESP_RETURN_ON_ERROR(http_server_register_camera_routes(s_ctx.server), TAG, \"Failed to register camera routes\");\\n" + block + "    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_ctx.server, HTTPD_404_NOT_FOUND, http_server_captive_404_handler),\\n", 1)
    core.write_text(s)
PY
    fi
    if grep -q "Failed to register API OPTIONS route" "$core" 2>/dev/null &&
       ! grep -q "allow JSON clients to POST /api/\\*" "$core" 2>/dev/null &&
       grep -q "lru_purge_enable" "$core" 2>/dev/null &&
       grep -q 'Connection", "close"' "$json" 2>/dev/null &&
       grep -q '"not_wired"' "$status" 2>/dev/null; then
        log "Phase 2 HTTP patch already applied"
        return
    fi
    log "applying patches/0004-http-phase2-preflight-battery.patch"
    python3 - <<PY
import pathlib

core = pathlib.Path(r"$core")
status = pathlib.Path(r"$status")
json = pathlib.Path(r"$json")

s = core.read_text()
s = s.replace(
    "    /* Phase 2 browser preflight: allow JSON clients to POST /api/*\n"
    "     * without the dashboard's text/plain workaround. */\n",
    "    /* Phase 2 browser preflight for JSON API clients. */\n",
)
old = (
    "static http_server_ctx_t s_ctx;\n"
    "\n"
    "http_server_ctx_t *http_server_ctx(void)\n"
)
new = (
    "static http_server_ctx_t s_ctx;\n"
    "\n"
    "static esp_err_t api_options_handler(httpd_req_t *req)\n"
    "{\n"
    "    /* Phase 2 browser preflight for JSON API clients. */\n"
    "    httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n"
    "    httpd_resp_set_hdr(req, \"Access-Control-Allow-Methods\", \"GET, POST, DELETE, OPTIONS\");\n"
    "    httpd_resp_set_hdr(req, \"Access-Control-Allow-Headers\", \"Content-Type, X-JarvisNano-Protocol\");\n"
    "    httpd_resp_set_hdr(req, \"Access-Control-Max-Age\", \"86400\");\n"
    "    httpd_resp_set_status(req, \"204 No Content\");\n"
    "    return httpd_resp_send(req, NULL, 0);\n"
    "}\n"
    "\n"
    "http_server_ctx_t *http_server_ctx(void)\n"
)
if "Phase 2 browser preflight" not in s:
    if old not in s:
        raise SystemExit(f"could not locate core insertion point in {core}")
    s = s.replace(old, new, 1)

old = (
    "    config.max_uri_handlers = 32;\n"
    "    config.stack_size = 8192;\n"
)
new = (
    "    config.max_uri_handlers = 32;\n"
    "    config.max_open_sockets = 13;\n"
    "    config.backlog_conn = 8;\n"
    "    config.lru_purge_enable = true;\n"
    "    config.recv_wait_timeout = 5;\n"
    "    config.send_wait_timeout = 5;\n"
    "    config.stack_size = 8192;\n"
)
if "lru_purge_enable" not in s:
    if old not in s:
        raise SystemExit(f"could not locate HTTP server config tuning point in {core}")
    s = s.replace(old, new, 1)

old = (
    "    ESP_RETURN_ON_ERROR(http_server_register_camera_routes(s_ctx.server), TAG, \"Failed to register camera routes\");\n"
    "    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_ctx.server, HTTPD_404_NOT_FOUND, http_server_captive_404_handler),\n"
)
new = (
    "    ESP_RETURN_ON_ERROR(http_server_register_camera_routes(s_ctx.server), TAG, \"Failed to register camera routes\");\n"
    "    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_ctx.server, &(httpd_uri_t) {\n"
    "                            .uri = \"/api/*\",\n"
    "                            .method = HTTP_OPTIONS,\n"
    "                            .handler = api_options_handler,\n"
    "                        }), TAG, \"Failed to register API OPTIONS route\");\n"
    "    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_ctx.server, HTTPD_404_NOT_FOUND, http_server_captive_404_handler),\n"
)
if "Failed to register API OPTIONS route" not in s:
    if old not in s:
        raise SystemExit(f"could not locate core route registration point in {core}")
    s = s.replace(old, new, 1)
core.write_text(s)

s = json.read_text()
if 'httpd_resp_set_hdr(req, "Connection", "close");' not in s:
    s = s.replace(
        '    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");\n'
        '    return httpd_resp_send(req, (const char *)start, content_len);\n',
        '    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");\n'
        '    httpd_resp_set_hdr(req, "Connection", "close");\n'
        '    return httpd_resp_send(req, (const char *)start, content_len);\n',
        1,
    )
    s = s.replace(
        '    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");\n'
        '    esp_err_t err = httpd_resp_sendstr(req, payload);\n',
        '    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");\n'
        '    httpd_resp_set_hdr(req, "Connection", "close");\n'
        '    esp_err_t err = httpd_resp_sendstr(req, payload);\n',
        1,
    )
    json.write_text(s)

s = status.read_text()
old = (
    "static esp_err_t restart_handler(httpd_req_t *req)\n"
    "{\n"
)
new = (
    "static esp_err_t battery_handler(httpd_req_t *req)\n"
    "{\n"
    "    cJSON *root = cJSON_CreateObject();\n"
    "    if (!root) {\n"
    "        httpd_resp_send_500(req);\n"
    "        return ESP_ERR_NO_MEM;\n"
    "    }\n"
    "\n"
    "    cJSON_AddBoolToObject(root, \"wired\", false);\n"
    "    cJSON_AddNumberToObject(root, \"mV\", 0);\n"
    "    cJSON_AddNumberToObject(root, \"pct\", 0);\n"
    "    http_server_json_add_string(root, \"state\", \"not_wired\");\n"
    "    http_server_json_add_string(root, \"source\", \"stub\");\n"
    "    return http_server_send_json_response(req, root);\n"
    "}\n"
    "\n"
    "static esp_err_t restart_handler(httpd_req_t *req)\n"
    "{\n"
)
if '"not_wired"' not in s:
    if old not in s:
        raise SystemExit(f"could not locate battery insertion point in {status}")
    s = s.replace(old, new, 1)

old = (
    "        { .uri = \"/api/status\", .method = HTTP_GET, .handler = status_handler },\n"
    "        { .uri = \"/api/restart\", .method = HTTP_POST, .handler = restart_handler },\n"
)
new = (
    "        { .uri = \"/api/status\", .method = HTTP_GET, .handler = status_handler },\n"
    "        { .uri = \"/api/battery\", .method = HTTP_GET, .handler = battery_handler },\n"
    "        { .uri = \"/api/restart\", .method = HTTP_POST, .handler = restart_handler },\n"
)
if "/api/battery" not in s:
    if old not in s:
        raise SystemExit(f"could not locate status route table in {status}")
    s = s.replace(old, new, 1)
status.write_text(s)
print("patched", core)
print("patched", status)
PY
}

# Point /api/battery at a real AXP2101 fuel-gauge read via the jarvis_pmic
# component. Runs AFTER apply_http_phase2_patch (which creates the battery_handler)
# and AFTER copy_http_display_diagnostics (so the http_server REQUIRES list is
# settled). Fully idempotent and self-cleaning: it strips ANY in-tree axp2101_*
# helper functions (including non-durable hand-edits committed straight into the
# esp-claw snapshot) and rewrites battery_handler to call jarvis_pmic. The PMIC
# stays init_skip:true — jarvis_pmic only reads, never re-sequences rails.
apply_battery_real_read_patch() {
    local status="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_status_api.c"
    local cmake="$ESP_CLAW_DIR/application/edge_agent/components/http_server/CMakeLists.txt"
    if [ ! -f "$status" ] || [ ! -f "$cmake" ]; then
        log "http_server sources not found — skipping AXP2101 battery patch"
        return
    fi
    log "asserting AXP2101 real battery read (jarvis_pmic → /api/battery)"
    python3 - <<PY
import pathlib
import re

status = pathlib.Path(r"$status")
cmake = pathlib.Path(r"$cmake")

s = status.read_text()
orig = s

# 1. Pull in the jarvis_pmic header (idempotent).
inc_anchor = '#include "http_server_priv.h"\n'
if '#include "jarvis_pmic.h"' not in s:
    if inc_anchor not in s:
        raise SystemExit("could not locate http_server_priv.h include in status_api.c")
    s = s.replace(inc_anchor, inc_anchor + '#include "jarvis_pmic.h"\n', 1)

# 2. Strip EVERY in-tree axp2101_* helper function (and an optional doc comment
#    directly above it). Older snapshots hand-edited a raw reader straight into
#    this upstream file; the canonical read now lives in the jarvis_pmic
#    component, so these become dead code. No-op when none are present.
helper_re = re.compile(
    r"(?:/\*.*?\*/\n)?"
    r"static [^\n]*\baxp2101_\w+\([^)]*\)\n\{\n.*?\n\}\n\n?",
    re.DOTALL,
)
s = helper_re.sub("", s)

# 3. Rewrite battery_handler (whatever its current body) to call jarvis_pmic.
#    The function ends at the first column-0 '}' — inner braces are indented.
real = (
    "static esp_err_t battery_handler(httpd_req_t *req)\n"
    "{\n"
    "    cJSON *root = cJSON_CreateObject();\n"
    "    if (!root) {\n"
    "        httpd_resp_send_500(req);\n"
    "        return ESP_ERR_NO_MEM;\n"
    "    }\n"
    "\n"
    "    jarvis_battery_t bat;\n"
    "    if (jarvis_pmic_read_battery(&bat) != ESP_OK) {\n"
    "        /* PMIC not reachable on the shared I2C bus yet — stay honest. */\n"
    "        cJSON_AddBoolToObject(root, \"wired\", false);\n"
    "        cJSON_AddNumberToObject(root, \"mV\", 0);\n"
    "        cJSON_AddNumberToObject(root, \"pct\", 0);\n"
    "        http_server_json_add_string(root, \"state\", \"not_wired\");\n"
    "        http_server_json_add_string(root, \"source\", \"axp2101\");\n"
    "        return http_server_send_json_response(req, root);\n"
    "    }\n"
    "\n"
    "    const char *state = bat.charging        ? \"charging\"\n"
    "                      : !bat.present          ? \"no_battery\"\n"
    "                      : (bat.percent != 0xFF && bat.percent <= 15) ? \"low\"\n"
    "                      :                         \"discharging\";\n"
    "\n"
    "    cJSON_AddBoolToObject(root, \"wired\", bat.present);\n"
    "    cJSON_AddNumberToObject(root, \"mV\", bat.millivolts);\n"
    "    cJSON_AddNumberToObject(root, \"pct\", bat.percent == 0xFF ? 0 : bat.percent);\n"
    "    cJSON_AddBoolToObject(root, \"charging\", bat.charging);\n"
    "    cJSON_AddBoolToObject(root, \"usb\", bat.usb_present);\n"
    "    http_server_json_add_string(root, \"state\", state);\n"
    "    http_server_json_add_string(root, \"source\", \"axp2101\");\n"
    "    return http_server_send_json_response(req, root);\n"
    "}\n"
)
handler_re = re.compile(
    r"static esp_err_t battery_handler\(httpd_req_t \*req\)\n\{\n.*?\n\}\n",
    re.DOTALL,
)
if not handler_re.search(s):
    raise SystemExit("could not locate battery_handler — apply_http_phase2_patch must run first")
s = handler_re.sub(lambda _m: real, s, count=1)

if s != orig:
    status.write_text(s)
    print("patched", status)
else:
    print("battery handler already current")

# 4. Add jarvis_pmic to the http_server component REQUIRES (idempotent).
c = cmake.read_text()
if "jarvis_pmic" not in c:
    anchor = "        json\n"
    if anchor not in c:
        raise SystemExit("could not locate http_server REQUIRES json anchor")
    c = c.replace(anchor, anchor + "        jarvis_pmic\n", 1)
    cmake.write_text(c)
    print("patched", cmake)
PY
}

# Add a read-only /api/imu endpoint backed by the jarvis_imu component (QMI8658
# accelerometer). Runs AFTER apply_battery_real_read_patch (shares the status API
# file + REQUIRES idiom) and copy_jarvis_imu. The IMU is read on demand only — no
# always-on poller is added (the I2C bus is shared). See firmware/components/jarvis_imu.
apply_imu_read_patch() {
    local status="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_status_api.c"
    local cmake="$ESP_CLAW_DIR/application/edge_agent/components/http_server/CMakeLists.txt"
    if [ ! -f "$status" ] || [ ! -f "$cmake" ]; then
        log "http status route sources not found — skipping IMU read patch"
        return
    fi
    if grep -q "jarvis_imu_read" "$status" 2>/dev/null; then
        log "QMI8658 IMU read patch already applied"
        return
    fi
    log "applying QMI8658 IMU read (jarvis_imu → /api/imu)"
    python3 - <<PY
import pathlib
status = pathlib.Path(r"$status")
cmake = pathlib.Path(r"$cmake")

s = status.read_text()

# 1. Pull in the jarvis_imu header (after jarvis_pmic.h if present, else priv).
if '#include "jarvis_imu.h"' not in s:
    for anchor in ('#include "jarvis_pmic.h"\n', '#include "http_server_priv.h"\n'):
        if anchor in s:
            s = s.replace(anchor, anchor + '#include "jarvis_imu.h"\n', 1)
            break
    else:
        raise SystemExit("could not locate status API include anchor in %s" % status)

# 2. Insert imu_handler just before restart_handler.
end_marker = "static esp_err_t restart_handler(httpd_req_t *req)\n"
idx = s.find(end_marker)
if idx == -1:
    raise SystemExit("could not locate restart_handler in %s" % status)

handler = '''static esp_err_t imu_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    jarvis_imu_t imu;
    if (jarvis_imu_read(&imu) != ESP_OK || !imu.present) {
        cJSON_AddBoolToObject(root, "present", false);
        http_server_json_add_string(root, "source", "qmi8658");
        return http_server_send_json_response(req, root);
    }

    cJSON_AddBoolToObject(root, "present", true);
    cJSON_AddNumberToObject(root, "addr", imu.i2c_addr);
    cJSON_AddNumberToObject(root, "ax", imu.ax);
    cJSON_AddNumberToObject(root, "ay", imu.ay);
    cJSON_AddNumberToObject(root, "az", imu.az);
    cJSON_AddNumberToObject(root, "gx", imu.gx);
    cJSON_AddNumberToObject(root, "gy", imu.gy);
    cJSON_AddNumberToObject(root, "gz", imu.gz);
    cJSON_AddNumberToObject(root, "pitch", imu.pitch_deg);
    cJSON_AddNumberToObject(root, "roll", imu.roll_deg);
    cJSON_AddNumberToObject(root, "motion_mg", imu.motion_mg);
    cJSON_AddBoolToObject(root, "moving", imu.moving);
    cJSON_AddBoolToObject(root, "shake", imu.shake);
    http_server_json_add_string(root, "orientation", imu.orientation);
    http_server_json_add_string(root, "source", "qmi8658");
    return http_server_send_json_response(req, root);
}

'''
if "imu_handler" not in s:
    s = s[:idx] + handler + s[idx:]

# 3. Register the /api/imu route after /api/battery.
route_anchor = '        { .uri = "/api/battery", .method = HTTP_GET, .handler = battery_handler },\n'
if '"/api/imu"' not in s:
    if route_anchor not in s:
        raise SystemExit("could not locate /api/battery route in %s" % status)
    s = s.replace(route_anchor,
                  route_anchor + '        { .uri = "/api/imu", .method = HTTP_GET, .handler = imu_handler },\n',
                  1)
status.write_text(s)

# 4. Add jarvis_imu to the http_server component REQUIRES (after jarvis_pmic).
c = cmake.read_text()
if "jarvis_imu" not in c:
    for anchor in ("        jarvis_pmic\n", "        json\n"):
        if anchor in c:
            c = c.replace(anchor, anchor + "        jarvis_imu\n", 1)
            break
    else:
        raise SystemExit("could not locate http_server REQUIRES anchor in %s" % cmake)
    cmake.write_text(c)

print("patched", status)
print("patched", cmake)
PY
}

apply_http_camera_gate_patch() {
    local cmake="$ESP_CLAW_DIR/application/edge_agent/components/http_server/CMakeLists.txt"
    local camera_api="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_camera_api.c"
    if [ ! -f "$cmake" ] || [ ! -f "$camera_api" ]; then
        log "http camera route sources not found — skipping camera gate patch"
        return
    fi
    if grep -q "CONFIG_APP_CLAW_LUA_MODULE_CAMERA" "$cmake" 2>/dev/null &&
       grep -q "CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_CAMERA" "$camera_api" 2>/dev/null &&
       grep -q "camera_unavailable_get_handler" "$camera_api" 2>/dev/null; then
        log "camera route dependency gate patch already applied"
        return
    fi
    log "applying patches/0005-http-camera-gate-lua-module.patch"
    python3 - <<PY
import pathlib

cmake = pathlib.Path(r"$cmake")
camera_api = pathlib.Path(r"$camera_api")

s = cmake.read_text()
s = s.replace(
    "if(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)\n"
    "    list(APPEND http_server_extra_requires lua_module_camera)\n"
    "endif()\n",
    "if(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT AND CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_CAMERA)\n"
    "    list(APPEND http_server_extra_requires lua_module_camera)\n"
    "endif()\n",
    1,
)
s = s.replace(
    "if(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)\n"
    "    target_include_directories(\${COMPONENT_LIB} PRIVATE\n"
    "        \"\${CMAKE_CURRENT_LIST_DIR}/../../../../components/lua_modules/lua_module_camera/src\")\n"
    "endif()\n",
    "if(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT AND CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_CAMERA)\n"
    "    target_include_directories(\${COMPONENT_LIB} PRIVATE\n"
    "        \"\${CMAKE_CURRENT_LIST_DIR}/../../../../components/lua_modules/lua_module_camera/src\")\n"
    "endif()\n",
    1,
)
cmake.write_text(s)

s = camera_api.read_text()
s = s.replace(
    "#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT\n",
    "#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT && CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_CAMERA\n",
    1,
)
s = s.replace(
    "#else  /* !CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT */\n",
    "#else  /* camera service unavailable */\n",
    1,
)
s = s.replace(
    "#endif /* CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT */\n",
    "#endif /* camera service gate */\n",
    1,
)
s = s.replace(
    "#include \"esp_log.h\"\n\n"
    "esp_err_t http_server_register_camera_routes(httpd_handle_t server)\n"
    "{\n"
    "    (void)server;\n"
    "    /* No camera service in this build; skip route registration. */\n"
    "    return ESP_OK;\n"
    "}\n",
    "static esp_err_t camera_unavailable_get_handler(httpd_req_t *req)\n"
    "{\n"
    "    cJSON *root = cJSON_CreateObject();\n"
    "    if (!root) {\n"
    "        httpd_resp_send_500(req);\n"
    "        return ESP_ERR_NO_MEM;\n"
    "    }\n"
    "    cJSON_AddBoolToObject(root, \"available\", false);\n"
    "    http_server_json_add_string(root, \"state\", \"not_available\");\n"
    "    http_server_json_add_string(root, \"reason\", \"camera service is not enabled in this build\");\n"
    "    httpd_resp_set_status(req, \"503 Service Unavailable\");\n"
    "    return http_server_send_json_response(req, root);\n"
    "}\n"
    "\n"
    "esp_err_t http_server_register_camera_routes(httpd_handle_t server)\n"
    "{\n"
    "    const httpd_uri_t handler = {\n"
    "        .uri     = \"/api/camera/snapshot\",\n"
    "        .method  = HTTP_GET,\n"
    "        .handler = camera_unavailable_get_handler,\n"
    "    };\n"
    "    return httpd_register_uri_handler(server, &handler);\n"
    "}\n",
    1,
)
s = s.replace(
    "esp_err_t http_server_register_camera_routes(httpd_handle_t server)\n"
    "{\n"
    "    (void)server;\n"
    "    /* No camera on this board — silently skip route registration. */\n"
    "    return ESP_OK;\n"
    "}\n",
    "static esp_err_t camera_unavailable_get_handler(httpd_req_t *req)\n"
    "{\n"
    "    cJSON *root = cJSON_CreateObject();\n"
    "    if (!root) {\n"
    "        httpd_resp_send_500(req);\n"
    "        return ESP_ERR_NO_MEM;\n"
    "    }\n"
    "    cJSON_AddBoolToObject(root, \"available\", false);\n"
    "    http_server_json_add_string(root, \"state\", \"not_available\");\n"
    "    http_server_json_add_string(root, \"reason\", \"camera service is not enabled in this build\");\n"
    "    httpd_resp_set_status(req, \"503 Service Unavailable\");\n"
    "    return http_server_send_json_response(req, root);\n"
    "}\n"
    "\n"
    "esp_err_t http_server_register_camera_routes(httpd_handle_t server)\n"
    "{\n"
    "    const httpd_uri_t handler = {\n"
    "        .uri     = \"/api/camera/snapshot\",\n"
    "        .method  = HTTP_GET,\n"
    "        .handler = camera_unavailable_get_handler,\n"
    "    };\n"
    "    return httpd_register_uri_handler(server, &handler);\n"
    "}\n",
    1,
)
camera_api.write_text(s)
print("patched", cmake)
print("patched", camera_api)
PY
}

apply_gemini_http_diagnostics_patch() {
    local cmake="$ESP_CLAW_DIR/application/edge_agent/components/http_server/CMakeLists.txt"
    if [ ! -f "$cmake" ]; then
        log "http_server/CMakeLists not found — skipping Gemini diagnostics dependency patch"
        return
    fi
    if grep -q "CONFIG_APP_CLAW_CAP_GEMINI_LIVE" "$cmake" &&
       grep -q "cap_gemini_live" "$cmake"; then
        log "Gemini diagnostics dependency patch already applied"
        return
    fi
    log "applying patches/0007-gemini-http-dependency.patch"
    python3 - <<PY
import pathlib

cmake = pathlib.Path(r"$cmake")
s = cmake.read_text()
if 'CONFIG_APP_CLAW_CAP_GEMINI_LIVE' not in s:
    if 'if(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT AND CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_CAMERA)' in s:
        s = s.replace(
            'if(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT AND CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_CAMERA)\n'
            '    list(APPEND http_server_extra_requires lua_module_camera)\n'
            'endif()\n',
            'if(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT AND CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_CAMERA)\n'
            '    list(APPEND http_server_extra_requires lua_module_camera)\n'
            'endif()\n'
            'if(CONFIG_APP_CLAW_CAP_GEMINI_LIVE)\n'
            '    list(APPEND http_server_extra_requires cap_gemini_live)\n'
            'endif()\n',
            1,
        )
    elif 'REQUIRES' in s:
        s = s.replace(
            '    esp_timer\n',
            '    esp_timer\n'
            '    \n'
            '    if(CONFIG_APP_CLAW_CAP_GEMINI_LIVE)\n'
            '        list(APPEND http_server_extra_requires cap_gemini_live)\n'
            '    endif()\n',
            1,
        )
if 'if(CONFIG_APP_CLAW_CAP_GEMINI_LIVE)' not in s:
    raise SystemExit(f"could not locate GEMINI dependency insertion point in {cmake}")
cmake.write_text(s)
print("patched", cmake)
PY
}

apply_gemini_http_action_response_patch() {
    local api="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_gemini_api.c"
    if [ ! -f "$api" ]; then
        log "Gemini HTTP API source not found — skipping action response patch"
        return
    fi
    if grep -q "action_copy" "$api" 2>/dev/null; then
        log "Gemini HTTP action response patch already applied"
        return
    fi
    log "applying Gemini HTTP action response lifetime patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$api")
s = p.read_text()
old = '    esp_err_t err = ESP_OK;\\n    const char *status = "ok";\\n\\n'
new = '    esp_err_t err = ESP_OK;\\n    const char *status = "ok";\\n    char action_copy[24];\\n    strlcpy(action_copy, action, sizeof(action_copy));\\n\\n'
if old not in s:
    raise SystemExit("could not locate Gemini HTTP action locals")
s = s.replace(old, new, 1)
s = s.replace('    cJSON_AddStringToObject(resp, "action", action);\\n',
              '    cJSON_AddStringToObject(resp, "action", action_copy);\\n',
              1)
p.write_text(s)
print("patched", p)
PY
}

apply_gemini_live_main_require_patch() {
    local target="$ESP_CLAW_DIR/application/edge_agent/main/CMakeLists.txt"
    if [ ! -f "$target" ]; then
        log "main CMakeLists not found — skipping Gemini Live main dependency patch"
        return
    fi
    if grep -q "JarvisNano: main.c includes cap_gemini_live.h" "$target" 2>/dev/null; then
        log "Gemini Live main dependency patch already applied"
        return
    fi
    log "applying Gemini Live main dependency patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$target")
s = p.read_text()
old = (
    "    esp_lcd_touch\n"
    "    espressif__esp_board_manager\n"
    ")\n"
)
new = (
    "    esp_lcd_touch\n"
    "    espressif__esp_board_manager\n"
    "    # JarvisNano: main.c includes cap_gemini_live.h behind the Kconfig\n"
    "    # macro, but this local ESP-Claw checkout can expose the macro before\n"
    "    # CMake appends the conditional requirement. Keep the include path\n"
    "    # available so the Waveshare touch-to-talk build is reproducible.\n"
    "    cap_gemini_live\n"
    ")\n"
)
if old not in s:
    raise SystemExit(f"could not locate main_requires block in {p}")
p.write_text(s.replace(old, new, 1))
print("patched", p)
PY
}

apply_gemini_live_api_key_patch() {
    # patches/0020 — wire the Gemini Live API key from NVS into the capability.
    # main.c starts the touch-to-talk monitor but never provisions the key, so the
    # BidiGenerateContent WSS handshake has no auth. Insert one call before the
    # touch monitor starts. The "gemini_api_key" struct field (NVS key "gemini_key")
    # is accessed directly — app_config has no getter for it (only get_timezone).
    local main_c="$ESP_CLAW_DIR/application/edge_agent/main/main.c"
    if [ ! -f "$main_c" ]; then
        log "main.c not found — skipping Gemini Live API key patch"
        return
    fi
    if grep -q "cap_gemini_live_set_api_key" "$main_c" 2>/dev/null; then
        log "Gemini Live API key patch already applied"
        return
    fi
    log "applying patches/0020-gemini-live-api-key-wiring.patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$main_c")
s = p.read_text()
old = (
    '#if CONFIG_APP_CLAW_CAP_GEMINI_LIVE\n'
    '    /* Start touch monitor — polls every 80ms, calls cap_gemini_live_toggle() on tap */\n'
    '    xTaskCreate(touch_monitor_task, "touch_mon", 4096, NULL, 3, NULL);\n'
    '#endif\n'
)
new = (
    '#if CONFIG_APP_CLAW_CAP_GEMINI_LIVE\n'
    '    /* Provision the Gemini Live API key from NVS (app_config field "gemini_key").\n'
    '     * Without this the BidiGenerateContent session has no auth and the WSS handshake\n'
    '     * is rejected. The field is empty until set via POST /api/config. */\n'
    "    s_gemini_api_key_configured = (s_config->gemini_api_key[0] != '\\0');\\n"
    '    cap_gemini_live_set_api_key(s_config->gemini_api_key);\n'
    '    /* Start touch monitor — polls every 80ms, calls cap_gemini_live_toggle() on tap */\n'
    '    xTaskCreate(touch_monitor_task, "touch_mon", 4096, NULL, 3, NULL);\n'
    '#endif\n'
)
if old not in s:
    raise SystemExit("could not locate gemini-live touch monitor block in main.c — upstream may have changed")
p.write_text(s.replace(old, new, 1))
print("patched", p)
PY
}

apply_gemini_live_network_ready_patch() {
    local main_c="$ESP_CLAW_DIR/application/edge_agent/main/main.c"
    if [ ! -f "$main_c" ]; then
        log "main.c not found — skipping Gemini Live network-ready patch"
        return
    fi
    if grep -q "main_gemini_live_start" "$main_c" 2>/dev/null; then
        log "Gemini Live network-ready patch already applied"
        return
    fi
    log "applying Gemini Live network-ready guard"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$main_c")
s = p.read_text()

old = '''#if CONFIG_APP_CLAW_CAP_GEMINI_LIVE
static bool gemini_live_configured(void)
{
    return s_gemini_api_key_configured;
}
#endif
'''
new = '''#if CONFIG_APP_CLAW_CAP_GEMINI_LIVE
static bool gemini_live_network_ready(void)
{
    if (!s_wifi_manager_ready) {
        return false;
    }

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    return status.sta_connected && status.sta_ip[0] != '\\0';
}

static bool gemini_live_time_ready(void)
{
    return time(NULL) > 1700000000;
}

static bool gemini_live_configured(void)
{
    return s_gemini_api_key_configured && gemini_live_network_ready() && gemini_live_time_ready();
}

static esp_err_t main_gemini_live_start(void)
{
    if (!s_gemini_api_key_configured) {
        ESP_LOGW(TAG, "Gemini Live start rejected: API key not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!gemini_live_network_ready()) {
        ESP_LOGW(TAG, "Gemini Live start rejected: Wi-Fi STA not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (!gemini_live_time_ready()) {
        ESP_LOGW(TAG, "Gemini Live start rejected: system time not synced");
        return ESP_ERR_INVALID_STATE;
    }
    return cap_gemini_live_start();
}
#endif
'''
if old not in s:
    raise SystemExit("could not locate gemini_live_configured block in main.c")
s = s.replace(old, new, 1)
s = s.replace(".gemini_live_start = cap_gemini_live_start,", ".gemini_live_start = main_gemini_live_start,", 1)
p.write_text(s)
print("patched", p)
PY
}

apply_gemini_live_nonblocking_stop_patch() {
    local cap_c="$ESP_CLAW_DIR/components/claw_capabilities/cap_gemini_live/src/cap_gemini_live.c"
    if [ ! -f "$cap_c" ]; then
        log "cap_gemini_live.c not found — skipping nonblocking stop patch"
        return
    fi
    if grep -q "Gateway stop requested" "$cap_c" 2>/dev/null; then
        log "Gemini Live nonblocking stop patch already applied"
        return
    fi
    log "applying Gemini Live nonblocking stop patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$cap_c")
s = p.read_text()
old = '''    /* Do not clear task handles here. The session task owns codec cleanup and
     * must still see tx_task so it can wait for GL_BIT_TX_DONE before closing
     * ADC/DAC. Clearing handles early was the race. Rather theatrical. */
    if (session_task && session_task != xTaskGetCurrentTaskHandle()) {
        EventBits_t bits = xEventGroupWaitBits(s_gl.ev, GL_BIT_SESSION_DONE,
                                               pdFALSE, pdTRUE, pdMS_TO_TICKS(8000));
        if (!(bits & GL_BIT_SESSION_DONE)) {
            ESP_LOGE(TAG, "Gateway stop timed out — force cleanup");
            vTaskDelete(session_task);
            s_gl.session_task = NULL;
            if (s_gl.ws_client) {
                esp_websocket_client_destroy(s_gl.ws_client);
                s_gl.ws_client = NULL;
            }
            s_gl.ws_connected  = false;
            s_gl.stop_requested = false;
            gl_set_state(GL_STATE_IDLE, "stop-timeout");
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
'''
new = '''    /* Stop may be called by HTTP. Do not wait here: the session task owns codec
     * and WebSocket teardown, and blocking the HTTP server makes the device look
     * dead while cleanup runs. */
    if (session_task == xTaskGetCurrentTaskHandle()) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Gateway stop requested");
    return ESP_OK;
'''
if old not in s:
    raise SystemExit("could not locate blocking Gemini Live stop block")
p.write_text(s.replace(old, new, 1))
print("patched", p)
PY
}

apply_http_wifi_scan_patch() {
    local cmake="$ESP_CLAW_DIR/application/edge_agent/components/http_server/CMakeLists.txt"
    local status="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_status_api.c"
    if [ ! -f "$cmake" ] || [ ! -f "$status" ]; then
        log "http status route sources not found — skipping Wi-Fi scan patch"
        return
    fi
    if grep -q "wifi_manager" "$cmake" 2>/dev/null &&
       grep -q "/api/wifi/scan" "$status" 2>/dev/null; then
        log "Wi-Fi scan HTTP route patch already applied"
        return
    fi
    log "applying patches/0006-http-wifi-scan.patch"
    python3 - <<PY
import pathlib

cmake = pathlib.Path(r"$cmake")
status = pathlib.Path(r"$status")

s = cmake.read_text()
if "        wifi_manager\n" not in s:
    old = "        json\n"
    if old not in s:
        raise SystemExit(f"could not locate http_server REQUIRES list in {cmake}")
    s = s.replace(old, old + "        wifi_manager\n", 1)
cmake.write_text(s)

s = status.read_text()
if '#include "wifi_manager.h"' not in s:
    old = '#include "http_server_priv.h"\n'
    if old not in s:
        raise SystemExit(f"could not locate status API include point in {status}")
    s = s.replace(old, old + '\n#include "wifi_manager.h"\n', 1)

old = (
    '#include "wifi_manager.h"\n'
    '\n'
    'static esp_err_t status_handler(httpd_req_t *req)\n'
)
new = (
    '#include "wifi_manager.h"\n'
    '\n'
    '#define HTTP_WIFI_SCAN_LIMIT 20\n'
    '\n'
    'static const char *wifi_auth_mode_to_string(wifi_auth_mode_t authmode)\n'
    '{\n'
    '    switch (authmode) {\n'
    '    case WIFI_AUTH_OPEN:            return "open";\n'
    '    case WIFI_AUTH_WEP:             return "wep";\n'
    '    case WIFI_AUTH_WPA_PSK:         return "wpa_psk";\n'
    '    case WIFI_AUTH_WPA2_PSK:        return "wpa2_psk";\n'
    '    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa_wpa2_psk";\n'
    '    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";\n'
    '    case WIFI_AUTH_WPA3_PSK:        return "wpa3_psk";\n'
    '    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2_wpa3_psk";\n'
    '    case WIFI_AUTH_WAPI_PSK:        return "wapi_psk";\n'
    '    default:                        return "unknown";\n'
    '    }\n'
    '}\n'
    '\n'
    'static esp_err_t status_handler(httpd_req_t *req)\n'
)
if "HTTP_WIFI_SCAN_LIMIT" not in s:
    if old not in s:
        raise SystemExit(f"could not locate Wi-Fi scan helper insertion point in {status}")
    s = s.replace(old, new, 1)

old = (
    'static esp_err_t battery_handler(httpd_req_t *req)\n'
    '{\n'
)
new = (
    'static esp_err_t wifi_scan_handler(httpd_req_t *req)\n'
    '{\n'
    '    wifi_manager_scan_record_t records[HTTP_WIFI_SCAN_LIMIT] = {0};\n'
    '    uint16_t count = 0;\n'
    '    esp_err_t err = wifi_manager_scan_aps(records, HTTP_WIFI_SCAN_LIMIT, &count);\n'
    '    if (err != ESP_OK) {\n'
    '        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to scan Wi-Fi APs");\n'
    '    }\n'
    '\n'
    '    cJSON *root = cJSON_CreateObject();\n'
    '    cJSON *aps = root ? cJSON_CreateArray() : NULL;\n'
    '    if (!root || !aps) {\n'
    '        cJSON_Delete(root);\n'
    '        httpd_resp_send_500(req);\n'
    '        return ESP_ERR_NO_MEM;\n'
    '    }\n'
    '    cJSON_AddItemToObject(root, "aps", aps);\n'
    '\n'
    '    for (uint16_t i = 0; i < count; i++) {\n'
    '        cJSON *ap = cJSON_CreateObject();\n'
    '        if (!ap || !cJSON_AddItemToArray(aps, ap)) {\n'
    '            cJSON_Delete(ap);\n'
    '            cJSON_Delete(root);\n'
    '            httpd_resp_send_500(req);\n'
    '            return ESP_ERR_NO_MEM;\n'
    '        }\n'
    '        http_server_json_add_string(ap, "ssid", records[i].ssid);\n'
    '        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);\n'
    '        cJSON_AddNumberToObject(ap, "channel", records[i].primary);\n'
    '        http_server_json_add_string(ap, "auth", wifi_auth_mode_to_string(records[i].authmode));\n'
    '    }\n'
    '\n'
    '    return http_server_send_json_response(req, root);\n'
    '}\n'
    '\n'
    'static esp_err_t battery_handler(httpd_req_t *req)\n'
    '{\n'
)
if "wifi_scan_handler" not in s:
    if old not in s:
        raise SystemExit(f"could not locate Wi-Fi scan handler insertion point in {status}")
    s = s.replace(old, new, 1)

old = '        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },\n'
new = old + '        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler },\n'
if '"/api/wifi/scan"' not in s:
    if old not in s:
        raise SystemExit(f"could not locate status route table in {status}")
    s = s.replace(old, new, 1)
status.write_text(s)
print("patched", cmake)
print("patched", status)
PY
}

apply_http_health_patch() {
    local status="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_status_api.c"
    if [ ! -f "$status" ]; then
        log "http status route source not found — skipping health patch"
        return
    fi
    if grep -q "/api/health" "$status" 2>/dev/null; then
        log "HTTP health route patch already applied"
        return
    fi
    log "applying patches/0008-http-health-route.patch"
    python3 - <<PY
import pathlib

status = pathlib.Path(r"$status")
s = status.read_text()

include_anchor = '#include "http_server_priv.h"\n'
if '#include "esp_heap_caps.h"' not in s:
    if include_anchor not in s:
        raise SystemExit(f"could not locate health include insertion point in {status}")
    s = s.replace(
        include_anchor,
        include_anchor + '\n#include "esp_heap_caps.h"\n#include "esp_timer.h"\n',
        1,
    )

helper_anchor = "static esp_err_t status_handler(httpd_req_t *req)\n{\n"
health_handler = (
    "static esp_err_t health_handler(httpd_req_t *req)\n"
    "{\n"
    "    static uint32_t s_health_requests;\n"
    "    http_server_ctx_t *ctx = http_server_ctx();\n"
    "    http_server_wifi_status_t status = {0};\n"
    "    bool wifi_status_ok = false;\n"
    "    if (ctx->services.get_wifi_status) {\n"
    "        wifi_status_ok = (ctx->services.get_wifi_status(&status) == ESP_OK);\n"
    "    }\n"
    "\n"
    "    cJSON *root = cJSON_CreateObject();\n"
    "    if (!root) {\n"
    "        httpd_resp_send_500(req);\n"
    "        return ESP_ERR_NO_MEM;\n"
    "    }\n"
    "\n"
    "    cJSON_AddBoolToObject(root, \"ok\", true);\n"
    "    cJSON_AddNumberToObject(root, \"uptime_ms\", (double)(esp_timer_get_time() / 1000));\n"
    "    cJSON_AddNumberToObject(root, \"free_heap\", heap_caps_get_free_size(MALLOC_CAP_8BIT));\n"
    "    cJSON_AddNumberToObject(root, \"min_free_heap\", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));\n"
    "    cJSON_AddNumberToObject(root, \"requests\", ++s_health_requests);\n"
    "    cJSON_AddBoolToObject(root, \"wifi_status_ok\", wifi_status_ok);\n"
    "    http_server_json_add_string(root, \"wifi_mode\", status.wifi_mode);\n"
    "    http_server_json_add_string(root, \"ip\", status.ip);\n"
    "    http_server_json_add_string(root, \"ap_ip\", status.ap_ip);\n"
    "    return http_server_send_json_response(req, root);\n"
    "}\n"
    "\n"
)
if "health_handler" not in s:
    if helper_anchor not in s:
        raise SystemExit(f"could not locate health handler insertion point in {status}")
    s = s.replace(helper_anchor, health_handler + helper_anchor, 1)

route_anchor = '        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },\n'
if '"/api/health"' not in s:
    if route_anchor not in s:
        raise SystemExit(f"could not locate health route table point in {status}")
    s = s.replace(
        route_anchor,
        '        { .uri = "/api/health", .method = HTTP_GET, .handler = health_handler },\n' + route_anchor,
        1,
    )

status.write_text(s)
print("patched", status)
PY
}

apply_native_status_led_patch() {
    local main_c="$ESP_CLAW_DIR/application/edge_agent/main/main.c"
    if [ ! -f "$main_c" ]; then
        log "edge_agent main.c not found — skipping native status LED patch"
        return
    fi
    if grep -q "Native status LED on gpio" "$main_c" 2>/dev/null; then
        log "native status LED patch already applied"
        return
    fi
    log "applying native GPIO21 status LED patch"
    python3 - <<PY
import pathlib

p = pathlib.Path(r"$main_c")
s = p.read_text()

s = s.replace('#include "app_claw.h"\n', '#include "app_claw.h"\n#include <stdbool.h>\n', 1)
s = s.replace('#include "esp_system.h"\n', '#include "esp_system.h"\n#include "driver/gpio.h"\n', 1)
s = s.replace(
    '#define APP_FATFS_PARTITION_LABEL "storage"\n#define APP_ENABLE_MEM_LOG        (0)\n',
    '#define APP_FATFS_PARTITION_LABEL "storage"\n#define APP_ENABLE_MEM_LOG        (0)\n#define APP_STATUS_LED_GPIO       GPIO_NUM_21\n#define APP_STATUS_LED_ACTIVE_LOW (1)\n',
    1,
)

insert_after = 'static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;\n'
native_led = r'''

static void app_status_led_set(bool on)
{
    gpio_set_level(APP_STATUS_LED_GPIO,
                   (APP_STATUS_LED_ACTIVE_LOW ? !on : on) ? 1 : 0);
}

static void app_status_led_blink(int times, int on_ms, int off_ms)
{
    for (int i = 0; i < times; i++) {
        app_status_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        app_status_led_set(false);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

static void app_status_led_pulse(const uint8_t *levels, size_t count, int frame_ms)
{
    for (size_t i = 0; i < count; i++) {
        int on_ms = levels[i];
        if (on_ms > 0) {
            app_status_led_set(true);
            vTaskDelay(pdMS_TO_TICKS(on_ms));
        }
        if (frame_ms > on_ms) {
            app_status_led_set(false);
            vTaskDelay(pdMS_TO_TICKS(frame_ms - on_ms));
        }
    }
    app_status_led_set(false);
}

static void app_status_led_task(void *arg)
{
    static const uint8_t boot_pulse[] = {2, 4, 7, 11, 15, 18, 15, 11, 7, 4, 2};
    static const uint8_t idle_main[] = {2, 4, 7, 11, 15, 18, 15, 11, 7, 4, 2};
    static const uint8_t idle_echo[] = {2, 5, 9, 12, 9, 5, 2};
    (void)arg;

    app_status_led_blink(2, 70, 80);
    app_status_led_pulse(boot_pulse, sizeof(boot_pulse), 18);
    vTaskDelay(pdMS_TO_TICKS(120));
    app_status_led_blink(1, 220, 180);

    while (true) {
        app_status_led_pulse(idle_main, sizeof(idle_main), 18);
        vTaskDelay(pdMS_TO_TICKS(120));
        app_status_led_pulse(idle_echo, sizeof(idle_echo), 16);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

static esp_err_t app_status_led_start(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << APP_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }
    app_status_led_set(false);

    ESP_LOGI(TAG, "Native status LED on gpio %d active_low=%d",
             APP_STATUS_LED_GPIO,
             APP_STATUS_LED_ACTIVE_LOW);

    BaseType_t ok = xTaskCreate(app_status_led_task, "status_led", 1536, NULL, 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
'''
if insert_after not in s:
    raise SystemExit(f"could not locate status LED insertion point in {p}")
s = s.replace(insert_after, insert_after + native_led, 1)

s = s.replace(
    '    ESP_ERROR_CHECK(esp_board_manager_init());\n    ESP_ERROR_CHECK(app_claw_ui_start());\n',
    '    ESP_ERROR_CHECK(esp_board_manager_init());\n    ESP_ERROR_CHECK(app_claw_ui_start());\n',
    1,
)
s = s.replace(
    '    ESP_ERROR_CHECK(app_claw_init_storage_paths(s_claw_paths));\n    ESP_ERROR_CHECK(app_claw_start(s_claw_config, s_claw_paths));\n',
    '    ESP_ERROR_CHECK(app_claw_init_storage_paths(s_claw_paths));\n    ESP_ERROR_CHECK(app_claw_start(s_claw_config, s_claw_paths));\n'
    '    esp_err_t status_led_err = app_status_led_start();\n'
    '    if (status_led_err != ESP_OK) {\n'
    '        ESP_LOGW(TAG, "Status LED heartbeat disabled: %s", esp_err_to_name(status_led_err));\n'
    '    }\n',
    1,
)

p.write_text(s)
print("patched", p)
PY
}

apply_emote_status_detail_patch() {
    # patches/0009 — emote idle screen shows the active LLM model
    # ("Ready * <model>") instead of a generic Wi-Fi message. Touches 3 files.
    local emote_c="$ESP_CLAW_DIR/components/common/emote/emote.c"
    local emote_h="$ESP_CLAW_DIR/components/common/emote/include/emote.h"
    local app_c="$ESP_CLAW_DIR/components/common/app_claw/app_claw.c"
    if [ ! -f "$emote_c" ] || [ ! -f "$emote_h" ] || [ ! -f "$app_c" ]; then
        log "emote/app_claw sources not found — skipping emote status-detail patch"
        return
    fi
    if grep -q "emote_set_status_detail" "$emote_c" 2>/dev/null; then
        log "emote status-detail patch already applied"
        return
    fi
    log "applying patches/0009-emote-status-detail.patch"
    python3 - <<PY
import pathlib

emote_c = pathlib.Path(r"$emote_c")
emote_h = pathlib.Path(r"$emote_h")
app_c   = pathlib.Path(r"$app_c")

# --- emote.c: split emote_set_network_status into render + two setters ---
s = emote_c.read_text()
old = (
    "esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid)\n"
    "{\n"
    "    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, \"emote handle is NULL\");\n"
    "\n"
    "    const bool ap_present = (ap_ssid != NULL && ap_ssid[0] != '\\0');\n"
    "    const char *idle = sta_connected ? \"swim\" : \"offline\";\n"
    "\n"
    "    char msg[96];\n"
    "    if (sta_connected && ap_present) {\n"
    "        snprintf(msg, sizeof(msg), \"Online * AP: %s\", ap_ssid);\n"
    "    } else if (sta_connected) {\n"
    "        snprintf(msg, sizeof(msg), \"Wi-Fi connected\");\n"
    "    } else if (ap_present) {\n"
    "        snprintf(msg, sizeof(msg), \"Setup WiFi: %s\", ap_ssid);\n"
    "    } else {\n"
    "        snprintf(msg, sizeof(msg), \"Wi-Fi offline\");\n"
    "    }\n"
    "\n"
    "    return emote_apply(idle, msg);\n"
    "}\n"
)
new = (
    "static bool s_sta_connected;\n"
    "static char s_ap_ssid[48];\n"
    "static char s_status_detail[48];   /* e.g. the active LLM model name */\n"
    "static bool s_voice_visual_active;\n"
    "\n"
    "static esp_err_t emote_render_status(void)\n"
    "{\n"
    "    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, \"emote handle is NULL\");\n"
    "\n"
    "    const bool ap_present = (s_ap_ssid[0] != '\\0');\n"
    "    const bool has_detail = (s_status_detail[0] != '\\0');\n"
    "    const char *idle = s_sta_connected ? \"swim\" : \"offline\";\n"
    "\n"
    "    char msg[96];\n"
    "    if (s_sta_connected && has_detail) {\n"
    "        snprintf(msg, sizeof(msg), \"Ready * %s\", s_status_detail);\n"
    "    } else if (s_sta_connected && ap_present) {\n"
    "        snprintf(msg, sizeof(msg), \"Online * AP: %s\", s_ap_ssid);\n"
    "    } else if (s_sta_connected) {\n"
    "        snprintf(msg, sizeof(msg), \"Wi-Fi connected\");\n"
    "    } else if (ap_present) {\n"
    "        snprintf(msg, sizeof(msg), \"Setup WiFi: %s\", s_ap_ssid);\n"
    "    } else {\n"
    "        snprintf(msg, sizeof(msg), \"Wi-Fi offline\");\n"
    "    }\n"
    "\n"
    "    return emote_apply(idle, msg);\n"
    "}\n"
    "\n"
    "esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid)\n"
    "{\n"
    "    s_sta_connected = sta_connected;\n"
    "    if (ap_ssid != NULL && ap_ssid[0] != '\\0') {\n"
    "        snprintf(s_ap_ssid, sizeof(s_ap_ssid), \"%s\", ap_ssid);\n"
    "    } else {\n"
    "        s_ap_ssid[0] = '\\0';\n"
    "    }\n"
    "    return emote_render_status();\n"
    "}\n"
    "\n"
    "esp_err_t emote_set_status_detail(const char *detail)\n"
    "{\n"
    "    if (detail != NULL && detail[0] != '\\0') {\n"
    "        snprintf(s_status_detail, sizeof(s_status_detail), \"%s\", detail);\n"
    "    } else {\n"
    "        s_status_detail[0] = '\\0';\n"
    "    }\n"
    "    if (s_voice_visual_active) {\n"
    "        return ESP_OK;\n"
    "    }\n"
    "    if (s_emote_handle != NULL) {\n"
    "        return emote_render_status();\n"
    "    }\n"
    "    return ESP_OK;\n"
    "}\n"
)
if old not in s:
    raise SystemExit("could not locate emote_set_network_status in emote.c — upstream may have changed")
emote_c.write_text(s.replace(old, new))

# --- emote.h: declare the new setter ---
h = emote_h.read_text()
h_old = "esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid);\n"
h_new = (
    "esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid);\n"
    "esp_err_t emote_set_status_detail(const char *detail);\n"
)
if h_old not in h:
    raise SystemExit("could not locate emote_set_network_status decl in emote.h")
emote_h.write_text(h.replace(h_old, h_new, 1))

# --- app_claw.c: call the setter after the LLM provider starts ---
a = app_c.read_text()
a_old = (
    "                 config->llm_model);\n"
    "        ESP_RETURN_ON_ERROR(claw_core_init(&core_config), TAG, \"Failed to init claw_core\");\n"
)
a_new = (
    "                 config->llm_model);\n"
    "#if defined(CONFIG_APP_CLAW_ENABLE_EMOTE)\n"
    "        emote_set_status_detail(config->llm_model);\n"
    "#endif\n"
    "        ESP_RETURN_ON_ERROR(claw_core_init(&core_config), TAG, \"Failed to init claw_core\");\n"
)
if a_old not in a:
    raise SystemExit("could not locate claw_core_init anchor in app_claw.c")
app_c.write_text(a.replace(a_old, a_new, 1))
print("patched emote.c, emote.h, app_claw.c")
PY
}

apply_touch_handle_deref_patch() {
    # patches/0010 — fix the CST9217 "must be initialized" spam in the upstream
    # main.c touch monitor. esp_board_manager_get_device_handle() returns the
    # INNER device handle (dev_lcd_touch_i2c_handles_t*), not the
    # esp_board_device_handle_t wrapper, so reading ->device_handle off it walks
    # past the struct into garbage and yields a non-NULL touch handle with a NULL
    # read_data/get_xy vtable. Cast the returned pointer directly and read its
    # first member, touch_handle — same as lua_module_board_manager.c.
    local main_c="$ESP_CLAW_DIR/application/edge_agent/main/main.c"
    if [ -f "$ESP_CLAW_DIR/application/edge_agent/main/touch_demo.c" ]; then
        log "touch handle deref patch superseded by touch_demo.c"
        return
    fi
    if [ ! -f "$main_c" ]; then
        log "main.c not found — skipping touch handle deref patch"
        return
    fi
    if grep -q "touch = ((dev_lcd_touch_i2c_handles_t \*)dev_h)->touch_handle;" "$main_c" 2>/dev/null; then
        log "touch handle deref patch already applied"
        return
    fi
    log "applying patches/0010-touch-handle-deref-fix.patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$main_c")
s = p.read_text()
old = (
    "    /* Wait until board manager has the touch handle ready */\n"
    "    while (touch == NULL) {\n"
    "        esp_board_device_handle_t *touch_h = NULL;\n"
    "        if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,\n"
    "                                                (void **)&touch_h) == ESP_OK && touch_h) {\n"
    "            touch = *(esp_lcd_touch_handle_t *)touch_h->device_handle;\n"
    "        }\n"
    "        if (touch == NULL) {\n"
    "            vTaskDelay(pdMS_TO_TICKS(500));\n"
    "        }\n"
    "    }\n"
)
new = (
    "    /* Wait until board manager has the touch handle ready.\n"
    "     * esp_board_manager_get_device_handle() returns the device's INNER handle\n"
    "     * (the dev_lcd_touch_i2c_handles_t* the device init stored), not the\n"
    "     * esp_board_device_handle_t wrapper. Its first member is the real\n"
    "     * esp_lcd_touch_handle_t. Reading ->device_handle off it (as if it were the\n"
    "     * wrapper) walks past the 2-pointer struct and yields a garbage non-NULL\n"
    "     * handle whose read_data/get_xy vtable is NULL — that is what spammed\n"
    "     * \"Touch controller must be initialized\" at the poll rate. Mirror the\n"
    "     * working lua_module_board_manager.c::get_lcd_touch_handle cast. */\n"
    "    while (touch == NULL) {\n"
    "        void *dev_h = NULL;\n"
    "        if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,\n"
    "                                                &dev_h) == ESP_OK && dev_h) {\n"
    "            touch = ((dev_lcd_touch_i2c_handles_t *)dev_h)->touch_handle;\n"
    "        }\n"
    "        if (touch == NULL) {\n"
    "            vTaskDelay(pdMS_TO_TICKS(500));\n"
    "        }\n"
    "    }\n"
)
if old not in s:
    raise SystemExit("could not locate touch_monitor_task handle block in main.c — upstream may have changed")
p.write_text(s.replace(old, new, 1))
print("patched", p)
PY
}

apply_touch_local_hardware_demo_patch() {
    # Touch should do something useful even before cloud voice is configured:
    # tap toggles Gemini Live when a key exists, otherwise runs a local AMOLED
    # reactive-face demo; long-press always runs the local demo. This uses the
    # real CST9217 touch panel, CO5300 display, and cap_gemini_live synthetic RMS
    # path, so hardware can be validated without spending API calls.
    local main_c="$ESP_CLAW_DIR/application/edge_agent/main/main.c"
    local touch_demo_c="$ESP_CLAW_DIR/application/edge_agent/main/touch_demo.c"
    local touch_demo_h="$ESP_CLAW_DIR/application/edge_agent/main/touch_demo.h"
    local touch_demo_src="$ROOT/firmware/main/touch_demo.c"
    local touch_demo_hdr="$ROOT/firmware/main/touch_demo.h"
    if [ -f "$touch_demo_c" ]; then
        if [ -f "$touch_demo_src" ] && [ -f "$touch_demo_hdr" ]; then
            log "copying patched touch_demo runtime → upstream tree"
            cp -f "$touch_demo_src" "$touch_demo_c"
            cp -f "$touch_demo_hdr" "$touch_demo_h"
            python3 - "$main_c" <<'PY'
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
s = p.read_text()
if ".touch_get_diagnostics = touch_demo_get_diagnostics_json," not in s:
    anchor = "            .gemini_live_is_active = cap_gemini_live_is_active,\n"
    if anchor not in s:
        raise SystemExit("main.c touch diagnostics service anchor missing")
    s = s.replace(
        anchor,
        anchor
        + "            .touch_get_diagnostics = touch_demo_get_diagnostics_json,\n"
        + "            .touch_run_scene = touch_demo_run_scene,\n",
        1,
    )
p.write_text(s)
print("touch diagnostics service wired")
PY
            return
        fi
        log "patching touch_demo.c local tap scenes"
        python3 - "$touch_demo_c" <<'PY'
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
s = p.read_text()

helper = r'''static local_scene_t local_scene_from_touch(uint16_t tx)
{
    if (tx < 155) {
        return LOCAL_SCENE_LISTEN;
    }
    if (tx > 310) {
        return LOCAL_SCENE_SPEAK;
    }
    return LOCAL_SCENE_THINK;
}

static const char *local_scene_name(local_scene_t scene)
{
    switch (scene) {
    case LOCAL_SCENE_LISTEN: return "listen";
    case LOCAL_SCENE_THINK: return "think";
    case LOCAL_SCENE_SPEAK: return "speak";
    case LOCAL_SCENE_SHOWCASE:
    default: return "showcase";
    }
}

'''

if "local_scene_from_touch" not in s:
    anchor = "\nstatic void touch_monitor_task(void *arg)\n"
    if anchor not in s:
        raise SystemExit("touch_demo.c touch_monitor_task anchor missing")
    s = s.replace(anchor, "\n" + helper + anchor, 1)

s = s.replace("     *   short tap  - Gemini Live toggle\n"
              "     *   long press - local hardware showcase\n",
              "     *   centre tap - Gemini Live toggle when ready, else local scanner\n"
              "     *   left tap   - local listening field\n"
              "     *   right tap  - local speaking pulse\n"
              "     *   long press - local hardware showcase\n")
s = s.replace("    const int PRESS_CONFIRM = 2;       /* ~160ms stable touch to count as a press */\n",
              "    const int PRESS_CONFIRM = 1;       /* one poll is enough for a deliberate tap */\n")
s = s.replace("    const int RELEASE_CONFIRM = 4;     /* ~320ms stable release to re-arm */\n",
              "    const int RELEASE_CONFIRM = 2;     /* ~160ms stable release to re-arm */\n")

if "uint16_t last_x = 233;" not in s:
    marker = "    bool long_fired = false;\n    (void)was_touching;\n"
    if marker not in s:
        raise SystemExit("touch_demo.c debounce variable anchor missing")
    s = s.replace(marker, "    bool long_fired = false;\n    uint16_t last_x = 233;\n    (void)was_touching;\n", 1)

if "last_x = x[0];" not in s:
    marker = "        if (touching) {\n            touch_run++;\n"
    if marker not in s:
        raise SystemExit("touch_demo.c touching branch anchor missing")
    s = s.replace(marker, "        if (touching) {\n            last_x = x[0];\n            touch_run++;\n", 1)

old = '''            if (just_released && armed && press_seen && !long_fired) {
                if (cap_gemini_live_is_active()) {
                    ESP_LOGI(TAG, "Tap: toggling Gemini Live off");
                    cap_gemini_live_toggle();
                } else if (gemini_live_configured()) {
                    ESP_LOGI(TAG, "Tap: toggling Gemini Live on");
                    cap_gemini_live_toggle();
                } else {
                    ESP_LOGW(TAG, "Tap ignored: Gemini Live not ready");
                }
                armed = false;
            }
'''
new = '''            if (just_released && armed && press_seen && !long_fired) {
                local_scene_t scene = local_scene_from_touch(last_x);
                if (cap_gemini_live_is_active()) {
                    ESP_LOGI(TAG, "Tap: toggling Gemini Live off (x=%u)",
                             (unsigned)last_x);
                    cap_gemini_live_toggle();
                } else if (gemini_live_configured() && scene == LOCAL_SCENE_THINK) {
                    ESP_LOGI(TAG, "Tap: toggling Gemini Live on (x=%u)",
                             (unsigned)last_x);
                    cap_gemini_live_toggle();
                } else {
                    ESP_LOGI(TAG, "Tap: local scene %s (x=%u)",
                             local_scene_name(scene), (unsigned)last_x);
                    local_hardware_demo_start(scene);
                }
                armed = false;
            }
'''
if old in s:
    s = s.replace(old, new, 1)
elif "Tap: local scene" not in s:
    raise SystemExit("touch_demo.c tap action block missing")

p.write_text(s)
print("patched", p)
PY
        return
    fi
    if [ ! -f "$main_c" ]; then
        log "main.c not found — skipping touch local hardware demo patch"
        return
    fi
    if grep -q "local_hardware_demo_task" "$main_c" 2>/dev/null; then
        log "touch local hardware demo patch already applied"
        return
    fi
    log "applying touch local hardware demo patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$main_c")
s = p.read_text()

inc_old = (
    '#if CONFIG_APP_CLAW_CAP_GEMINI_LIVE\n'
    '#include "cap_gemini_live.h"\n'
    '#include "esp_lcd_touch.h"\n'
    '#endif\n'
)
inc_new = (
    '#if CONFIG_APP_CLAW_CAP_GEMINI_LIVE\n'
    '#include "cap_gemini_live.h"\n'
    '#include "emote.h"\n'
    '#include "esp_lcd_touch.h"\n'
    '#endif\n'
)
if inc_old in s:
    s = s.replace(inc_old, inc_new, 1)

helper_anchor = "static void touch_monitor_task(void *arg)\n"
helpers = r'''
typedef enum {
    LOCAL_SCENE_SHOWCASE = 0,
    LOCAL_SCENE_LISTEN,
    LOCAL_SCENE_THINK,
    LOCAL_SCENE_SPEAK,
} local_scene_t;

static TaskHandle_t s_local_demo_task;
static volatile local_scene_t s_local_demo_scene = LOCAL_SCENE_SHOWCASE;
static bool s_gemini_api_key_configured;

static bool gemini_live_configured(void);

static void local_scene_listen(void)
{
    emote_set_status_detail("touch: listening field");
    emote_set_listening();
    for (int i = 0; i < 30; ++i) {
        float lvl = (float)((i % 10) + 1) / 10.0f;
        cap_gemini_live_set_synthetic_levels(lvl, 0.0f);
        vTaskDelay(pdMS_TO_TICKS(90));
    }
}

static void local_scene_think(void)
{
    emote_set_status_detail("touch: scanner");
    emote_set_thinking();
    for (int i = 0; i < 18; ++i) {
        float lvl = (i % 2) ? 0.35f : 0.0f;
        cap_gemini_live_set_synthetic_levels(lvl, lvl);
        vTaskDelay(pdMS_TO_TICKS(140));
    }
}

static void local_scene_speak(void)
{
    emote_set_status_detail("touch: voice pulse");
    emote_set_speaking();
    for (int i = 0; i < 34; ++i) {
        float lvl = (float)((i % 12) + 1) / 12.0f;
        cap_gemini_live_set_synthetic_levels(0.0f, lvl);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void local_hardware_demo_task(void *arg)
{
    (void)arg;
    local_scene_t scene = s_local_demo_scene;
    ESP_LOGI("touch_mon", "Local hardware demo started scene=%d", (int)scene);

    emote_set_connecting();
    vTaskDelay(pdMS_TO_TICKS(scene == LOCAL_SCENE_SHOWCASE ? 700 : 350));

    switch (scene) {
    case LOCAL_SCENE_LISTEN:
        local_scene_listen();
        break;
    case LOCAL_SCENE_THINK:
        local_scene_think();
        break;
    case LOCAL_SCENE_SPEAK:
        local_scene_speak();
        break;
    case LOCAL_SCENE_SHOWCASE:
    default:
        local_scene_listen();
        local_scene_think();
        local_scene_speak();
        break;
    }

    cap_gemini_live_set_synthetic_levels(0.0f, 0.0f);
    emote_set_status_detail("");
    emote_set_voice_idle();
    ESP_LOGI("touch_mon", "Local hardware demo finished");
    s_local_demo_task = NULL;
    vTaskDelete(NULL);
}

static void local_hardware_demo_start(local_scene_t scene)
{
    if (cap_gemini_live_is_active()) {
        ESP_LOGI("touch_mon", "Local hardware demo suppressed while Gemini Live is active");
        return;
    }
    if (s_local_demo_task) {
        ESP_LOGI("touch_mon", "Local hardware demo already running");
        return;
    }
    s_local_demo_scene = scene;
    if (xTaskCreate(local_hardware_demo_task, "hw_demo", 4096, NULL, 3,
                    &s_local_demo_task) != pdPASS) {
        s_local_demo_task = NULL;
        ESP_LOGW("touch_mon", "Failed to start local hardware demo");
    }
}

static bool gemini_live_configured(void)
{
    return s_gemini_api_key_configured;
}

static local_scene_t local_scene_from_touch(uint16_t tx)
{
    if (tx < 155) {
        return LOCAL_SCENE_LISTEN;
    }
    if (tx > 310) {
        return LOCAL_SCENE_SPEAK;
    }
    return LOCAL_SCENE_THINK;
}

static const char *local_scene_name(local_scene_t scene)
{
    switch (scene) {
    case LOCAL_SCENE_LISTEN: return "listen";
    case LOCAL_SCENE_THINK: return "think";
    case LOCAL_SCENE_SPEAK: return "speak";
    case LOCAL_SCENE_SHOWCASE:
    default: return "showcase";
    }
}

'''
if helper_anchor not in s:
    raise SystemExit("could not locate touch_monitor_task anchor")
s = s.replace(helper_anchor, helpers + helper_anchor, 1)

old_loop = (
    "    while (1) {\n"
    "        esp_lcd_touch_read_data(touch);\n"
    "        bool touching = (esp_lcd_touch_get_coordinates(touch, x, y, strength,\n"
    "                                                       &point_num, 1) && point_num > 0);\n"
    "        if (touching && !was_touching) {\n"
    '            ESP_LOGI("touch_mon", "Tap: toggling Gemini Live");\n'
    "            cap_gemini_live_toggle();\n"
    "        }\n"
    "        was_touching = touching;\n"
    "        vTaskDelay(pdMS_TO_TICKS(80));\n"
    "    }\n"
)
new_loop = (
    '    ESP_LOGI("touch_mon", "Touch monitor task started");\n'
    "\n"
    "    /* Debounced gesture detection. The CST9217 read flickers (point_num oscillates\n"
    "     * 0/1 between adjacent polls — phantom noise), so a bare rising-edge fired a\n"
    "     * flood of taps that thrashed the voice session and left it stuck on\n"
    "     * \"Connecting\". Require a CONFIRMED press (>=2 consecutive touching polls)\n"
    "     * after a CLEAN sustained release (>=4 consecutive not-touching polls), and\n"
    "     * disarm until that clean release returns. One genuine finger-down = one tap;\n"
    "     * single-poll flicker and stuck-on noise are filtered. cap_gemini_live_toggle\n"
    "     * keeps its own 2s debounce as a backstop.\n"
    "     *\n"
    "     * Gestures:\n"
    "     *   centre tap — Gemini Live toggle when configured; otherwise scanner scene\n"
    "     *   left tap   — local listening field\n"
    "     *   right tap  — local speaking pulse\n"
    "     *   long press — full local hardware showcase\n"
    "     */\n"
    "    const int PRESS_CONFIRM   = 2;   /* ~160ms stable touch to count as a press   */\n"
    "    const int RELEASE_CONFIRM = 4;   /* ~320ms stable release to re-arm           */\n"
    "    const int LONG_CONFIRM    = 15;  /* ~1.2s stable touch = local hardware demo  */\n"
    "    int  touch_run   = 0;            /* consecutive touching polls                */\n"
    "    int  release_run = RELEASE_CONFIRM; /* consecutive not-touching polls (armed)  */\n"
    "    bool armed       = true;\n"
    "    bool press_seen  = false;\n"
    "    bool long_fired  = false;\n"
    "    uint16_t last_x  = 233;\n"
    "    (void)was_touching;\n"
    "\n"
    "    while (1) {\n"
    "        esp_lcd_touch_read_data(touch);\n"
    "        bool touching = (esp_lcd_touch_get_coordinates(touch, x, y, strength,\n"
    "                                                       &point_num, 1) && point_num > 0);\n"
    "        if (touching) {\n"
    "            last_x = x[0];\n"
    "            touch_run++;\n"
    "            release_run = 0;\n"
    "            if (touch_run >= PRESS_CONFIRM) {\n"
    "                press_seen = true;\n"
    "            }\n"
    "            if (armed && !long_fired && touch_run == LONG_CONFIRM) {\n"
    "                if (cap_gemini_live_is_active()) {\n"
    '                    ESP_LOGI("touch_mon", "Long press ignored while Gemini Live is active");\n'
    "                } else {\n"
    '                    ESP_LOGI("touch_mon", "Long press: local hardware showcase");\n'
    "                    local_hardware_demo_start(LOCAL_SCENE_SHOWCASE);\n"
    "                }\n"
    "                long_fired = true;\n"
    "                armed = false;             /* need a clean release before any new gesture */\n"
    "            }\n"
    "        } else {\n"
    "            bool just_released = touch_run > 0;\n"
    "            release_run++;\n"
    "            if (just_released && armed && press_seen && !long_fired) {\n"
    "                local_scene_t scene = local_scene_from_touch(last_x);\n"
    "                if (cap_gemini_live_is_active()) {\n"
    '                    ESP_LOGI("touch_mon", "Tap ignored while Gemini Live is active");\n'
    "                } else if (gemini_live_configured() && scene == LOCAL_SCENE_THINK) {\n"
    '                    ESP_LOGI("touch_mon", "Tap: toggling Gemini Live on");\n'
    "                    cap_gemini_live_toggle();\n"
    "                } else {\n"
    '                    ESP_LOGI("touch_mon", "Tap: local scene %s (x=%u)",\n'
    "                             local_scene_name(scene), (unsigned)last_x);\n"
    "                    local_hardware_demo_start(scene);\n"
    "                }\n"
    "                armed = false;             /* one tap per press; need a clean release */\n"
    "            }\n"
    "            touch_run = 0;\n"
    "            press_seen = false;\n"
    "            long_fired = false;\n"
    "        }\n"
    "\n"
    "        if (release_run >= RELEASE_CONFIRM) {\n"
    "            armed = true;                  /* re-arm only after a sustained release   */\n"
    "        }\n"
    "        vTaskDelay(pdMS_TO_TICKS(80));\n"
    "    }\n"
)
if old_loop not in s:
    raise SystemExit("could not locate simple touch loop in main.c — upstream may have changed")
s = s.replace(old_loop, new_loop, 1)
p.write_text(s)
print("patched", p)
PY
}

apply_emote_voice_states_patch() {
    # patches/0012 — voice-state faces (Connecting/Listening/Thinking/Speaking/
    # Idle) + the eaf art they need. Runs AFTER apply_emote_status_detail_patch
    # (it appends to the emote_set_status_detail block that patch 0009 produces).
    # Idempotent and self-healing: converges whether the cloned tree carries the
    # original swim/offline pack or already carries the voice entries.
    local emote_c="$ESP_CLAW_DIR/components/common/emote/emote.c"
    local emote_h="$ESP_CLAW_DIR/components/common/emote/include/emote.h"
    local emote_json="$ESP_CLAW_DIR/components/common/emote/assets_local/284_240/emote.json"
    local coll_dst="$ESP_CLAW_DIR/components/common/emote/assets_local/emoji_large"
    local coll_src="$ESP_CLAW_DIR/application/edge_agent/managed_components/espressif2022__esp_emote_assets/emoji_large"
    if [ ! -f "$emote_c" ] || [ ! -f "$emote_h" ]; then
        log "emote sources not found — skipping emote voice-states patch"
        return
    fi

    # 1+2. Voice helpers in emote.c / emote.h (and swim->neutral idle face).
    if grep -q "emote_set_listening" "$emote_c" 2>/dev/null; then
        log "emote voice-states code already applied"
    else
        log "applying patches/0012-emote-voice-states.patch (code)"
        python3 - <<PY
import pathlib
emote_c = pathlib.Path(r"$emote_c")
emote_h = pathlib.Path(r"$emote_h")

# emote.c: idle face swim -> neutral (the JarvisNano face)
s = emote_c.read_text()
swim = '    const char *idle = s_sta_connected ? "swim" : "offline";\n'
neut = '    const char *idle = s_sta_connected ? "neutral" : "offline";\n'
if swim in s:
    s = s.replace(swim, neut, 1)

# emote.c: append voice helpers just before emote_cleanup().
anchor = "static void emote_cleanup(void)"
helpers = (
    "/* ---- Voice state helpers -------------------------------------------------- */\n"
    "/* Each maps a GL_STATE_* to an eye animation (name indexes emote.json) plus a\n"
    " * short status label. emote_apply() sets the anim + EMOTE_MGR_EVT_SYS message\n"
    " * and refreshes if we own the display arbiter. */\n"
    "\n"
    "esp_err_t emote_set_connecting(void)\n"
    "{\n"
    '    return emote_apply("thinking", "Connecting...");\n'
    "}\n"
    "\n"
    "esp_err_t emote_set_listening(void)\n"
    "{\n"
    '    return emote_apply("listen", "Listening...");\n'
    "}\n"
    "\n"
    "esp_err_t emote_set_thinking(void)\n"
    "{\n"
    '    return emote_apply("thinking", "Thinking...");\n'
    "}\n"
    "\n"
    "esp_err_t emote_set_speaking(void)\n"
    "{\n"
    '    return emote_apply("happy", "Speaking...");\n'
    "}\n"
    "\n"
    "esp_err_t emote_set_voice_idle(void)\n"
    "{\n"
    "    return emote_render_status();\n"
    "}\n"
    "\n"
)
if anchor not in s:
    raise SystemExit("could not locate emote_cleanup anchor in emote.c")
s = s.replace(anchor, helpers + anchor, 1)
emote_c.write_text(s)

# emote.h: declare the five helpers after emote_set_status_detail decl.
h = emote_h.read_text()
if "emote_set_listening" not in h:
    h_old = "esp_err_t emote_set_status_detail(const char *detail);\n"
    h_new = (
        "esp_err_t emote_set_status_detail(const char *detail);\n"
        "\n"
        "/* Voice state helpers — called by cap_gemini_live when session state changes.\n"
        " * One per GL_STATE_* so every transition repaints the face. */\n"
        "esp_err_t emote_set_connecting(void);\n"
        "esp_err_t emote_set_listening(void);\n"
        "esp_err_t emote_set_thinking(void);\n"
        "esp_err_t emote_set_speaking(void);\n"
        "esp_err_t emote_set_voice_idle(void);\n"
    )
    if h_old not in h:
        raise SystemExit("could not locate emote_set_status_detail decl in emote.h")
    emote_h.write_text(h.replace(h_old, h_new, 1))
print("patched emote.c + emote.h voice helpers")
PY
    fi

    # 3. emote.json — ensure the voice emotes are mapped to their eaf art.
    if [ -f "$emote_json" ] && ! grep -q '"listen"' "$emote_json" 2>/dev/null; then
        log "applying patches/0012 (emote.json voice entries)"
        cat > "$emote_json" <<'JSON'
[
    {"emote": "swim",        "src": "swim.eaf",     "loop": true,  "fps": 20},
    {"emote": "offline",     "src": "offline.eaf",  "loop": true,  "fps": 20},
    {"emote": "listen",      "src": "listen.eaf",   "loop": true,  "fps": 20},
    {"emote": "thinking",    "src": "confused.eaf", "loop": true,  "fps": 20},
    {"emote": "happy",       "src": "Happy.eaf",    "loop": true,  "fps": 20},
    {"emote": "neutral",     "src": "neutral.eaf",  "loop": false, "fps": 20},
    {"emote": "winking",     "src": "winking.eaf",  "loop": false, "fps": 20},
    {"emote": "surprised",   "src": "shocked.eaf",  "loop": false, "fps": 20},
    {"emote": "sad",         "src": "Sad.eaf",      "loop": true,  "fps": 20},
    {"emote": "angry",       "src": "angry.eaf",    "loop": true,  "fps": 20},
    {"emote": "rwave_idle",   "src": "rwave_idle.eaf",   "loop": true, "fps": 20},
    {"emote": "rwave_listen", "src": "rwave_listen.eaf", "loop": true, "fps": 24},
    {"emote": "rwave_think",  "src": "rwave_think.eaf",  "loop": true, "fps": 24},
    {"emote": "rwave_speak",  "src": "rwave_speak.eaf",  "loop": true, "fps": 24}
]
JSON
    fi
    # patch 0034: the four reactive-face EAFs are manifest-driven (build.py only
    # packs files listed in emote.json). The guard above only rewrites emote.json
    # when "listen" is absent, so on an already-patched tree force-add the rwave
    # entries if missing (idempotent).
    if [ -f "$emote_json" ] && ! grep -q "rwave_listen" "$emote_json" 2>/dev/null; then
        log "applying patches/0034 (emote.json rwave reactive-face entries)"
        python3 - "$emote_json" <<'PYR'
import sys, json, pathlib
p = pathlib.Path(sys.argv[1]); data = json.loads(p.read_text())
have = {e.get("emote") for e in data}
for name, fps in (("rwave_idle", 20), ("rwave_listen", 24), ("rwave_think", 24), ("rwave_speak", 24)):
    if name not in have:
        data.append({"emote": name, "src": f"{name}.eaf", "loop": True, "fps": fps})
p.write_text(json.dumps(data, indent=4) + "\n")
print("emote.json: rwave entries ensured")
PYR
    fi
    # Copy the vendored reactive-face EAFs into the emoji collection so build.py
    # packs them into the emote partition (source-of-truth: firmware/mascot/reactive/).
    local rwave_src="$ROOT/firmware/mascot/reactive"
    if [ -d "$rwave_src" ] && [ -d "$coll_dst" ]; then
        for f in rwave_idle.eaf rwave_listen.eaf rwave_think.eaf rwave_speak.eaf; do
            [ -f "$rwave_src/$f" ] && cp -f "$rwave_src/$f" "$coll_dst/$f"
        done
        log "copied reactive-face EAFs into emoji_large"
    else
        log "reactive-face EAFs not found at $rwave_src — face will fall back to eye"
    fi

    # 4. eaf art — copy the voice/expression frames into the local collection so
    # the faces render (build.py marks missing eaf "lack":true otherwise).
    if [ -d "$coll_src" ] && [ -d "$coll_dst" ]; then
        if [ ! -f "$coll_dst/listen.eaf" ]; then
            log "applying patches/0012 (copy voice eaf art into emoji_large)"
            for f in listen.eaf confused.eaf Happy.eaf neutral.eaf winking.eaf shocked.eaf Sad.eaf angry.eaf; do
                [ -f "$coll_src/$f" ] && cp -f "$coll_src/$f" "$coll_dst/$f"
            done
        else
            log "emote voice eaf art already present"
        fi
    else
        log "emoji_large source/dest missing — voice faces may render blank until art is added"
    fi
}

apply_emote_partition_resize_patch() {
    # patches/0030 — grow emote 3M->6M (storage 4M->5M), reclaim unused ota_1.
    # The 8 voice-face .eaf files push emote_assets.bin past the old 3M emote
    # partition (ESP_ERR_INVALID_SIZE → blank face); the 466 mascot pack needs
    # more still. Rewrites partitions_16MB.csv with explicit offsets. Idempotent
    # (guard: the 6M emote already present).
    local csv="$ESP_CLAW_DIR/application/edge_agent/partitions_16MB.csv"
    if [ ! -f "$csv" ]; then
        log "partitions_16MB.csv not found — skipping emote partition resize patch"
        return
    fi
    if grep -q "0x6E0000" "$csv" 2>/dev/null; then
        log "emote partition resize patch already applied"
        return
    fi
    log "applying patches/0030-emote-partition-resize (emote 6.875M, storage moved up)"
    python3 - <<PY
import pathlib
csv = pathlib.Path(r"$csv")
new = (
    "# Name,    Type, SubType, Offset,   Size\n"
    "# JarvisNano AMOLED-1.75: emote grown 3M->6M->6.875M for native 466x466 art\n"
    "# (reactive face frame bump for smoother loops), storage 4M->5M. ota_1 (4M,\n"
    "# never flashed -- OTA-B was reserved-but-empty) reclaimed. Explicit offsets so\n"
    "# the resized layout is unambiguous. storage now ends exactly at 0x1000000 --\n"
    "# the former 896K top gap is folded into emote. Moving storage wipes the FAT\n"
    "# partition (runtime storage_base_path is the SD card; Wi-Fi/LLM config live\n"
    "# in NVS and survive) -- flash with STORAGE=1 after changing this table.\n"
    "nvs,       data, nvs,     0x9000,   0x6000\n"
    "otadata,   data, ota,     0xF000,   0x2000\n"
    "phy_init,  data, phy,     0x11000,  0x1000\n"
    "ota_0,     app,  ota_0,   0x20000,  0x400000\n"
    "emote,     data, spiffs,  0x420000, 0x6E0000\n"
    "storage,   data, fat,     0xB00000, 0x500000\n"
)
# Sanity: only replace layouts we know — the upstream pre-resize table (has the
# auto-offset ota_1 line / 3M emote) or our previous 6M-emote resize. Anything
# else means an unexpected upstream change: abort rather than clobber.
s = csv.read_text()
known_pre = "ota_1" in s or "0x300000" in s or "   3M" in s
known_6m = "0x600000" in s and "0xA20000" in s
if not (known_pre or known_6m):
    raise SystemExit("partitions_16MB.csv is not a known layout — aborting")
csv.write_text(new)
print("rewrote partitions_16MB.csv: emote 0x420000+0x6E0000, storage 0xB00000+5M")
PY
}

apply_emote_performance_patch() {
    local emote_c="$ESP_CLAW_DIR/components/common/emote/emote.c"
    [ -f "$emote_c" ] || return 0
    python3 - "$emote_c" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
replacements = {
    ".double_buffer = false,": ".double_buffer = true,",
    ".fps = 10,": ".fps = 30,",
    ".buf_pixels = (size_t)s_lcd_width * 16,": ".buf_pixels = (size_t)s_lcd_width * EMOTE_FLUSH_STRIP_ROWS,",
    ".buf_pixels = (size_t)s_lcd_width * (s_lcd_height / 4),": ".buf_pixels = (size_t)s_lcd_width * EMOTE_FLUSH_STRIP_ROWS,",
    ".buf_pixels = (size_t)s_lcd_width * 48,": ".buf_pixels = (size_t)s_lcd_width * EMOTE_FLUSH_STRIP_ROWS,",
}
for old, new in replacements.items():
    s = s.replace(old, new)
if "            .buff_dma = true,\n" in s and "            .buff_spiram = true,\n" not in s:
    pass
s = s.replace("            .buff_spiram = true,\n", "")
p.write_text(s)
print("emote performance patch applied")
PY
}

apply_reactive_waveform_face_patch() {
    # patches/0031 — reactive "Siri-style" waveform face + `face` CLI demo.
    # Runs AFTER apply_emote_voice_states_patch (anchors on the voice-state decls
    # it adds to emote.h). waveform.c is vendored under firmware/emote/ (same
    # idiom as copy_cap_gemini_live) and copied into the emote component; the 4
    # existing files get idempotent guarded edits.
    local emote_dir="$ESP_CLAW_DIR/components/common/emote"
    local emote_h="$emote_dir/include/emote.h"
    local emote_c="$emote_dir/emote.c"
    local cmake="$emote_dir/CMakeLists.txt"
    local cli="$ESP_CLAW_DIR/components/common/app_claw/app_claw_cli.c"
    # patch 0034: the reactive face is now BAKED-EAF (reactive_face.c) — the old
    # runtime-buffer waveform.c spiraled on the CO5300 panel. Source-of-truth is
    # firmware/emote/reactive_face.c; it loads firmware/mascot/reactive/*.eaf
    # (mounted into the emote partition by apply_emote_voice_states_patch).
    local wf_src="$ROOT/firmware/emote/reactive_face.c"
    if [ ! -d "$emote_dir" ] || [ ! -f "$emote_h" ]; then
        log "emote component not found — skipping reactive face patch"
        return
    fi
    [ -f "$wf_src" ] || die "missing vendored reactive_face.c at $wf_src"
    # Always re-copy the vendored renderer so source-of-truth edits propagate even
    # on an incremental tree where the code-guard below would short-circuit. Drop
    # the stale runtime waveform.c if a previous bootstrap left it behind.
    cp -f "$wf_src" "$emote_dir/reactive_face.c"
    rm -f "$emote_dir/waveform.c" "$emote_dir/reactive_face.h"
    # CMakeLists: converge SRCS to reactive_face.c (handles old waveform.c trees)
    # and add the esp_partition REQUIRES (reactive_face.c esp_partition_read's the
    # EAF clips out of flash when assets are NOT memory-mapped — XIP-from-PSRAM).
    python3 - "$cmake" <<'PYCM'
import sys, pathlib
cmake = pathlib.Path(sys.argv[1])
m = cmake.read_text()
if '"reactive_face.c"' not in m:
    if '"waveform.c"' in m:
        m = m.replace('"waveform.c"', '"reactive_face.c"', 1)
    else:
        m = m.replace('        "emote.c"\n', '        "emote.c"\n        "reactive_face.c"\n', 1)
    print("CMakeLists SRCS -> reactive_face.c")
if 'esp_partition' not in m:
    m = m.replace('        espressif2022__esp_emote_gfx\n',
                  '        espressif2022__esp_emote_gfx\n        esp_partition\n', 1)
    print("CMakeLists REQUIRES += esp_partition")
cmake.write_text(m)
PYCM
    if grep -q "emote_face_set_state" "$emote_h" 2>/dev/null; then
        # emote.h API already present from a prior run; ensure the voice helpers
        # still drive the reactive face (idempotent re-apply of the 0034 wiring).
        apply_reactive_face_voice_wiring "$emote_c"
        log "reactive face patch already applied (reactive_face.c + CMake refreshed)"
        return
    fi
    log "applying patches/0031+0034-reactive-face.patch"

    python3 - <<PY
import pathlib
emote_h = pathlib.Path(r"$emote_h")
emote_c = pathlib.Path(r"$emote_c")
cmake   = pathlib.Path(r"$cmake")
cli     = pathlib.Path(r"$cli")

# --- emote.h: stdint include + face API after the voice-state decls ---
h = emote_h.read_text()
if "#include <stdint.h>" not in h:
    h = h.replace("#include <stdbool.h>\n", "#include <stdbool.h>\n#include <stdint.h>\n", 1)
anchor = "esp_err_t emote_set_voice_idle(void);\n"
face_api = (
    "\n"
    "/* ---- Reactive waveform face (the primary \"Siri-style\" visual) ------------- */\n"
    "typedef enum {\n"
    "    EMOTE_FACE_OFF = 0,\n"
    "    EMOTE_FACE_IDLE,\n"
    "    EMOTE_FACE_LISTENING,\n"
    "    EMOTE_FACE_THINKING,\n"
    "    EMOTE_FACE_SPEAKING,\n"
    "} emote_face_state_t;\n"
    "\n"
    "esp_err_t emote_face_set_state(emote_face_state_t state);\n"
    "void emote_face_set_synthetic_amplitude(int amp_milli);\n"
    "typedef uint16_t (*emote_face_amp_cb_t)(emote_face_state_t state);\n"
    "void emote_face_set_amplitude_source(emote_face_amp_cb_t cb);\n"
)
if anchor not in h:
    raise SystemExit("emote.h voice-idle anchor missing — apply 0012 first")
if "emote_face_set_state" not in h:
    h = h.replace(anchor, anchor + face_api, 1)
emote_h.write_text(h)

# --- emote.c: forward-decl + call emote_face_init after assets load ---
c = emote_c.read_text()
decl_anchor = '#define EMOTE_ASSETS_PARTITION "emote"\n'
decl = (
    '#define EMOTE_ASSETS_PARTITION "emote"\n'
    "\n"
    "/* Reactive waveform face — defined in reactive_face.c (same component). */\n"
    "esp_err_t emote_face_init(emote_handle_t emote);\n"
)
if "emote_face_init" not in c:
    if decl_anchor not in c:
        raise SystemExit("emote.c partition-define anchor missing")
    c = c.replace(decl_anchor, decl, 1)
    call_anchor = "    return emote_set_network_status(false, NULL);"
    call = (
        "    esp_err_t face_err = emote_face_init(s_emote_handle);\n"
        "    if (face_err != ESP_OK) {\n"
        '        ESP_LOGW(TAG, "waveform face init failed: %s (idle face still works)", esp_err_to_name(face_err));\n'
        "    }\n"
        "\n"
        "    return emote_set_network_status(false, NULL);"
    )
    if call_anchor not in c:
        raise SystemExit("emote.c network-status return anchor missing")
    c = c.replace(call_anchor, call, 1)
    emote_c.write_text(c)

# --- CMakeLists SRCS handled in shell (reactive_face.c) before this heredoc. ---

# --- app_claw_cli.c: emote.h include + cmd_face + registration (emote-gated) ---
cl = cli.read_text()
if "cmd_face" not in cl:
    inc_anchor = '#include "esp_log.h"\n'
    inc = (
        '#include "esp_log.h"\n'
        "\n"
        "#if CONFIG_APP_CLAW_ENABLE_EMOTE\n"
        '#include "emote.h"\n'
        "#endif\n"
    )
    cl = cl.replace(inc_anchor, inc, 1)

    fn_anchor = "static char *join_prompt_args(int argc, char **argv)\n"
    fn = (
        "#if CONFIG_APP_CLAW_ENABLE_EMOTE\n"
        "static int cmd_face(int argc, char **argv)\n"
        "{\n"
        "    if (argc < 2) {\n"
        '        printf("Usage: face <off|idle|listen|think|speak> [amplitude_pct 0-100]\\n");\n'
        "        return 1;\n"
        "    }\n"
        "    const char *s = argv[1];\n"
        "    emote_face_state_t st;\n"
        "    bool reactive = false;\n"
        '    if (strcmp(s, "off") == 0) { st = EMOTE_FACE_OFF; }\n'
        '    else if (strcmp(s, "idle") == 0) { st = EMOTE_FACE_IDLE; }\n'
        '    else if (strcmp(s, "listen") == 0) { st = EMOTE_FACE_LISTENING; reactive = true; }\n'
        '    else if (strcmp(s, "think") == 0) { st = EMOTE_FACE_THINKING; }\n'
        '    else if (strcmp(s, "speak") == 0) { st = EMOTE_FACE_SPEAKING; reactive = true; }\n'
        "    else {\n"
        '        printf("Unknown state \'%s\'. Use off|idle|listen|think|speak.\\n", s);\n'
        "        return 1;\n"
        "    }\n"
        "    if (reactive) {\n"
        "        if (argc >= 3) {\n"
        "            int pct = atoi(argv[2]);\n"
        "            if (pct < 0) pct = 0;\n"
        "            if (pct > 100) pct = 100;\n"
        "            emote_face_set_synthetic_amplitude(pct * 10);\n"
        '            printf("face %s @ %d%% (fixed synthetic amplitude)\\n", s, pct);\n'
        "        } else {\n"
        "            emote_face_set_synthetic_amplitude(-1);\n"
        '            printf("face %s (live audio if available, else synthetic sweep)\\n", s);\n'
        "        }\n"
        "    } else {\n"
        '        printf("face %s\\n", s);\n'
        "    }\n"
        "    emote_face_set_state(st);\n"
        "    return 0;\n"
        "}\n"
        "#endif /* CONFIG_APP_CLAW_ENABLE_EMOTE */\n"
        "\n"
        "static char *join_prompt_args(int argc, char **argv)\n"
    )
    if fn_anchor not in cl:
        raise SystemExit("app_claw_cli.c join_prompt_args anchor missing")
    cl = cl.replace(fn_anchor, fn, 1)

    reg_anchor = '    printf("Type \'help\', \'auto rules\','
    reg = (
        "#if CONFIG_APP_CLAW_ENABLE_EMOTE\n"
        "    {\n"
        "        esp_console_cmd_t face_cmd = {\n"
        '            .command = "face",\n'
        '            .help = "Reactive waveform face demo: face <off|idle|listen|think|speak> [amplitude_pct]",\n'
        "            .func = cmd_face,\n"
        "        };\n"
        "        ESP_ERROR_CHECK(esp_console_cmd_register(&face_cmd));\n"
        "    }\n"
        "#endif\n"
        "\n"
        '    printf("Type \'help\', \'auto rules\','
    )
    if reg_anchor not in cl:
        raise SystemExit("app_claw_cli.c registration anchor missing")
    cl = cl.replace(reg_anchor, reg, 1)
    cli.write_text(cl)

print("applied reactive face: reactive_face.c + emote.h/.c + CMake + cli")
PY
    # Rewire the voice helpers (added by apply_emote_voice_states_patch) so the
    # reactive face is the PRIMARY visual during a voice session.
    apply_reactive_face_voice_wiring "$emote_c"
}

apply_reactive_face_voice_wiring() {
    # Idempotent: make voice-state helpers non-blocking. The Gemini session task
    # must not call emote_apply(); that can wait on the render lock while the
    # display is flushing and wedge voice startup in CONNECTING.
    local emote_c="$1"
    [ -f "$emote_c" ] || return 0
    grep -q "emote_set_listening" "$emote_c" 2>/dev/null || return 0   # voice helpers not yet added
    python3 - "$emote_c" <<'PYW'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); c = p.read_text()
repl = {
    'esp_err_t emote_set_connecting(void)\n{\n    return emote_apply("thinking", "Connecting...");\n}':
        'esp_err_t emote_set_connecting(void)\n{\n    s_voice_visual_active = true;\n    emote_face_set_state(EMOTE_FACE_THINKING);\n    return ESP_OK;\n}',
    'esp_err_t emote_set_listening(void)\n{\n    return emote_apply("listen", "Listening...");\n}':
        'esp_err_t emote_set_listening(void)\n{\n    s_voice_visual_active = true;\n    emote_face_set_state(EMOTE_FACE_LISTENING);\n    return ESP_OK;\n}',
    'esp_err_t emote_set_thinking(void)\n{\n    return emote_apply("thinking", "Thinking...");\n}':
        'esp_err_t emote_set_thinking(void)\n{\n    s_voice_visual_active = true;\n    emote_face_set_state(EMOTE_FACE_THINKING);\n    return ESP_OK;\n}',
    'esp_err_t emote_set_speaking(void)\n{\n    return emote_apply("happy", "Speaking...");\n}':
        'esp_err_t emote_set_speaking(void)\n{\n    s_voice_visual_active = true;\n    emote_face_set_state(EMOTE_FACE_SPEAKING);\n    return ESP_OK;\n}',
    'esp_err_t emote_set_connecting(void)\n{\n    s_voice_visual_active = true;\n    esp_err_t err = emote_apply("thinking", "Connecting...");\n    emote_face_set_state(EMOTE_FACE_THINKING);\n    return err;\n}':
        'esp_err_t emote_set_connecting(void)\n{\n    s_voice_visual_active = true;\n    emote_face_set_state(EMOTE_FACE_THINKING);\n    return ESP_OK;\n}',
    'esp_err_t emote_set_listening(void)\n{\n    s_voice_visual_active = true;\n    esp_err_t err = emote_apply("listen", "Listening...");\n    emote_face_set_state(EMOTE_FACE_LISTENING);\n    return err;\n}':
        'esp_err_t emote_set_listening(void)\n{\n    s_voice_visual_active = true;\n    emote_face_set_state(EMOTE_FACE_LISTENING);\n    return ESP_OK;\n}',
    'esp_err_t emote_set_thinking(void)\n{\n    s_voice_visual_active = true;\n    esp_err_t err = emote_apply("thinking", "Thinking...");\n    emote_face_set_state(EMOTE_FACE_THINKING);\n    return err;\n}':
        'esp_err_t emote_set_thinking(void)\n{\n    s_voice_visual_active = true;\n    emote_face_set_state(EMOTE_FACE_THINKING);\n    return ESP_OK;\n}',
    'esp_err_t emote_set_speaking(void)\n{\n    s_voice_visual_active = true;\n    esp_err_t err = emote_apply("happy", "Speaking...");\n    emote_face_set_state(EMOTE_FACE_SPEAKING);\n    return err;\n}':
        'esp_err_t emote_set_speaking(void)\n{\n    s_voice_visual_active = true;\n    emote_face_set_state(EMOTE_FACE_SPEAKING);\n    return ESP_OK;\n}',
    'esp_err_t emote_set_voice_idle(void)\n{\n    return emote_render_status();\n}':
        'esp_err_t emote_set_voice_idle(void)\n{\n    s_voice_visual_active = false;\n    return emote_render_status();\n}',
    'esp_err_t emote_set_voice_idle(void)\n{\n    emote_face_set_state(EMOTE_FACE_OFF);   /* restore the eye + status idle screen */\n    return emote_render_status();\n}':
        'esp_err_t emote_set_voice_idle(void)\n{\n    s_voice_visual_active = false;\n    return emote_render_status();\n}',
}
n = 0
for old, new in repl.items():
    if old in c:
        c = c.replace(old, new, 1); n += 1
status_old = "    return emote_apply(idle, msg);\n"
status_new = "    esp_err_t err = emote_apply(idle, msg);\n    emote_face_set_state(EMOTE_FACE_IDLE);\n    return err;\n"
if status_old in c:
    c = c.replace(status_old, status_new, 1); n += 1
p.write_text(c)
print(f"reactive face voice wiring: {n} replacements")
PYW
}

apply_reactive_waveform_audio_bridge_patch() {
    # patches/0032 — wire the reactive waveform face to cap_gemini_live's live
    # audio levels. Runs AFTER apply_reactive_waveform_face_patch (needs the
    # emote_face_* API) and copy_cap_gemini_live (needs the level getters).
    # Bridge .c/.h vendored under firmware/emote/ (same idiom as 0031).
    local app_dir="$ESP_CLAW_DIR/components/common/app_claw"
    local app_c="$app_dir/app_claw.c"
    local cmake="$app_dir/CMakeLists.txt"
    local br_c="$ROOT/firmware/emote/app_claw_face_bridge.c"
    local br_h="$ROOT/firmware/emote/app_claw_face_bridge.h"
    if [ ! -f "$app_c" ]; then
        log "app_claw.c not found — skipping reactive waveform audio bridge patch"
        return
    fi
    if grep -q "app_claw_face_bridge" "$app_c" 2>/dev/null; then
        log "reactive waveform audio bridge patch already applied"
        return
    fi
    [ -f "$br_c" ] && [ -f "$br_h" ] || die "missing vendored bridge files at $br_c / $br_h"
    log "applying patches/0032-reactive-waveform-audio-bridge.patch"

    cp -f "$br_c" "$app_dir/app_claw_face_bridge.c"
    cp -f "$br_h" "$app_dir/app_claw_face_bridge.h"

    python3 - <<PY
import pathlib
app_c = pathlib.Path(r"$app_c")
cmake = pathlib.Path(r"$cmake")

# app_claw.c: include the bridge header next to emote.h, and register after emote_start
a = app_c.read_text()
inc_anchor = (
    "#if CONFIG_APP_CLAW_ENABLE_EMOTE\n"
    '#include "emote.h"\n'
    "#endif\n"
)
inc_new = (
    "#if CONFIG_APP_CLAW_ENABLE_EMOTE\n"
    '#include "emote.h"\n'
    '#include "app_claw_face_bridge.h"\n'
    "#endif\n"
)
if "app_claw_face_bridge.h" not in a:
    if inc_anchor not in a:
        raise SystemExit("app_claw.c emote include block anchor missing")
    a = a.replace(inc_anchor, inc_new, 1)

start_anchor = (
    "#if defined(CONFIG_APP_CLAW_ENABLE_EMOTE)\n"
    "    return emote_start();\n"
    "#else\n"
    "    return ESP_OK;\n"
    "#endif\n"
)
start_new = (
    "#if defined(CONFIG_APP_CLAW_ENABLE_EMOTE)\n"
    "    esp_err_t err = emote_start();\n"
    "    if (err == ESP_OK) {\n"
    "        app_claw_face_bridge_register();\n"
    "    }\n"
    "    return err;\n"
    "#else\n"
    "    return ESP_OK;\n"
    "#endif\n"
)
if "app_claw_face_bridge_register" not in a:
    if start_anchor not in a:
        raise SystemExit("app_claw.c emote_start return anchor missing")
    a = a.replace(start_anchor, start_new, 1)
app_c.write_text(a)

# CMakeLists: add the bridge .c to SRCS, emote-gated
m = cmake.read_text()
if "app_claw_face_bridge.c" not in m:
    anchor = (
        'set(app_claw_srcs\n'
        '    "app_claw.c"\n'
        '    "app_capabilities.c"\n'
        '    "app_lua_modules.c"\n'
        ")\n"
    )
    add = (
        anchor
        + "\n"
        + "if(CONFIG_APP_CLAW_ENABLE_EMOTE)\n"
        + '    list(APPEND app_claw_srcs "app_claw_face_bridge.c")\n'
        + "endif()\n"
    )
    if anchor not in m:
        raise SystemExit("app_claw CMakeLists app_claw_srcs anchor missing")
    m = m.replace(anchor, add, 1)
    cmake.write_text(m)

print("applied reactive waveform audio bridge: face_bridge.c/.h + app_claw.c + CMake")
PY
}

main() {
    mkdir -p "$ROOT/.build_logs"
    clone_or_update_esp_claw
    copy_board
    copy_firmware_assets
    copy_cap_gemini_live
    copy_jarvis_logger
    copy_jarvis_brain
    copy_jarvis_pmic
    copy_jarvis_imu
    apply_patch
    apply_wifi_ps_patch
    apply_jpeg_soi_patch
    apply_http_phase2_patch
    apply_http_camera_gate_patch
    apply_gemini_http_diagnostics_patch
    apply_gemini_http_action_response_patch
    apply_gemini_live_main_require_patch
    apply_gemini_live_api_key_patch
    apply_gemini_live_network_ready_patch
    apply_gemini_live_nonblocking_stop_patch
    apply_http_wifi_scan_patch
    apply_http_health_patch
    copy_http_display_diagnostics
    apply_battery_real_read_patch
    apply_imu_read_patch
    apply_native_status_led_patch
    apply_emote_status_detail_patch
    apply_touch_handle_deref_patch
    apply_touch_local_hardware_demo_patch
    apply_emote_voice_states_patch
    apply_emote_partition_resize_patch
    apply_reactive_waveform_face_patch
    copy_emote_runtime
    apply_emote_performance_patch
    apply_reactive_waveform_audio_bridge_patch
    apply_ble_disable_patch

    if [ "${1:-}" = "build" ]; then
        build
    else
        log "ready. run \`./scripts/bootstrap.sh build\` to compile in Docker"
    fi
}

# BLE GATT companion service off: NimBLE keeps the BT controller active and
# Wi-Fi/BT software coexistence time-slices the single 2.4 GHz radio — measured
# as "apply reconnect coex policy" Wi-Fi failures and intermittent Gemini Live
# audio stalls. Voice path is Wi-Fi only; re-enable if a BLE companion ships.
# See docs/reference/audio-es8311-es7210.md (2026-06-10 finding).
apply_ble_disable_patch() {
    local main_c="$ESP_CLAW_DIR/application/edge_agent/main/main.c"
    if [ ! -f "$main_c" ]; then
        log "main.c not found — skipping BLE disable patch"
        return
    fi
    if grep -q "BLE GATT service disabled" "$main_c" 2>/dev/null; then
        log "BLE disable patch already applied"
        return
    fi
    log "applying BLE-disable patch (skip ble_gatt_init; avoid Wi-Fi coex audio stalls)"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$main_c")
s = p.read_text()
old = (
    '    if (ble_gatt_init() != ESP_OK) {\n'
    '        ESP_LOGE(TAG, "Failed to initialize BLE GATT service");\n'
    '    }\n'
)
new = (
    '    /* BLE GATT companion service intentionally NOT started. NimBLE keeps the\n'
    '     * BT controller active, and Wi-Fi/BT software coexistence time-slices the\n'
    '     * single 2.4 GHz radio — measured as repeated "apply reconnect coex\n'
    '     * policy" Wi-Fi failures and intermittent Gemini Live audio cutoffs. The\n'
    '     * JarvisNano voice path is Wi-Fi only; nothing pairs over BLE today.\n'
    '     * Re-enable by restoring the ble_gatt_init() call if a companion app\n'
    '     * ships. (Skipping init also keeps the controller SRAM unallocated.) */\n'
    '    ESP_LOGI(TAG, "BLE GATT service disabled (voice path is Wi-Fi only; avoids coex audio stalls)");\n'
)
if old not in s:
    raise SystemExit("could not locate ble_gatt_init block in main.c — upstream may have changed")
p.write_text(s.replace(old, new, 1))
print("patched", p)
PY
}

main "$@"
