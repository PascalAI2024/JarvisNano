/*
 * GET /api/display/snapshot.ppm
 *
 * Streams the latest emote-rendered panel mirror as a binary PPM image. This
 * captures the pixels the firmware last flushed to the display; it does not
 * depend on panel readback support. Add ?save=1 to also write the capture to
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
#endif

static const char *TAG = "http_display";

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

    ESP_LOGI(TAG, "display snapshot stream frame=%llu %ux%u save=%s",
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
    cJSON_AddNumberToObject(root, "width", info.width);
    cJSON_AddNumberToObject(root, "height", info.height);
    cJSON_AddNumberToObject(root, "bytes", info.bytes);
    cJSON_AddNumberToObject(root, "frame_id", info.frame_id);
    cJSON_AddNumberToObject(root, "last_flush_ms", info.last_flush_ms);
    cJSON_AddBoolToObject(root, "valid", info.valid);
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

    emote_face_state_t state = EMOTE_FACE_IDLE;
    if (!display_parse_face_state(state_text, &state)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "state must be off, idle, listen, think, or speak");
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
    esp_err_t err = emote_face_set_state(state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display face control failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display face control failed");
    }

    ESP_LOGI(TAG, "display face control state=%s amp=%d", display_face_state_name(state), amp);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    http_server_json_add_string(resp, "state", display_face_state_name(state));
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
