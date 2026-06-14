/*
 * SPDX-FileCopyrightText: 2026 JarvisRobot
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interactive-UI HTTP API — drives the ui_layer second display-arbiter owner so
 * the round AMOLED can show data, tappable choices, and images WITHOUT a Gemini
 * session (and therefore with no Gemini quota). Mirrors the structure of
 * http_server_display_api.c; verify any render with GET /api/display/snapshot.ppm.
 *
 *   POST /api/ui/choice  {"q":"...","options":["A","B",...]}  -> ui_layer_show_choice
 *   POST /api/ui/data    {"title":"...","rows":[{"label":"..","value":".."},...]}
 *                        (also accepts parallel "labels":[],"values":[])
 *   POST /api/ui/image   {"path":"/sdcard/x.jpg"}  (b64 inline jpeg optional, see below)
 *   POST /api/ui/menu    {"items":["A","B",...]}    -> ui_layer_show_menu
 *   GET  /api/ui/result  -> {"done":bool,"index":int}
 *   POST /api/ui/dismiss -> ui_layer_dismiss
 *
 * Forward-decls instead of #include "ui_layer.h": the http_server component does
 * not carry ui_layer's include dir on its compile path, but the symbols live in
 * the link graph (ui_layer is REQUIRED by app_claw and added to http_server's
 * REQUIRES by bootstrap). Same trick the /api/debug/gain endpoint uses for
 * cap_gemini_live. Keep these prototypes in sync with firmware/ui_layer/ui_layer.h.
 */
#include "http_server_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* ---- ui_layer forward decls (kept in sync with ui_layer.h) ---------------- */
#define HTTP_UI_MAX_OPTIONS 6
#define HTTP_UI_MAX_ROWS    5

/* display_hal capture (provided by lua_module_display; see the bootstrap
 * apply_display_hal_capture_patch). visible_ptr returns the live framebuffer
 * pointer (RGB565) so the snapshot streams without a full-frame copy buffer. */
const uint16_t *display_hal_visible_ptr(int *out_w, int *out_h);

esp_err_t ui_layer_show_choice(const char *question, const char *const *opts, int n);
esp_err_t ui_layer_show_data(const char *title, const char *const *labels,
                             const char *const *values, int n);
esp_err_t ui_layer_show_image(const uint8_t *jpeg, size_t len, const char *path);
esp_err_t ui_layer_show_menu(const char *const *items, int n);
esp_err_t ui_layer_dismiss(void);
esp_err_t ui_layer_get_result(int *out_index, bool *out_done);
bool ui_layer_is_active(void);

static const char *TAG = "http_ui";

/* Bounded copies of strings pulled out of the request JSON. cJSON owns the
 * parsed tree only until cJSON_Delete(root); ui_layer copies internally too, but
 * we keep local storage so the const char* arrays we hand it stay valid for the
 * call and the JSON tree can be freed deterministically. */
#define HTTP_UI_STR_MAX  64

static esp_err_t ui_send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "active", ui_layer_is_active());
    return http_server_send_json_response(req, root);
}

static esp_err_t ui_send_err(httpd_req_t *req, esp_err_t err, const char *msg)
{
    ESP_LOGW(TAG, "%s: %s", msg, esp_err_to_name(err));
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
}

/* POST /api/ui/choice  {"q":"...","options":["A","B"]} */
static esp_err_t ui_choice_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    char question[HTTP_UI_STR_MAX] = {0};
    cJSON *q = cJSON_GetObjectItemCaseSensitive(root, "q");
    if (!cJSON_IsString(q)) {
        q = cJSON_GetObjectItemCaseSensitive(root, "question");
    }
    if (cJSON_IsString(q) && q->valuestring) {
        strlcpy(question, q->valuestring, sizeof(question));
    }

    cJSON *options = cJSON_GetObjectItemCaseSensitive(root, "options");
    if (!cJSON_IsArray(options)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "options must be a JSON array of 2..6 strings");
    }

    int n = cJSON_GetArraySize(options);
    if (n < 2 || n > HTTP_UI_MAX_OPTIONS) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "options must hold 2..6 strings");
    }

    char store[HTTP_UI_MAX_OPTIONS][HTTP_UI_STR_MAX] = {{0}};
    const char *opts[HTTP_UI_MAX_OPTIONS] = {0};
    for (int i = 0; i < n; ++i) {
        cJSON *it = cJSON_GetArrayItem(options, i);
        const char *s = cJSON_IsString(it) ? it->valuestring : NULL;
        strlcpy(store[i], s ? s : "", sizeof(store[i]));
        opts[i] = store[i];
    }
    cJSON_Delete(root);

    esp_err_t err = ui_layer_show_choice(question, opts, n);
    if (err != ESP_OK) {
        return ui_send_err(req, err, "ui_layer_show_choice failed");
    }
    ESP_LOGI(TAG, "show_choice q=\"%s\" n=%d", question, n);
    return ui_send_ok(req);
}

/* POST /api/ui/data  {"title":"...","rows":[{"label":"..","value":".."}]}
 *                    or {"title":"...","labels":[..],"values":[..]} */
static esp_err_t ui_data_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    char title[HTTP_UI_STR_MAX] = {0};
    cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (cJSON_IsString(t) && t->valuestring) {
        strlcpy(title, t->valuestring, sizeof(title));
    }

    char lstore[HTTP_UI_MAX_ROWS][HTTP_UI_STR_MAX] = {{0}};
    char vstore[HTTP_UI_MAX_ROWS][HTTP_UI_STR_MAX] = {{0}};
    const char *labels[HTTP_UI_MAX_ROWS] = {0};
    const char *values[HTTP_UI_MAX_ROWS] = {0};
    int n = 0;

    cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "rows");
    if (cJSON_IsArray(rows)) {
        int count = cJSON_GetArraySize(rows);
        for (int i = 0; i < count && n < HTTP_UI_MAX_ROWS; ++i) {
            cJSON *row = cJSON_GetArrayItem(rows, i);
            if (!cJSON_IsObject(row)) {
                continue;
            }
            cJSON *l = cJSON_GetObjectItemCaseSensitive(row, "label");
            cJSON *v = cJSON_GetObjectItemCaseSensitive(row, "value");
            strlcpy(lstore[n], (cJSON_IsString(l) && l->valuestring) ? l->valuestring : "",
                    sizeof(lstore[n]));
            strlcpy(vstore[n], (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "",
                    sizeof(vstore[n]));
            labels[n] = lstore[n];
            values[n] = vstore[n];
            n++;
        }
    } else {
        /* parallel "labels"/"values" arrays */
        cJSON *la = cJSON_GetObjectItemCaseSensitive(root, "labels");
        cJSON *va = cJSON_GetObjectItemCaseSensitive(root, "values");
        int lc = cJSON_IsArray(la) ? cJSON_GetArraySize(la) : 0;
        int vc = cJSON_IsArray(va) ? cJSON_GetArraySize(va) : 0;
        int count = lc > vc ? lc : vc;
        for (int i = 0; i < count && n < HTTP_UI_MAX_ROWS; ++i) {
            cJSON *l = cJSON_GetArrayItem(la, i);
            cJSON *v = cJSON_GetArrayItem(va, i);
            strlcpy(lstore[n], (cJSON_IsString(l) && l->valuestring) ? l->valuestring : "",
                    sizeof(lstore[n]));
            strlcpy(vstore[n], (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "",
                    sizeof(vstore[n]));
            labels[n] = lstore[n];
            values[n] = vstore[n];
            n++;
        }
    }
    cJSON_Delete(root);

    esp_err_t err = ui_layer_show_data(title, labels, values, n);
    if (err != ESP_OK) {
        return ui_send_err(req, err, "ui_layer_show_data failed");
    }
    ESP_LOGI(TAG, "show_data title=\"%s\" rows=%d", title, n);
    return ui_send_ok(req);
}

/* POST /api/ui/image  {"path":"/sdcard/diagnostics/x.jpg"}
 * Inline base64 jpeg is intentionally NOT supported here (large bodies +
 * decode-from-path is the offline-test path); use a filesystem path the device
 * can read. The Gemini/tool path can still call ui_layer_show_image() with an
 * in-memory buffer directly. */
static esp_err_t ui_image_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    char path[HTTP_SERVER_PATH_MAX] = {0};
    cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (cJSON_IsString(p) && p->valuestring) {
        strlcpy(path, p->valuestring, sizeof(path));
    }
    cJSON_Delete(root);

    if (!path[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "path is required (filesystem JPEG path)");
    }

    esp_err_t err = ui_layer_show_image(NULL, 0, path);
    if (err != ESP_OK) {
        return ui_send_err(req, err, "ui_layer_show_image failed");
    }
    ESP_LOGI(TAG, "show_image path=\"%s\"", path);
    return ui_send_ok(req);
}

/* POST /api/ui/menu  {"items":["A","B",...]} */
static esp_err_t ui_menu_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "items must be a JSON array of 1..6 strings");
    }
    int n = cJSON_GetArraySize(items);
    if (n < 1 || n > HTTP_UI_MAX_OPTIONS) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "items must hold 1..6 strings");
    }

    char store[HTTP_UI_MAX_OPTIONS][HTTP_UI_STR_MAX] = {{0}};
    const char *arr[HTTP_UI_MAX_OPTIONS] = {0};
    for (int i = 0; i < n; ++i) {
        cJSON *it = cJSON_GetArrayItem(items, i);
        const char *s = cJSON_IsString(it) ? it->valuestring : NULL;
        strlcpy(store[i], s ? s : "", sizeof(store[i]));
        arr[i] = store[i];
    }
    cJSON_Delete(root);

    esp_err_t err = ui_layer_show_menu(arr, n);
    if (err != ESP_OK) {
        return ui_send_err(req, err, "ui_layer_show_menu failed");
    }
    ESP_LOGI(TAG, "show_menu n=%d", n);
    return ui_send_ok(req);
}

/* GET /api/ui/result -> {"ok":true,"done":bool,"index":int,"active":bool} */
static esp_err_t ui_result_handler(httpd_req_t *req)
{
    int index = -1;
    bool done = false;
    ui_layer_get_result(&index, &done);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "done", done);
    cJSON_AddNumberToObject(root, "index", done ? index : -1);
    cJSON_AddBoolToObject(root, "active", ui_layer_is_active());
    return http_server_send_json_response(req, root);
}

/* POST /api/ui/dismiss -> {"ok":true} */
static esp_err_t ui_dismiss_handler(httpd_req_t *req)
{
    esp_err_t err = ui_layer_dismiss();
    if (err != ESP_OK) {
        return ui_send_err(req, err, "ui_layer_dismiss failed");
    }
    ESP_LOGI(TAG, "dismiss");
    return ui_send_ok(req);
}

/* GET /api/ui/snapshot.ppm -> the UI layer's visible framebuffer as a binary
 * PPM (P6). The emote-mirror /api/display/snapshot.ppm cannot see this layer
 * because the face yields the panel while a UI scene owns it. Troubleshooting
 * tool: lets the screen render be captured even when ui_layer holds the panel. */
static esp_err_t ui_snapshot_handler(httpd_req_t *req)
{
    int w = 0, h = 0;
    const uint16_t *fb = display_hal_visible_ptr(&w, &h);
    if (!fb || w <= 0 || h <= 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no UI framebuffer (is a ui_layer scene active?)");
    }
    /* Stream-convert RGB565 -> RGB888 a row at a time: a ~1.4 KB row buffer
     * instead of a full-frame copy, which cannot allocate once display_hal has
     * fragmented PSRAM. */
    uint8_t *row = malloc((size_t)w * 3);
    if (!row) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "row alloc failed");
    }
    httpd_resp_set_type(req, "image/x-portable-pixmap");
    char hdr[32];
    int hlen = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", w, h);
    httpd_resp_send_chunk(req, hdr, hlen);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint16_t px = fb[(size_t)y * w + x];
            row[x * 3 + 0] = (uint8_t)(((px >> 11) & 0x1F) << 3);
            row[x * 3 + 1] = (uint8_t)(((px >> 5) & 0x3F) << 2);
            row[x * 3 + 2] = (uint8_t)((px & 0x1F) << 3);
        }
        httpd_resp_send_chunk(req, (const char *)row, (size_t)w * 3);
    }
    free(row);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t http_server_register_ui_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/ui/choice",  .method = HTTP_POST, .handler = ui_choice_handler },
        { .uri = "/api/ui/data",    .method = HTTP_POST, .handler = ui_data_handler },
        { .uri = "/api/ui/image",   .method = HTTP_POST, .handler = ui_image_handler },
        { .uri = "/api/ui/menu",    .method = HTTP_POST, .handler = ui_menu_handler },
        { .uri = "/api/ui/result",  .method = HTTP_GET,  .handler = ui_result_handler },
        { .uri = "/api/ui/dismiss", .method = HTTP_POST, .handler = ui_dismiss_handler },
        { .uri = "/api/ui/snapshot.ppm", .method = HTTP_GET, .handler = ui_snapshot_handler },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
