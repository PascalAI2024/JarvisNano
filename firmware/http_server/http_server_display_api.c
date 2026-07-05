/*
 * GET /api/display/snapshot.ppm
 *
 * Streams the active display owner's software mirror as a binary PPM image.
 * This is NOT CO5300 panel readback; the QSPI display path has no reliable
 * readback primitive in v1. The response identifies whether it came from the
 * UI framebuffer or the emote mirror so diagnostics do not confuse "framebuffer
 * exists" with "panel read back." Add ?save=1 to also write emote captures to
 * /sdcard/diagnostics/display-<frame>.ppm.
 */
#include "http_server_priv.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#if CONFIG_APP_CLAW_ENABLE_EMOTE
#include "emote.h"

/* ui_layer/display_hal live capture. http_server links these through bootstrap's
 * component graph; keep forward decls here to avoid a hard include-path
 * dependency from this component. */
bool ui_layer_has_scene(void);
const uint16_t *display_hal_visible_ptr(int *out_w, int *out_h);
typedef enum {
    DISPLAY_ARBITER_OWNER_NONE = 0,
    DISPLAY_ARBITER_OWNER_LUA,
    DISPLAY_ARBITER_OWNER_EMOTE,
} display_arbiter_owner_t;
display_arbiter_owner_t display_arbiter_get_owner(void);
#endif

static const char *TAG = "http_display";

#if CONFIG_APP_CLAW_ENABLE_EMOTE
static const char *display_owner_name(void)
{
    switch (display_arbiter_get_owner()) {
    case DISPLAY_ARBITER_OWNER_LUA: return "ui";
    case DISPLAY_ARBITER_OWNER_EMOTE: return "emote";
    case DISPLAY_ARBITER_OWNER_NONE:
    default:
        return "none";
    }
}
#endif

static bool display_query_save_requested(httpd_req_t *req)
{
    char value[16];
    if (http_server_query_get(req, "save", value, sizeof(value)) == ESP_OK) {
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0;
    }
    if (http_server_query_get(req, "sd", value, sizeof(value)) == ESP_OK) {
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0;
    }
    return false;
}

static void rgb565_to_rgb888(uint16_t px, uint8_t *out)
{
    if (px == 0xA210) {
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        return;
    }
    px = (uint16_t)((px << 8) | (px >> 8));
    uint8_t r = (uint8_t)((px >> 11) & 0x1F);
    uint8_t g = (uint8_t)((px >> 5) & 0x3F);
    uint8_t b = (uint8_t)(px & 0x1F);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

static void rgb565_native_to_rgb888(uint16_t px, uint8_t *out)
{
    uint8_t r = (uint8_t)((px >> 11) & 0x1F);
    uint8_t g = (uint8_t)((px >> 5) & 0x3F);
    uint8_t b = (uint8_t)(px & 0x1F);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

static esp_err_t display_send_unavailable(httpd_req_t *req, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "available", false);
    http_server_json_add_string(root, "state", "not_available");
    http_server_json_add_string(root, "reason", reason);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return http_server_send_json_response(req, root);
}

static esp_err_t display_snapshot_get_handler(httpd_req_t *req)
{
#if !CONFIG_APP_CLAW_ENABLE_EMOTE
    return display_send_unavailable(req, "emote is not enabled in this build");
#else
    if (ui_layer_has_scene()) {
        int w = 0;
        int h = 0;
        const uint16_t *fb = display_hal_visible_ptr(&w, &h);
        if (!fb || w <= 0 || h <= 0) {
            ESP_LOGW(TAG, "display snapshot UI scene active but framebuffer is unavailable");
            return display_send_unavailable(req, "UI framebuffer is not ready");
        }

        uint8_t *row = (uint8_t *)heap_caps_malloc((size_t)w * 3, MALLOC_CAP_DEFAULT);
        if (!row) {
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }

        char header[96];
        int header_len = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", w, h);

        httpd_resp_set_type(req, "image/x-portable-pixmap");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "X-Jarvis-Display-Source", "ui_framebuffer");
        httpd_resp_set_hdr(req, "X-Jarvis-Display-Owner", display_owner_name());
        httpd_resp_set_hdr(req, "X-Jarvis-Panel-Readback", "false");
        httpd_resp_set_hdr(req, "X-Jarvis-Display-Fresh", "true");

        char width_value[16];
        char height_value[16];
        snprintf(width_value, sizeof(width_value), "%d", w);
        snprintf(height_value, sizeof(height_value), "%d", h);
        httpd_resp_set_hdr(req, "X-Jarvis-Display-Width", width_value);
        httpd_resp_set_hdr(req, "X-Jarvis-Display-Height", height_value);

        ESP_LOGI(TAG, "display snapshot stream source=ui_framebuffer owner=%s readback=false %dx%d",
                 display_owner_name(), w, h);
        esp_err_t err = httpd_resp_send_chunk(req, header, (ssize_t)header_len);
        for (int y = 0; y < h && err == ESP_OK; y++) {
            const uint16_t *src = fb + ((size_t)y * (size_t)w);
            for (int x = 0; x < w; x++) {
                rgb565_native_to_rgb888(src[x], row + ((size_t)x * 3));
            }
            err = httpd_resp_send_chunk(req, (const char *)row, (ssize_t)((size_t)w * 3));
        }
        heap_caps_free(row);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UI display snapshot stream failed: %s", esp_err_to_name(err));
            httpd_resp_send_chunk(req, NULL, 0);
            return err;
        }
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    emote_display_snapshot_info_t info = {0};
    esp_err_t err = emote_display_snapshot_get_info(&info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display snapshot unavailable: %s", esp_err_to_name(err));
        return display_send_unavailable(req, "display snapshot mirror is not ready");
    }

    uint16_t *rgb565 = (uint16_t *)heap_caps_malloc(info.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565) {
        rgb565 = (uint16_t *)heap_caps_malloc(info.bytes, MALLOC_CAP_DEFAULT);
    }
    if (!rgb565) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    err = emote_display_snapshot_copy_rgb565(rgb565, info.bytes, &info);
    if (err != ESP_OK) {
        heap_caps_free(rgb565);
        ESP_LOGW(TAG, "display snapshot copy failed: %s", esp_err_to_name(err));
        return display_send_unavailable(req, "display snapshot copy failed");
    }

    bool save = display_query_save_requested(req);
    FILE *save_file = NULL;
    char save_path[HTTP_SERVER_PATH_MAX] = {0};
    if (save) {
        char diag_dir[HTTP_SERVER_PATH_MAX] = {0};
        char relative_save_path[64] = {0};
        int path_len = snprintf(relative_save_path, sizeof(relative_save_path),
                                "/diagnostics/display-%llu.ppm",
                                (unsigned long long)info.frame_id);
        if (http_server_resolve_storage_path("/diagnostics", diag_dir, sizeof(diag_dir)) != ESP_OK ||
            path_len <= 0 || (size_t)path_len >= sizeof(relative_save_path) ||
            http_server_resolve_storage_path(relative_save_path, save_path, sizeof(save_path)) != ESP_OK) {
            ESP_LOGW(TAG, "display snapshot save path is too long");
        } else {
            mkdir(diag_dir, 0775);
            save_file = fopen(save_path, "wb");
            if (!save_file) {
                ESP_LOGW(TAG, "failed to open display snapshot save path: %s", save_path);
                save_path[0] = '\0';
            }
        }
    }

    char header[96];
    int header_len = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                              (unsigned)info.width, (unsigned)info.height);

    httpd_resp_set_type(req, "image/x-portable-pixmap");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Jarvis-Display-Source", "emote_mirror");
    httpd_resp_set_hdr(req, "X-Jarvis-Display-Owner", display_owner_name());
    httpd_resp_set_hdr(req, "X-Jarvis-Panel-Readback", "false");
    httpd_resp_set_hdr(req, "X-Jarvis-Display-Fresh", info.valid ? "true" : "false");

    char width_value[16];
    char height_value[16];
    char frame_value[32];
    snprintf(width_value, sizeof(width_value), "%u", (unsigned)info.width);
    snprintf(height_value, sizeof(height_value), "%u", (unsigned)info.height);
    snprintf(frame_value, sizeof(frame_value), "%llu", (unsigned long long)info.frame_id);
    httpd_resp_set_hdr(req, "X-Jarvis-Display-Width", width_value);
    httpd_resp_set_hdr(req, "X-Jarvis-Display-Height", height_value);
    httpd_resp_set_hdr(req, "X-Jarvis-Display-Frame", frame_value);
    if (save_path[0] != '\0') {
        httpd_resp_set_hdr(req, "X-Jarvis-Saved-Path", save_path);
    }

    ESP_LOGI(TAG, "display snapshot stream source=emote_mirror owner=%s readback=false frame=%llu %ux%u save=%s",
             display_owner_name(),
             (unsigned long long)info.frame_id,
             (unsigned)info.width,
             (unsigned)info.height,
             save_path[0] ? save_path : "no");

    if (save_file) {
        fwrite(header, 1, (size_t)header_len, save_file);
    }
    err = httpd_resp_send_chunk(req, header, (ssize_t)header_len);
    if (err == ESP_OK) {
        uint8_t *row = (uint8_t *)heap_caps_malloc((size_t)info.width * 3, MALLOC_CAP_DEFAULT);
        if (!row) {
            err = ESP_ERR_NO_MEM;
        } else {
            for (uint16_t y = 0; y < info.height && err == ESP_OK; y++) {
                const uint16_t *src = rgb565 + ((size_t)y * (size_t)info.width);
                for (uint16_t x = 0; x < info.width; x++) {
                    rgb565_to_rgb888(src[x], row + ((size_t)x * 3));
                }
                if (save_file) {
                    fwrite(row, 1, (size_t)info.width * 3, save_file);
                }
                err = httpd_resp_send_chunk(req, (const char *)row, (ssize_t)((size_t)info.width * 3));
            }
            heap_caps_free(row);
        }
    }

    if (save_file) {
        fclose(save_file);
        ESP_LOGI(TAG, "display snapshot saved: %s", save_path);
    }
    heap_caps_free(rgb565);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display snapshot stream failed: %s", esp_err_to_name(err));
        httpd_resp_send_chunk(req, NULL, 0);
        return err;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
#endif
}

static esp_err_t display_snapshot_info_handler(httpd_req_t *req)
{
#if !CONFIG_APP_CLAW_ENABLE_EMOTE
    return display_send_unavailable(req, "emote is not enabled in this build");
#else
    if (ui_layer_has_scene()) {
        int w = 0;
        int h = 0;
        const uint16_t *fb = display_hal_visible_ptr(&w, &h);
        if (!fb || w <= 0 || h <= 0) {
            ESP_LOGW(TAG, "display snapshot info UI scene active but framebuffer is unavailable");
            return display_send_unavailable(req, "UI framebuffer is not ready");
        }
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(root, "available", true);
        http_server_json_add_string(root, "source", "ui_framebuffer");
        http_server_json_add_string(root, "capture_source", "ui_framebuffer");
        http_server_json_add_string(root, "display_owner", display_owner_name());
        cJSON_AddBoolToObject(root, "panel_readback", false);
        http_server_json_add_string(root, "note",
                                    "software framebuffer mirror; CO5300 panel readback is not available");
        cJSON_AddNumberToObject(root, "width", w);
        cJSON_AddNumberToObject(root, "height", h);
        cJSON_AddNumberToObject(root, "bytes", (size_t)w * (size_t)h * sizeof(uint16_t));
        cJSON_AddNumberToObject(root, "frame_id", 0);
        cJSON_AddNumberToObject(root, "last_flush_ms", 0);
        cJSON_AddBoolToObject(root, "valid", true);
        cJSON_AddBoolToObject(root, "buffer_valid", true);
        cJSON_AddBoolToObject(root, "mirror_fresh", true);
        return http_server_send_json_response(req, root);
    }

    emote_display_snapshot_info_t info = {0};
    esp_err_t err = emote_display_snapshot_get_info(&info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display snapshot info unavailable: %s", esp_err_to_name(err));
        return display_send_unavailable(req, "display snapshot mirror is not ready");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "available", true);
    http_server_json_add_string(root, "source", "emote_mirror");
    http_server_json_add_string(root, "capture_source", "emote_mirror");
    http_server_json_add_string(root, "display_owner", display_owner_name());
    cJSON_AddBoolToObject(root, "panel_readback", false);
    http_server_json_add_string(root, "note",
                                "software framebuffer mirror; CO5300 panel readback is not available");
    cJSON_AddNumberToObject(root, "width", info.width);
    cJSON_AddNumberToObject(root, "height", info.height);
    cJSON_AddNumberToObject(root, "bytes", info.bytes);
    cJSON_AddNumberToObject(root, "frame_id", info.frame_id);
    cJSON_AddNumberToObject(root, "last_flush_ms", info.last_flush_ms);
    cJSON_AddBoolToObject(root, "valid", true);
    cJSON_AddBoolToObject(root, "buffer_valid", true);
    cJSON_AddBoolToObject(root, "mirror_fresh", info.valid);
    return http_server_send_json_response(req, root);
#endif
}

#if CONFIG_APP_CLAW_ENABLE_EMOTE
static const char *display_face_state_name(emote_face_state_t state)
{
    switch (state) {
    case EMOTE_FACE_OFF: return "off";
    case EMOTE_FACE_IDLE: return "idle";
    case EMOTE_FACE_LISTENING: return "listen";
    case EMOTE_FACE_THINKING: return "think";
    case EMOTE_FACE_SPEAKING: return "speak";
    default: return "unknown";
    }
}

static bool display_parse_face_state(const char *value, emote_face_state_t *out)
{
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "off") == 0) {
        *out = EMOTE_FACE_OFF;
    } else if (strcmp(value, "idle") == 0) {
        *out = EMOTE_FACE_IDLE;
    } else if (strcmp(value, "listen") == 0 || strcmp(value, "listening") == 0) {
        *out = EMOTE_FACE_LISTENING;
    } else if (strcmp(value, "think") == 0 || strcmp(value, "thinking") == 0) {
        *out = EMOTE_FACE_THINKING;
    } else if (strcmp(value, "speak") == 0 || strcmp(value, "speaking") == 0) {
        *out = EMOTE_FACE_SPEAKING;
    } else {
        return false;
    }
    return true;
}
#endif

static esp_err_t display_face_control_handler(httpd_req_t *req)
{
#if !CONFIG_APP_CLAW_ENABLE_EMOTE
    return display_send_unavailable(req, "emote is not enabled in this build");
#else
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    const char *state_text = NULL;
    cJSON *state_item = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(state_item)) {
        state_text = state_item->valuestring;
    }

    bool state_error = state_text && (strcmp(state_text, "error") == 0);
    bool state_sleep = state_text && (strcmp(state_text, "sleep") == 0 || strcmp(state_text, "sleeping") == 0);
    emote_face_state_t state = EMOTE_FACE_IDLE;
    if (!state_error && !state_sleep && !display_parse_face_state(state_text, &state)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "state must be off, idle, listen, think, speak, error, or sleep");
    }

    int amp = -1;
    cJSON *amp_item = cJSON_GetObjectItemCaseSensitive(root, "amp");
    if (!amp_item) {
        amp_item = cJSON_GetObjectItemCaseSensitive(root, "amp_milli");
    }
    if (cJSON_IsNumber(amp_item)) {
        amp = amp_item->valueint;
        if (amp < 0) {
            amp = -1;
        } else if (amp > 1000) {
            amp = 1000;
        }
    }
    cJSON_Delete(root);

    emote_face_set_synthetic_amplitude(amp);
    esp_err_t err = ESP_OK;
    const char *response_state = NULL;
    if (state_error) {
        response_state = "error";
        esp_err_t alert_err = emote_set_alert(EMOTE_ALERT_GENERIC, "error");
        if (alert_err != ESP_OK) {
            ESP_LOGW(TAG, "display face error alert unavailable: %s", esp_err_to_name(alert_err));
        }
        err = emote_face_set_state(EMOTE_FACE_THINKING);
    } else if (state_sleep) {
        response_state = "sleep";
        esp_err_t alert_err = emote_set_alert(EMOTE_ALERT_GENERIC, "tap to wake");
        if (alert_err != ESP_OK) {
            ESP_LOGW(TAG, "display face sleep alert unavailable: %s", esp_err_to_name(alert_err));
        }
        err = emote_face_set_state(EMOTE_FACE_OFF);
    } else {
        response_state = display_face_state_name(state);
        if (state != EMOTE_FACE_OFF) {
            (void)emote_clear_alert();
        }
        err = emote_face_set_state(state);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display face control failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display face control failed");
    }

    ESP_LOGI(TAG, "display face control state=%s amp=%d", response_state, amp);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "state", response_state);
    cJSON_AddNumberToObject(resp, "amp", amp);
    return http_server_send_json_response(req, resp);
#endif
}

static esp_err_t display_face_get_handler(httpd_req_t *req)
{
#if !CONFIG_APP_CLAW_ENABLE_EMOTE
    return display_send_unavailable(req, "emote is not enabled in this build");
#else
    emote_face_debug_t dbg = {0};
    esp_err_t err = emote_face_get_debug(&dbg);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "face diagnostics unavailable");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "state", dbg.state);
    cJSON_AddNumberToObject(root, "applied_state", dbg.applied_state);
    cJSON_AddNumberToObject(root, "synth_amp", dbg.synth_amp);
    cJSON_AddNumberToObject(root, "displayed_amp", dbg.displayed_amp);
    cJSON_AddNumberToObject(root, "last_start", dbg.last_start);
    cJSON_AddNumberToObject(root, "last_end", dbg.last_end);
    cJSON_AddNumberToObject(root, "driver_ticks", dbg.driver_ticks);
    cJSON_AddNumberToObject(root, "segment_sets", dbg.segment_sets);
    cJSON_AddNumberToObject(root, "keepalive_kicks", dbg.keepalive_kicks);
    cJSON_AddNumberToObject(root, "state_changes", dbg.state_changes);
    cJSON_AddBoolToObject(root, "running", dbg.running);
    cJSON_AddBoolToObject(root, "object_ready", dbg.object_ready);
    cJSON_AddBoolToObject(root, "display_owned", dbg.display_owned);
    cJSON_AddBoolToObject(root, "current_clip_loaded", dbg.current_clip_loaded);
    return http_server_send_json_response(req, root);
#endif
}

esp_err_t http_server_register_display_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        {
            .uri     = "/api/display/snapshot.ppm",
            .method  = HTTP_GET,
            .handler = display_snapshot_get_handler,
        },
        {
            .uri     = "/api/display/snapshot.json",
            .method  = HTTP_GET,
            .handler = display_snapshot_info_handler,
        },
        {
            .uri     = "/api/display/face",
            .method  = HTTP_GET,
            .handler = display_face_get_handler,
        },
        {
            .uri     = "/api/display/face",
            .method  = HTTP_POST,
            .handler = display_face_control_handler,
        },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
