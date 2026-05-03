#!/usr/bin/env bash
# bootstrap.sh — clone esp-claw, drop in our board, apply codegen patch, optionally build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_CLAW_DIR="$ROOT/esp-claw"
ESP_CLAW_REPO="https://github.com/espressif/esp-claw.git"
BOARD_NAME="xiao_esp32s3_sense"
BOARD_VENDOR="seeed"
IDF_IMAGE="espressif/idf:release-v5.5"

log() { printf '\033[1;36m[bootstrap]\033[0m %s\n' "$*"; }

clone_or_update_esp_claw() {
    if [ -d "$ESP_CLAW_DIR/.git" ]; then
        log "esp-claw already cloned at $ESP_CLAW_DIR"
    else
        log "cloning esp-claw → $ESP_CLAW_DIR"
        git clone --depth 1 "$ESP_CLAW_REPO" "$ESP_CLAW_DIR"
    fi
}

copy_board() {
    local src="$ROOT/boards/$BOARD_VENDOR/$BOARD_NAME"
    local dst="$ESP_CLAW_DIR/application/edge_agent/boards/$BOARD_VENDOR/$BOARD_NAME"
    log "copying board $BOARD_VENDOR/$BOARD_NAME → upstream tree"
    mkdir -p "$dst"
    cp -f "$src"/* "$dst/"
}

apply_patch() {
    local target="$ESP_CLAW_DIR/application/edge_agent/managed_components/espressif__esp_board_manager/peripherals/periph_i2s/periph_i2s.py"
    if [ ! -f "$target" ]; then
        log "esp_board_manager not pulled yet — running idf.py reconfigure inside Docker to populate managed_components"
        docker run --rm -v "$ESP_CLAW_DIR":/project -w /project/application/edge_agent "$IDF_IMAGE" \
            bash -lc 'pip install --quiet esp-bmgr-assist && idf.py set-target esp32s3 && idf.py reconfigure' \
            > "$ROOT/.build_logs/reconfigure.log" 2>&1 || true
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
    log "building inside $IDF_IMAGE (output streams to .build_logs/build.log)"
    docker run --rm -v "$ESP_CLAW_DIR":/project -w /project/application/edge_agent "$IDF_IMAGE" \
        bash -lc 'set -e; pip install --quiet esp-bmgr-assist;
                  idf.py set-target esp32s3;
                  idf.py gen-bmgr-config -c ./boards -b xiao_esp32s3_sense;
                  idf.py build' \
        | tee "$ROOT/.build_logs/build.log"
    log "✓ build complete → $ESP_CLAW_DIR/application/edge_agent/build/edge_agent.bin"
}

apply_wifi_ps_patch() {
    local target="$ESP_CLAW_DIR/components/common/wifi_manager/wifi_manager.c"
    if [ ! -f "$target" ]; then
        log "wifi_manager.c not found at $target — skipping wifi PS patch"
        return
    fi
    if grep -q "Disable Wi-Fi modem-sleep AFTER association" "$target" 2>/dev/null; then
        log "wifi PS patch already applied"
        return
    fi
    log "applying patches/0002-wifi-disable-modem-sleep.patch"
    python3 - <<PY
import pathlib
p = pathlib.Path(r"$target")
s = p.read_text()
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

main() {
    mkdir -p "$ROOT/.build_logs"
    clone_or_update_esp_claw
    copy_board
    apply_patch
    apply_wifi_ps_patch
    apply_jpeg_soi_patch

    if [ "${1:-}" = "build" ]; then
        build
    else
        log "ready. run \`./scripts/bootstrap.sh build\` to compile in Docker"
    fi
}

main "$@"
