#!/usr/bin/env bash
# bootstrap.sh — clone esp-claw, drop in our board, apply codegen patch, optionally build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_CLAW_DIR="$ROOT/esp-claw"
ESP_CLAW_REPO="https://github.com/espressif/esp-claw.git"
ESP_CLAW_REF="${ESP_CLAW_REF:-6a211756a6ebf8d725173e294f582a6cf30c9592}"
BOARD_NAME="xiao_esp32s3_sense"
BOARD_VENDOR="seeed"
IDF_IMAGE="espressif/idf:release-v5.5"

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

apply_patch() {
    local target="$ESP_CLAW_DIR/application/edge_agent/managed_components/espressif__esp_board_manager/peripherals/periph_i2s/periph_i2s.py"
    if [ ! -f "$target" ]; then
        log "esp_board_manager not pulled yet — running idf.py reconfigure inside Docker to populate managed_components"
        docker run --rm -v "$ESP_CLAW_DIR":/project -w /project/application/edge_agent "$IDF_IMAGE" \
            bash -lc 'pip install --quiet esp-bmgr-assist && idf.py set-target esp32s3 && idf.py reconfigure' \
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
    log "building inside $IDF_IMAGE (output streams to .build_logs/build.log)"
    docker run --rm -v "$ESP_CLAW_DIR":/project -w /project/application/edge_agent "$IDF_IMAGE" \
        bash -lc 'set -e; pip install --quiet esp-bmgr-assist;
                  idf.py set-target esp32s3;
                  idf.py gen-bmgr-config -c ./boards -b xiao_esp32s3_sense;
                  python3 - <<'"'"'PY'"'"'
from pathlib import Path
p = Path("sdkconfig")
s = p.read_text()
s = s.replace("CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y", "# CONFIG_ESPTOOLPY_FLASHSIZE_2MB is not set")
s = s.replace("# CONFIG_ESPTOOLPY_FLASHSIZE_8MB is not set", "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y")
s = s.replace("CONFIG_ESPTOOLPY_FLASHSIZE=\"2MB\"", "CONFIG_ESPTOOLPY_FLASHSIZE=\"8MB\"")
# The XIAO ESP32-S3 Sense has enough flash for the app image, but not enough
# free internal heap to register the full serial REPL after Wi-Fi, MCP, Lua,
# router, scheduler, and camera startup. Keep USB logs, skip the interactive CLI.
s = s.replace("CONFIG_APP_CLAW_ENABLE_CLI=y", "# CONFIG_APP_CLAW_ENABLE_CLI is not set")
p.write_text(s)
PY
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

apply_http_phase2_patch() {
    local core="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_core.c"
    local status="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_status_api.c"
    if [ ! -f "$core" ] || [ ! -f "$status" ]; then
        log "http_server sources not found — skipping Phase 2 HTTP patch"
        return
    fi
    if grep -q "Failed to register API OPTIONS route" "$core" 2>/dev/null &&
       ! grep -q "allow JSON clients to POST /api/\\*" "$core" 2>/dev/null &&
       grep -q '"not_wired"' "$status" 2>/dev/null; then
        log "Phase 2 HTTP patch already applied"
        return
    fi
    log "applying patches/0004-http-phase2-preflight-battery.patch"
    python3 - <<PY
import pathlib

core = pathlib.Path(r"$core")
status = pathlib.Path(r"$status")

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
    "    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, \"Failed to start HTTP server\");\n"
    "    ESP_RETURN_ON_ERROR(http_server_register_assets_routes(s_ctx.server), TAG, \"Failed to register assets routes\");\n"
)
new = (
    "    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, \"Failed to start HTTP server\");\n"
    "    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_ctx.server, &(httpd_uri_t) {\n"
    "                            .uri = \"/api/*\",\n"
    "                            .method = HTTP_OPTIONS,\n"
    "                            .handler = api_options_handler,\n"
    "                        }), TAG, \"Failed to register API OPTIONS route\");\n"
    "    ESP_RETURN_ON_ERROR(http_server_register_assets_routes(s_ctx.server), TAG, \"Failed to register assets routes\");\n"
)
if "Failed to register API OPTIONS route" not in s:
    if old not in s:
        raise SystemExit(f"could not locate core route registration point in {core}")
    s = s.replace(old, new, 1)
core.write_text(s)

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

apply_http_camera_gate_patch() {
    local cmake="$ESP_CLAW_DIR/application/edge_agent/components/http_server/CMakeLists.txt"
    local camera_api="$ESP_CLAW_DIR/application/edge_agent/components/http_server/http_server_camera_api.c"
    if [ ! -f "$cmake" ] || [ ! -f "$camera_api" ]; then
        log "http camera route sources not found — skipping camera gate patch"
        return
    fi
    if grep -q "CONFIG_APP_CLAW_LUA_MODULE_CAMERA" "$cmake" 2>/dev/null &&
       grep -q "CONFIG_APP_CLAW_CAP_LUA && CONFIG_APP_CLAW_LUA_MODULE_CAMERA" "$camera_api" 2>/dev/null; then
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
camera_api.write_text(s)
print("patched", cmake)
print("patched", camera_api)
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

main() {
    mkdir -p "$ROOT/.build_logs"
    clone_or_update_esp_claw
    copy_board
    copy_firmware_assets
    apply_patch
    apply_wifi_ps_patch
    apply_jpeg_soi_patch
    apply_http_phase2_patch
    apply_http_camera_gate_patch
    apply_http_wifi_scan_patch
    apply_native_status_led_patch

    if [ "${1:-}" = "build" ]; then
        build
    else
        log "ready. run \`./scripts/bootstrap.sh build\` to compile in Docker"
    fi
}

main "$@"
