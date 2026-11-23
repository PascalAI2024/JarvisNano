/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * One persistent worker owns all blocking JarvisMCP HTTPS work. The voice
 * owner only submits/polls fixed-size records; it never blocks on network I/O.
 */
#include "jr_tools/jr_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jr_net/jr_net.h"
#include "jr_tools/jr_weather.h"

static const char *TAG = "jr_tools";

#define JR_TOOLS_JOB_QUEUE_DEPTH     4U
#define JR_TOOLS_RESULT_QUEUE_DEPTH  4U
#define JR_TOOLS_CANCEL_SLOTS        12U
#define JR_TOOLS_CODE_CAP            4096U  /* the program + args; board_poll is ~3 KB */
#define JR_TOOLS_HTTP_BODY_CAP       8192U
#define JR_TOOLS_HTTP_TIMEOUT_MS     35000
#define JR_TOOLS_KEEPALIVE_IDLE_US   (45LL * 1000LL * 1000LL)
#define JR_TOOLS_IDLE_POLL_MS        5000U
#define JR_TOOLS_TASK_STACK          16384U
#define JR_TOOLS_TASK_PRIORITY       4U

/* A result slot is just over 3 KB. Four result slots plus four owned jobs do
 * not belong in the ESP32-S3's scarce internal heap, especially after the
 * 20 KB voice stack has been reserved. These queues are task-only (never
 * touched by an ISR or DMA), so keep both their control blocks and storage in
 * PSRAM on the device. The ordinary allocator remains the host/non-PSRAM
 * fallback. */
#if defined(CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION) && \
    CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION && defined(CONFIG_SPIRAM) && \
    CONFIG_SPIRAM
#define JR_TOOLS_QUEUE_WITH_CAPS 1
static QueueHandle_t tools_queue_create(UBaseType_t depth,
                                        UBaseType_t item_size)
{
    return xQueueCreateWithCaps(depth, item_size,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void tools_queue_delete(QueueHandle_t queue)
{
    if (queue != NULL) {
        vQueueDeleteWithCaps(queue);
    }
}
#else
#define JR_TOOLS_QUEUE_WITH_CAPS 0
static QueueHandle_t tools_queue_create(UBaseType_t depth,
                                        UBaseType_t item_size)
{
    return xQueueCreate(depth, item_size);
}

static void tools_queue_delete(QueueHandle_t queue)
{
    if (queue != NULL) {
        vQueueDelete(queue);
    }
}
#endif

typedef struct {
    uint32_t call_id;
    char call_id_text[JR_TOOLS_CALL_ID_TEXT_CAP];
    char name[JR_TOOLS_NAME_CAP];
    char args_json[JR_TOOLS_ARGS_CAP];
    uint32_t session_gen;
    bool physical_confirmed;
} owned_job_t;

typedef struct {
    bool used;
    uint32_t call_id;
    char call_id_text[JR_TOOLS_CALL_ID_TEXT_CAP];
} cancel_entry_t;

static struct {
    QueueHandle_t jobs;
    QueueHandle_t results;
    TaskHandle_t task;
    portMUX_TYPE lock;
    bool initialized;
    uint32_t boot_nonce;
    uint32_t session_gen;
    uint32_t config_gen;
    uint8_t cancel_cursor;
    uint32_t oversize;                 /* bodies past JR_TOOLS_HTTP_BODY_CAP */
    char last_target[JR_TOOLS_NAME_CAP]; /* method the last job addressed */
    cancel_entry_t cancelled[JR_TOOLS_CANCEL_SLOTS];
    char mcp_url[JR_CFG_MCP_URL_CAP];
    char mcp_key[JR_CFG_MCP_KEY_CAP];
} s_tools = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void secure_zero(void *ptr, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len-- > 0U) {
        *p++ = 0U;
    }
}

static bool copy_bounded(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0U || src == NULL) {
        return false;
    }
    size_t len = strnlen(src, cap);
    if (len >= cap) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, src, len + 1U);
    return true;
}

static bool cancellation_matches(const cancel_entry_t *entry,
                                 const owned_job_t *job)
{
    if (!entry->used) {
        return false;
    }
    if (entry->call_id_text[0] != '\0' && job->call_id_text[0] != '\0') {
        return strcmp(entry->call_id_text, job->call_id_text) == 0;
    }
    return entry->call_id != 0U && entry->call_id == job->call_id;
}

static bool job_cancelled(const owned_job_t *job, bool consume)
{
    bool found = false;
    taskENTER_CRITICAL(&s_tools.lock);
    for (size_t i = 0; i < JR_TOOLS_CANCEL_SLOTS; ++i) {
        if (cancellation_matches(&s_tools.cancelled[i], job)) {
            found = true;
            if (consume) {
                memset(&s_tools.cancelled[i], 0, sizeof(s_tools.cancelled[i]));
            }
            break;
        }
    }
    taskEXIT_CRITICAL(&s_tools.lock);
    return found;
}

static bool job_stale(const owned_job_t *job)
{
    uint32_t current;
    taskENTER_CRITICAL(&s_tools.lock);
    current = s_tools.session_gen;
    taskEXIT_CRITICAL(&s_tools.lock);
    /* Generation zero means the composition root has not enabled filtering;
     * SESSION_ANY marks a job the device owns, which no session can orphan. */
    return current != 0U && job->session_gen != JR_TOOLS_SESSION_ANY &&
           job->session_gen != current;
}

static void set_error_json(jr_tool_result_t *result, jr_tool_status_t status)
{
    const char *code = "internal_error";
    const char *message = "Tool execution failed";
    switch (status) {
    case JR_TOOL_STATUS_UNKNOWN_TOOL:
        code = "unknown_tool"; message = "Tool is not allowlisted"; break;
    case JR_TOOL_STATUS_INVALID_ARGS:
        code = "invalid_args"; message = "Tool arguments are invalid"; break;
    case JR_TOOL_STATUS_NOT_CONFIGURED:
        code = "not_configured"; message = "JarvisMCP is not configured"; break;
    case JR_TOOL_STATUS_CANCELLED:
        code = "cancelled"; message = "Tool call was cancelled"; break;
    case JR_TOOL_STATUS_STALE:
        code = "stale_session"; message = "Tool call belongs to an old session"; break;
    case JR_TOOL_STATUS_NETWORK_ERROR:
        code = "network_error"; message = "JarvisMCP request failed"; break;
    case JR_TOOL_STATUS_HTTP_ERROR:
        code = "http_error"; message = "JarvisMCP rejected the request"; break;
    case JR_TOOL_STATUS_BAD_RESPONSE:
        code = "bad_response"; message = "JarvisMCP returned an invalid response"; break;
    case JR_TOOL_STATUS_INTERNAL_ERROR:
    case JR_TOOL_STATUS_OK:
    default:
        break;
    }
    result->status = status;
    (void)snprintf(result->response_json, sizeof(result->response_json),
                   "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                   code, message);
}

static void init_result(const owned_job_t *job, jr_tool_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->call_id = job->call_id;
    result->session_gen = job->session_gen;
    (void)copy_bounded(result->call_id_text, sizeof(result->call_id_text),
                       job->call_id_text);
    (void)copy_bounded(result->name, sizeof(result->name), job->name);
    set_error_json(result, JR_TOOL_STATUS_INTERNAL_ERROR);
}

static void publish_result(const jr_tool_result_t *result)
{
    if (xQueueSend(s_tools.results, result, 0) == pdTRUE) {
        return;
    }
    /* A stalled owner cannot pin unbounded memory. Preserve the newest result,
     * which is the one most likely to belong to the current session. */
    jr_tool_result_t discarded;
    (void)xQueueReceive(s_tools.results, &discarded, 0);
    (void)xQueueSend(s_tools.results, result, 0);
}

static bool snapshot_config(char *url, size_t url_cap, char *key, size_t key_cap,
                            uint32_t *out_gen)
{
    bool copied;
    uint32_t gen;
    taskENTER_CRITICAL(&s_tools.lock);
    copied = copy_bounded(url, url_cap, s_tools.mcp_url) &&
             copy_bounded(key, key_cap, s_tools.mcp_key);
    gen = s_tools.config_gen;
    taskEXIT_CRITICAL(&s_tools.lock);
    if (out_gen != NULL) {
        *out_gen = gen;
    }
    return copied && strncmp(url, "https://", 8U) == 0 && url[8] != '\0' &&
           key[0] != '\0';
}

static uint32_t current_config_gen(void)
{
    taskENTER_CRITICAL(&s_tools.lock);
    const uint32_t gen = s_tools.config_gen;
    taskEXIT_CRITICAL(&s_tools.lock);
    return gen;
}

/* A gateway body larger than JR_TOOLS_HTTP_BODY_CAP. It used to end as a
 * silent bad_response on an HTTP 200; now one log line names the method (never
 * its arguments) and the sizes, the counter moves, and the model gets a
 * bounded answer that says the result was too large and to narrow the
 * request. */
static void note_oversize(const char *tool, const char *args_json,
                          int64_t announced, size_t bytes_read,
                          jr_tool_result_t *result)
{
    char target[JR_TOOLS_NAME_CAP];
    char line[160];
    (void)jr_tools_target_name(tool, args_json, target, sizeof target);
    (void)jr_tools_describe_oversize(line, sizeof line, tool, target, announced,
                                     bytes_read, JR_TOOLS_HTTP_BODY_CAP);
    ESP_LOGW(TAG, "%s", line);
    (void)jr_tools_oversize_result(result->response_json,
                                   sizeof(result->response_json), announced,
                                   bytes_read);
    taskENTER_CRITICAL(&s_tools.lock);
    s_tools.oversize++;
    taskEXIT_CRITICAL(&s_tools.lock);
}

static void record_target(const char *tool, const char *args_json)
{
    char target[JR_TOOLS_NAME_CAP];
    (void)jr_tools_target_name(tool, args_json, target, sizeof target);
    taskENTER_CRITICAL(&s_tools.lock);
    memcpy(s_tools.last_target, target, sizeof s_tools.last_target);
    taskEXIT_CRITICAL(&s_tools.lock);
}

uint32_t jr_tools_oversize_count(void)
{
    taskENTER_CRITICAL(&s_tools.lock);
    const uint32_t count = s_tools.oversize;
    taskEXIT_CRITICAL(&s_tools.lock);
    return count;
}

void jr_tools_last_target(char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0U) {
        return;
    }
    taskENTER_CRITICAL(&s_tools.lock);
    strncpy(out, s_tools.last_target, out_cap - 1U);
    taskEXIT_CRITICAL(&s_tools.lock);
    out[out_cap - 1U] = '\0';
}

static bool response_from_mcp(const char *raw, jr_tool_result_t *result)
{
    /* Diagnostic, added because every tool call was returning bad_response on
     * an HTTP 200 and nothing said why. Three distinct failures all collapsed
     * into one status: unparseable JSON, a non-object root, and a normalized
     * result too large for the 3072-byte response buffer. The upstream payload
     * measured only ~1.7 KB, so "too big" was a guess — this replaces the
     * guess with the number. Body is tool output, not credentials, and it is
     * capped hard at 160 characters. */
    const size_t raw_len = raw ? strlen(raw) : 0U;
    cJSON *root = cJSON_Parse(raw);
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "mcp parse failed: %s len=%u head=\"%.160s\"",
                 root == NULL ? "not JSON" : "root not an object",
                 (unsigned)raw_len, raw ? raw : "");
        cJSON_Delete(root);
        return false;
    }
    cJSON *upstream = cJSON_GetObjectItemCaseSensitive(root, "result");
    /* Typed gateways historically wrapped payloads in {"result":...}; the
     * current device endpoint may return the bounded result object directly.
     * Normalize both shapes for Gemini instead of treating HTTP 200 as a
     * transport failure. */
    if (upstream == NULL) {
        upstream = root;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON *copy = cJSON_Duplicate(upstream, true);
    if (response == NULL || copy == NULL) {
        ESP_LOGE(TAG, "mcp result copy failed: out of memory raw=%u",
                 (unsigned)raw_len);
        cJSON_Delete(copy);
        cJSON_Delete(response);
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddItemToObject(response, "result", copy);
    bool ok = cJSON_PrintPreallocated(response, result->response_json,
                                     sizeof(result->response_json), false);
    if (!ok) {
        /* OVER BUDGET IS NOT A FAILURE. It used to be: a result one byte past
         * the cap became bad_response, Gemini got nothing, and the owner's
         * experience was "I can't get any help" — intermittently, because
         * whether a search fits depends on the query. Measured on the device:
         * need=3728 against cap=3072, over by 656 bytes.
         *
         * A truncated answer beats no answer. Emit a bounded object that says
         * plainly it was cut, carries the byte counts, and includes as much of
         * the real payload as fits — Gemini can use a partial catalogue, and
         * the `truncated` flag lets it say so rather than inventing the rest.
         *
         * The proper fix is server-side projection with a cursor (PLAN N6.3);
         * this is the floor that stops a size overrun from being total data
         * loss while that lands. */
        char *measured = cJSON_PrintUnformatted(response);
        const unsigned need = (unsigned)(measured ? strlen(measured) : 0U);
        ESP_LOGW(TAG, "mcp result over budget: need=%u cap=%u raw=%u — truncating",
                 need, (unsigned)sizeof(result->response_json), (unsigned)raw_len);

        /* Reserve room for the envelope, then fill the remainder with payload. */
        static const char *const head =
            "{\"result\":{\"truncated\":true,\"note\":"
            "\"payload exceeded the device budget; content is cut\",\"text\":\"";
        static const char *const tail = "\"}}";
        const size_t cap = sizeof(result->response_json);
        const size_t hlen = strlen(head), tlen = strlen(tail);
        if (measured != NULL && cap > hlen + tlen + 16U) {
            size_t room = cap - hlen - tlen - 1U;
            memcpy(result->response_json, head, hlen);
            size_t w = hlen;
            /* Copy payload, JSON-escaping the few characters that would break
             * the string, and stop on a whole escape rather than mid-sequence. */
            for (size_t i = 0; measured[i] != '\0' && w + 8U < hlen + room; ++i) {
                unsigned char ch = (unsigned char)measured[i];
                if (ch == '"' || ch == '\\') {
                    result->response_json[w++] = '\\';
                    result->response_json[w++] = (char)ch;
                } else if (ch < 0x20) {
                    w += (size_t)snprintf(result->response_json + w,
                                          cap - w, "\\u%04x", ch);
                } else {
                    result->response_json[w++] = (char)ch;
                }
            }
            memcpy(result->response_json + w, tail, tlen);
            result->response_json[w + tlen] = '\0';
            ok = true;
        }
        if (measured != NULL) {
            cJSON_free(measured);
        }
        if (!ok) {
            ESP_LOGE(TAG, "mcp result over budget and could not be truncated: "
                     "need=%u cap=%u raw=%u", need,
                     (unsigned)sizeof(result->response_json), (unsigned)raw_len);
        }
    }
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ok;
}

static bool is_typed_device_endpoint(const char *url)
{
    return url != NULL && strstr(url, "/device/v1/invoke") != NULL;
}

static char *build_request_body(const char *url, const char *tool_name,
                                const char *args_json, const char *code,
                                bool physical_confirmed,
                                const char *request_id)
{
    cJSON *request = cJSON_CreateObject();
    if (request == NULL) {
        return NULL;
    }
    if (is_typed_device_endpoint(url)) {
        cJSON *args = cJSON_Parse(args_json != NULL ? args_json : "{}");
        if (!cJSON_IsObject(args) ||
            !cJSON_AddStringToObject(request, "tool", tool_name)) {
            cJSON_Delete(args);
            cJSON_Delete(request);
            return NULL;
        }
        if (strcmp(tool_name, "execute_tool") == 0) {
            cJSON *target = cJSON_GetObjectItemCaseSensitive(args, "tool");
            cJSON *nested_text =
                cJSON_GetObjectItemCaseSensitive(args, "args_json");
            cJSON *nested = cJSON_IsString(nested_text) &&
                    nested_text->valuestring != NULL
                ? cJSON_Parse(nested_text->valuestring) : NULL;
            cJSON *translated = cJSON_CreateObject();
            bool translated_ok = cJSON_IsString(target) &&
                target->valuestring != NULL && cJSON_IsObject(nested) &&
                translated != NULL &&
                cJSON_AddStringToObject(translated, "tool",
                                       target->valuestring);
            if (translated_ok) {
                cJSON_AddItemToObject(translated, "args", nested);
                nested = NULL;
            }
            cJSON_Delete(nested);
            cJSON_Delete(args);
            args = translated;
            if (!translated_ok) {
                cJSON_Delete(args);
                cJSON_Delete(request);
                return NULL;
            }
        }
        cJSON_AddItemToObject(request, "args", args);
        if (physical_confirmed) {
            if (request_id == NULL || request_id[0] == '\0' ||
                !cJSON_AddStringToObject(request, "request_id", request_id)) {
                cJSON_Delete(request);
                return NULL;
            }
            cJSON *confirmation = cJSON_AddObjectToObject(request,
                                                          "confirmation");
            if (confirmation == NULL ||
                !cJSON_AddBoolToObject(confirmation, "approved", true) ||
                !cJSON_AddStringToObject(confirmation, "source", "touch")) {
                cJSON_Delete(request);
                return NULL;
            }
        }
    } else if (!cJSON_AddStringToObject(request, "code", code)) {
        cJSON_Delete(request);
        return NULL;
    }
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    return body;
}

/* One tools_task-owned persistent HTTPS client for JarvisMCP keep-alive.
 * Never touched from any other task; auto-invalidated on config reload,
 * transport/HTTP error, or 45 s idle so TLS RAM is released during rest. */
static esp_http_client_handle_t s_mcp_client;
static uint32_t s_mcp_client_gen;
static int64_t  s_mcp_client_last_used_us;

static void mcp_client_close(void)
{
    if (s_mcp_client != NULL) {
        esp_http_client_cleanup(s_mcp_client);
        s_mcp_client = NULL;
        s_mcp_client_gen = 0U;
        s_mcp_client_last_used_us = 0;
    }
}

static void mcp_client_maybe_expire(void)
{
    if (s_mcp_client == NULL) {
        return;
    }
    const uint32_t gen = current_config_gen();
    const int64_t now_us = esp_timer_get_time();
    if (gen != s_mcp_client_gen ||
        (now_us - s_mcp_client_last_used_us) >= JR_TOOLS_KEEPALIVE_IDLE_US) {
        ESP_LOGI(TAG, "closing idle/stale mcp keep-alive handle (cfg_changed=%d)",
                 gen != s_mcp_client_gen ? 1 : 0);
        mcp_client_close();
    }
}

static jr_tool_status_t post_tool(const char *tool_name, const char *args_json,
                                  const char *code, bool physical_confirmed,
                                  const char *request_id,
                                  jr_tool_result_t *result)
{
    char url[JR_CFG_MCP_URL_CAP] = {0};
    char key[JR_CFG_MCP_KEY_CAP] = {0};
    char auth[JR_CFG_MCP_KEY_CAP + 8U] = {0};
    uint32_t cfg_gen = 0U;
    if (!snapshot_config(url, sizeof(url), key, sizeof(key), &cfg_gen)) {
        secure_zero(key, sizeof(key));
        mcp_client_close();
        return JR_TOOL_STATUS_NOT_CONFIGURED;
    }

    char *body = build_request_body(url, tool_name, args_json, code,
                                    physical_confirmed, request_id);
    if (body == NULL) {
        secure_zero(key, sizeof(key));
        return JR_TOOL_STATUS_INTERNAL_ERROR;
    }

    /* Drop any cached handle whose config changed or whose idle budget elapsed. */
    mcp_client_maybe_expire();
    if (s_mcp_client != NULL && s_mcp_client_gen != cfg_gen) {
        mcp_client_close();
    }

    (void)snprintf(auth, sizeof(auth), "Bearer %s", key);
    secure_zero(key, sizeof(key));

    jr_tool_status_t outcome = JR_TOOL_STATUS_NETWORK_ERROR;
    const size_t body_len = strlen(body);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool reused = (s_mcp_client != NULL);
        if (s_mcp_client == NULL) {
            esp_http_client_config_t config = {
                .url = url,
                .method = HTTP_METHOD_POST,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms = JR_TOOLS_HTTP_TIMEOUT_MS,
                .disable_auto_redirect = true,
                .keep_alive_enable = true,
                .keep_alive_idle = 30,
                .keep_alive_interval = 5,
                .keep_alive_count = 3,
                .buffer_size = 1024,
                .buffer_size_tx = 1024,
            };
            s_mcp_client = esp_http_client_init(&config);
            if (s_mcp_client == NULL) {
                outcome = JR_TOOL_STATUS_INTERNAL_ERROR;
                break;
            }
            s_mcp_client_gen = cfg_gen;
            esp_err_t header_err = esp_http_client_set_header(
                s_mcp_client, "Content-Type", "application/json");
            if (header_err == ESP_OK) {
                header_err = esp_http_client_set_header(
                    s_mcp_client, "Authorization", auth);
            }
            if (header_err != ESP_OK) {
                mcp_client_close();
                outcome = JR_TOOL_STATUS_NETWORK_ERROR;
                break;
            }
        }

        esp_http_client_handle_t client = s_mcp_client;
        result->http_status = 0;
        outcome = JR_TOOL_STATUS_NETWORK_ERROR;
        bool keep_conn = false;

        esp_err_t err = esp_http_client_open(client, (int)body_len);
        size_t sent = 0U;
        if (err == ESP_OK) {
            while (sent < body_len) {
                int written = esp_http_client_write(client, body + sent,
                                                    (int)(body_len - sent));
                if (written <= 0) {
                    break;
                }
                sent += (size_t)written;
            }
        }
        int64_t announced = -1;
        if (err == ESP_OK && sent == body_len) {
            announced = esp_http_client_fetch_headers(client);
            result->http_status = esp_http_client_get_status_code(client);
        }

        /* A stale keep-alive socket commonly surfaces as open/write failure or
         * immediate EOF (announced < 0, http_status == 0) on the reused handle.
         * Tear it down and reconnect once cleanly. */
        if (reused && attempt == 0 &&
            (err != ESP_OK || sent < body_len ||
             (announced < 0 && result->http_status == 0))) {
            ESP_LOGI(TAG, "mcp keep-alive stale (err=%d sent=%u/%u status=%d), reconnecting",
                     (int)err, (unsigned)sent, (unsigned)body_len,
                     result->http_status);
            mcp_client_close();
            continue;
        }

        if (err == ESP_OK && sent == body_len &&
            (announced >= 0 || result->http_status > 0)) {
            if (result->http_status < 200 || result->http_status >= 300) {
                outcome = JR_TOOL_STATUS_HTTP_ERROR;
            } else if (announced > (int64_t)JR_TOOLS_HTTP_BODY_CAP) {
                note_oversize(tool_name, args_json, announced, 0U, result);
                outcome = JR_TOOL_STATUS_OK;
            } else {
                /* Off internal SRAM: TLS session needs the contiguous internal
                 * block, matching fetch_weather(). */
                char *raw = (char *)heap_caps_malloc(
                    JR_TOOLS_HTTP_BODY_CAP + 1U,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (raw == NULL) {
                    raw = (char *)malloc(JR_TOOLS_HTTP_BODY_CAP + 1U);
                }
                if (raw == NULL) {
                    outcome = JR_TOOL_STATUS_INTERNAL_ERROR;
                } else {
                    size_t have = 0U;
                    bool overflow = false;
                    for (;;) {
                        /* Never ask for more bytes than announced: on a
                         * keep-alive socket, reading past Content-Length waits
                         * out the full HTTP timeout. */
                        if (announced >= 0 && have >= (size_t)announced) {
                            raw[have] = '\0';
                            outcome = response_from_mcp(raw, result)
                                          ? JR_TOOL_STATUS_OK
                                          : JR_TOOL_STATUS_BAD_RESPONSE;
                            break;
                        }
                        size_t room = JR_TOOLS_HTTP_BODY_CAP - have;
                        if (announced > 0 && (size_t)announced - have < room) {
                            room = (size_t)announced - have;
                        }
                        if (room == 0U) {
                            char extra;
                            int got = esp_http_client_read(client, &extra, 1);
                            if (got > 0) {
                                overflow = true;
                            } else if (got == 0) {
                                raw[have] = '\0';
                                outcome = response_from_mcp(raw, result)
                                              ? JR_TOOL_STATUS_OK
                                              : JR_TOOL_STATUS_BAD_RESPONSE;
                            } else {
                                outcome = JR_TOOL_STATUS_NETWORK_ERROR;
                            }
                            break;
                        }
                        int got = esp_http_client_read(client, raw + have, (int)room);
                        if (got < 0) {
                            outcome = JR_TOOL_STATUS_NETWORK_ERROR;
                            break;
                        }
                        if (got == 0) {
                            raw[have] = '\0';
                            outcome = response_from_mcp(raw, result)
                                          ? JR_TOOL_STATUS_OK
                                          : JR_TOOL_STATUS_BAD_RESPONSE;
                            break;
                        }
                        have += (size_t)got;
                        if (esp_http_client_is_complete_data_received(client)) {
                            raw[have] = '\0';
                            outcome = response_from_mcp(raw, result)
                                          ? JR_TOOL_STATUS_OK
                                          : JR_TOOL_STATUS_BAD_RESPONSE;
                            break;
                        }
                    }
                    if (overflow) {
                        note_oversize(tool_name, args_json, announced,
                                      JR_TOOLS_HTTP_BODY_CAP + 1U, result);
                        outcome = JR_TOOL_STATUS_OK;
                    } else if (outcome == JR_TOOL_STATUS_OK &&
                               esp_http_client_is_complete_data_received(client)) {
                        keep_conn = true;
                    }
                    secure_zero(raw, JR_TOOLS_HTTP_BODY_CAP + 1U);
                    free(raw);
                }
            }
        }

        if (keep_conn) {
            s_mcp_client_last_used_us = esp_timer_get_time();
        } else {
            mcp_client_close();
        }
        break;
    }

    secure_zero(auth, sizeof(auth));
    secure_zero(body, body_len);
    free(body);
    return outcome;
}

/* The weather glance does not go through the gateway: Open-Meteo is public and
 * keyless, so one direct HTTPS GET serves it, with no Authorization header and
 * no gateway configuration. The body (bounded, measured ~2 KB) is projected to
 * the compact JSON weather_apply_result reads. Same TLS bundle, same timeout,
 * same single worker as post_tool. */
static jr_tool_status_t fetch_weather(jr_tool_result_t *result)
{
    esp_http_client_config_t config = {
        .url = JR_WEATHER_URL,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = JR_TOOLS_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return JR_TOOL_STATUS_INTERNAL_ERROR;
    }
    /* Off the task stack, and off internal RAM when PSRAM exists: the TLS
     * session is what needs the contiguous internal block. */
    char *raw = (char *)heap_caps_malloc(JR_WEATHER_BODY_CAP + 1U,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) {
        raw = (char *)malloc(JR_WEATHER_BODY_CAP + 1U);
    }
    jr_tool_status_t outcome = JR_TOOL_STATUS_NETWORK_ERROR;
    size_t have = 0U;
    if (raw == NULL) {
        outcome = JR_TOOL_STATUS_INTERNAL_ERROR;
    } else if (esp_http_client_open(client, 0) == ESP_OK) {
        const int64_t announced = esp_http_client_fetch_headers(client);
        result->http_status = esp_http_client_get_status_code(client);
        if (result->http_status < 200 || result->http_status >= 300) {
            outcome = JR_TOOL_STATUS_HTTP_ERROR;
        } else if (announced > (int64_t)JR_WEATHER_BODY_CAP) {
            ESP_LOGE(TAG, "weather: body too large announced=%lld cap=%u",
                     (long long)announced, (unsigned)JR_WEATHER_BODY_CAP);
            outcome = JR_TOOL_STATUS_BAD_RESPONSE;
        } else {
            for (;;) {
                /* One byte past the cap is read to tell "exactly full" from
                 * "too big"; a body that does not fit is rejected. Never ask
                 * for more than the server announced: on a keep-alive
                 * connection a read past the body waits out the whole 35 s
                 * timeout (measured on the device: a 2 KB answer took 36.8 s). */
                size_t want = JR_WEATHER_BODY_CAP + 1U - have;
                if (announced > 0 && (size_t)announced - have < want) {
                    want = (size_t)announced - have;
                }
                const int got = want == 0U ? 0 : esp_http_client_read(
                    client, raw + have, (int)want);
                if (got < 0) {
                    outcome = JR_TOOL_STATUS_NETWORK_ERROR;
                    break;
                }
                have += (size_t)got;
                if (have > JR_WEATHER_BODY_CAP) {
                    ESP_LOGE(TAG, "weather: body too large read=%u cap=%u",
                             (unsigned)have, (unsigned)JR_WEATHER_BODY_CAP);
                    outcome = JR_TOOL_STATUS_BAD_RESPONSE;
                    break;
                }
                if (got == 0 || (announced > 0 && have >= (size_t)announced) ||
                    esp_http_client_is_complete_data_received(client)) {
                    const jr_weather_status_t ws = jr_weather_project(
                        raw, have, result->response_json,
                        sizeof(result->response_json));
                    if (ws != JR_WEATHER_OK) {
                        ESP_LOGE(TAG, "weather: projection failed (%d) bytes=%u",
                                 (int)ws, (unsigned)have);
                    }
                    outcome = ws == JR_WEATHER_OK ? JR_TOOL_STATUS_OK
                                                  : JR_TOOL_STATUS_BAD_RESPONSE;
                    break;
                }
            }
        }
    }
    esp_http_client_cleanup(client);
    free(raw);
    ESP_LOGI(TAG, "weather: open-meteo http=%d bytes=%u stack_free=%u",
             result->http_status, (unsigned)have,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return outcome;
}

/* Cancellation and generation changes win races with a completed request;
 * otherwise the outcome stands. */
static void settle_job(const owned_job_t *job, jr_tool_result_t *result,
                       jr_tool_status_t status)
{
    if (job_stale(job)) {
        set_error_json(result, JR_TOOL_STATUS_STALE);
    } else if (job_cancelled(job, true)) {
        set_error_json(result, JR_TOOL_STATUS_CANCELLED);
    } else if (status != JR_TOOL_STATUS_OK) {
        set_error_json(result, status);
    } else {
        result->status = JR_TOOL_STATUS_OK;
    }
}

static void execute_job(const owned_job_t *job, jr_tool_result_t *result)
{
    if (job_stale(job)) {
        set_error_json(result, JR_TOOL_STATUS_STALE);
        return;
    }
    if (job_cancelled(job, true)) {
        set_error_json(result, JR_TOOL_STATUS_CANCELLED);
        return;
    }
    if (strcmp(job->name, JR_WEATHER_JOB_NAME) == 0) {
        settle_job(job, result, fetch_weather(result));
        return;
    }

    char code[JR_TOOLS_CODE_CAP];
    jr_tool_template_status_t template_status =
        jr_tools_build_code(job->name, job->args_json, code, sizeof(code));
    if (template_status != JR_TOOL_TEMPLATE_OK) {
        set_error_json(result,
                       template_status == JR_TOOL_TEMPLATE_UNKNOWN_TOOL
                           ? JR_TOOL_STATUS_UNKNOWN_TOOL
                           : (template_status == JR_TOOL_TEMPLATE_INVALID_ARGS ||
                              template_status == JR_TOOL_TEMPLATE_TOO_LARGE)
                                 ? JR_TOOL_STATUS_INVALID_ARGS
                                 : JR_TOOL_STATUS_INTERNAL_ERROR);
        return;
    }

    char request_id[JR_TOOLS_REQUEST_ID_CAP] = {0};
    if (job->physical_confirmed &&
        !jr_tools_build_request_id(s_tools.boot_nonce, job->session_gen,
                                   job->call_id, job->call_id_text,
                                   request_id, sizeof(request_id))) {
        secure_zero(code, sizeof(code));
        set_error_json(result, JR_TOOL_STATUS_INTERNAL_ERROR);
        return;
    }
    jr_tool_status_t status = post_tool(job->name, job->args_json, code,
                                       job->physical_confirmed,
                                       request_id[0] != '\0' ? request_id : NULL,
                                       result);
    secure_zero(code, sizeof(code));
    secure_zero(request_id, sizeof(request_id));

    /* The old payload must never be sent into a new model session. */
    settle_job(job, result, status);
}

static void tools_task(void *arg)
{
    (void)arg;
    owned_job_t job;
    for (;;) {
        const TickType_t wait = (s_mcp_client != NULL)
                                    ? pdMS_TO_TICKS(JR_TOOLS_IDLE_POLL_MS)
                                    : portMAX_DELAY;
        if (xQueueReceive(s_tools.jobs, &job, wait) != pdTRUE) {
            mcp_client_maybe_expire();
            continue;
        }
        jr_tool_result_t result;
        init_result(&job, &result);
        int64_t started_us = esp_timer_get_time();
        record_target(job.name, job.args_json);
        execute_job(&job, &result);
        int64_t elapsed_us = esp_timer_get_time() - started_us;
        result.duration_ms = elapsed_us > 0 ? (uint32_t)(elapsed_us / 1000) : 0U;
        publish_result(&result);
        const char *safe_name = result.status == JR_TOOL_STATUS_UNKNOWN_TOOL
                                    ? "unknown" : job.name;
        ESP_LOGI(TAG, "tool=%s status=%s duration_ms=%lu http_status=%d",
                 safe_name, jr_tools_status_name(result.status),
                 (unsigned long)result.duration_ms, result.http_status);
        secure_zero(&job, sizeof(job));
        secure_zero(&result, sizeof(result));
    }
}

static esp_err_t apply_config(const char *url, const char *key)
{
    char next_url[JR_CFG_MCP_URL_CAP] = {0};
    char next_key[JR_CFG_MCP_KEY_CAP] = {0};
    if (!copy_bounded(next_url, sizeof(next_url), url ? url : "") ||
        !copy_bounded(next_key, sizeof(next_key), key ? key : "")) {
        secure_zero(next_key, sizeof(next_key));
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = jr_cfg_validate(JR_CFG_JARVIS_MCP_URL, next_url);
    if (err == ESP_OK) {
        err = jr_cfg_validate(JR_CFG_JARVIS_MCP_KEY, next_key);
    }
    if (err != ESP_OK) {
        secure_zero(next_key, sizeof(next_key));
        return err;
    }
    taskENTER_CRITICAL(&s_tools.lock);
    memcpy(s_tools.mcp_url, next_url, sizeof(next_url));
    secure_zero(s_tools.mcp_key, sizeof(s_tools.mcp_key));
    memcpy(s_tools.mcp_key, next_key, sizeof(next_key));
    s_tools.config_gen++;
    taskEXIT_CRITICAL(&s_tools.lock);
    secure_zero(next_key, sizeof(next_key));
    return ESP_OK;
}

esp_err_t jr_tools_init(const jr_tools_config_t *config)
{
    if (s_tools.initialized) {
        return ESP_OK;
    }

    jr_net_config_t stored = {0};
    bool need_stored = config == NULL || config->mcp_url == NULL ||
                       config->mcp_key == NULL;
    if (need_stored) {
        /* Absence is a supported state. Never log or expose the loaded values. */
        (void)jr_cfg_load(&stored, JR_CFG_VIEW_INTERNAL);
    }
    const char *url = (config != NULL && config->mcp_url != NULL)
                          ? config->mcp_url : stored.jarvis_mcp_url;
    const char *key = (config != NULL && config->mcp_key != NULL)
                          ? config->mcp_key : stored.jarvis_mcp_key;
    if (need_stored) {
        (void)jr_tools_set_board_project(stored.board_project);
    }

    s_tools.jobs = tools_queue_create(JR_TOOLS_JOB_QUEUE_DEPTH,
                                      sizeof(owned_job_t));
    s_tools.results = tools_queue_create(JR_TOOLS_RESULT_QUEUE_DEPTH,
                                         sizeof(jr_tool_result_t));
    if (s_tools.jobs == NULL || s_tools.results == NULL) {
        tools_queue_delete(s_tools.jobs);
        tools_queue_delete(s_tools.results);
        s_tools.jobs = NULL;
        s_tools.results = NULL;
        secure_zero(&stored, sizeof(stored));
        ESP_LOGE(TAG,
                 "queue allocation failed (psram=%d internal_free=%u psram_free=%u)",
                 JR_TOOLS_QUEUE_WITH_CAPS,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = apply_config(url, key);
    if (err != ESP_OK) {
        tools_queue_delete(s_tools.jobs);
        tools_queue_delete(s_tools.results);
        s_tools.jobs = NULL;
        s_tools.results = NULL;
        secure_zero(&stored, sizeof(stored));
        return err;
    }
    s_tools.boot_nonce = esp_random();
    if (s_tools.boot_nonce == 0U) {
        s_tools.boot_nonce = esp_random() ^ (uint32_t)esp_timer_get_time();
    }
    if (s_tools.boot_nonce == 0U) {
        /* Zero is reserved as an invalid namespace. This fallback is only for
         * the vanishingly unlikely all-zero RNG/timer result. */
        s_tools.boot_nonce = 0x4a524e35U;
    }
    s_tools.session_gen = config != NULL ? config->initial_session_gen : 0U;
    s_tools.initialized = true;
    secure_zero(&stored, sizeof(stored));
    ESP_LOGI(TAG, "initialized (configured=%d)", jr_tools_is_configured());
    return ESP_OK;
}

esp_err_t jr_tools_start(void)
{
    if (!s_tools.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tools.task != NULL) {
        return ESP_OK;
    }
    BaseType_t ok;
#if defined(CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION) && \
    CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION && \
    defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && defined(CONFIG_SPIRAM) && \
    CONFIG_SPIRAM
    ok = xTaskCreateWithCaps(tools_task, "jr_tools", JR_TOOLS_TASK_STACK,
                             NULL, JR_TOOLS_TASK_PRIORITY, &s_tools.task,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_tools.task = NULL;
        ok = xTaskCreate(tools_task, "jr_tools", JR_TOOLS_TASK_STACK, NULL,
                         JR_TOOLS_TASK_PRIORITY, &s_tools.task);
    }
#else
    ok = xTaskCreate(tools_task, "jr_tools", JR_TOOLS_TASK_STACK, NULL,
                     JR_TOOLS_TASK_PRIORITY, &s_tools.task);
#endif
    if (ok != pdPASS) {
        s_tools.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t jr_tools_reload_config(void)
{
    if (!s_tools.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    jr_net_config_t stored = {0};
    esp_err_t err = jr_cfg_load(&stored, JR_CFG_VIEW_INTERNAL);
    if (err == ESP_OK) {
        err = apply_config(stored.jarvis_mcp_url, stored.jarvis_mcp_key);
        (void)jr_tools_set_board_project(stored.board_project);
    }
    secure_zero(&stored, sizeof(stored));
    return err;
}

bool jr_tools_is_configured(void)
{
    char url[JR_CFG_MCP_URL_CAP];
    char key[JR_CFG_MCP_KEY_CAP];
    bool configured = snapshot_config(url, sizeof(url), key, sizeof(key), NULL);
    secure_zero(key, sizeof(key));
    return configured;
}

esp_err_t jr_tools_submit(const jr_tool_job_t *job)
{
    if (!s_tools.initialized || s_tools.task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (job == NULL || job->name == NULL || job->name[0] == '\0' ||
        ((job->call_id_text == NULL || job->call_id_text[0] == '\0') &&
         job->call_id == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    owned_job_t owned = {
        .call_id = job->call_id,
        .session_gen = job->session_gen,
        .physical_confirmed = job->physical_confirmed,
    };
    const char *id_text = job->call_id_text ? job->call_id_text : "";
    const char *args = (job->args_json && job->args_json[0]) ? job->args_json : "{}";
    if (!copy_bounded(owned.call_id_text, sizeof(owned.call_id_text), id_text) ||
        !copy_bounded(owned.name, sizeof(owned.name), job->name) ||
        !copy_bounded(owned.args_json, sizeof(owned.args_json), args)) {
        secure_zero(&owned, sizeof(owned));
        return ESP_ERR_INVALID_SIZE;
    }
    BaseType_t queued = xQueueSend(s_tools.jobs, &owned, 0);
    secure_zero(&owned, sizeof(owned));
    return queued == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t jr_tools_cancel(uint32_t call_id, const char *call_id_text)
{
    if (!s_tools.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *text = call_id_text ? call_id_text : "";
    size_t text_len = strnlen(text, JR_TOOLS_CALL_ID_TEXT_CAP);
    if ((call_id == 0U && text_len == 0U) ||
        text_len >= JR_TOOLS_CALL_ID_TEXT_CAP) {
        return text_len >= JR_TOOLS_CALL_ID_TEXT_CAP
                   ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_tools.lock);
    size_t slot = JR_TOOLS_CANCEL_SLOTS;
    for (size_t i = 0; i < JR_TOOLS_CANCEL_SLOTS; ++i) {
        bool same_text = text_len > 0U && s_tools.cancelled[i].used &&
                         strcmp(s_tools.cancelled[i].call_id_text, text) == 0;
        bool same_number = text_len == 0U && call_id != 0U &&
                           s_tools.cancelled[i].used &&
                           s_tools.cancelled[i].call_id == call_id;
        if (same_text || same_number) {
            slot = i;
            break;
        }
        if (!s_tools.cancelled[i].used && slot == JR_TOOLS_CANCEL_SLOTS) {
            slot = i;
        }
    }
    if (slot == JR_TOOLS_CANCEL_SLOTS) {
        slot = s_tools.cancel_cursor++ % JR_TOOLS_CANCEL_SLOTS;
    }
    cancel_entry_t *entry = &s_tools.cancelled[slot];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->call_id = call_id;
    if (text_len > 0U) {
        memcpy(entry->call_id_text, text, text_len + 1U);
    }
    taskEXIT_CRITICAL(&s_tools.lock);
    return ESP_OK;
}

void jr_tools_set_session_generation(uint32_t session_gen)
{
    taskENTER_CRITICAL(&s_tools.lock);
    if (s_tools.session_gen != session_gen) {
        s_tools.session_gen = session_gen;
        memset(s_tools.cancelled, 0, sizeof(s_tools.cancelled));
        s_tools.cancel_cursor = 0U;
    }
    taskEXIT_CRITICAL(&s_tools.lock);
}

bool jr_tools_poll(jr_tool_result_t *out)
{
    return out != NULL && s_tools.results != NULL &&
           xQueueReceive(s_tools.results, out, 0) == pdTRUE;
}

const char *jr_tools_status_name(jr_tool_status_t status)
{
    static const char *const names[] = {
        [JR_TOOL_STATUS_OK] = "ok",
        [JR_TOOL_STATUS_UNKNOWN_TOOL] = "unknown_tool",
        [JR_TOOL_STATUS_INVALID_ARGS] = "invalid_args",
        [JR_TOOL_STATUS_NOT_CONFIGURED] = "not_configured",
        [JR_TOOL_STATUS_CANCELLED] = "cancelled",
        [JR_TOOL_STATUS_STALE] = "stale",
        [JR_TOOL_STATUS_NETWORK_ERROR] = "network_error",
        [JR_TOOL_STATUS_HTTP_ERROR] = "http_error",
        [JR_TOOL_STATUS_BAD_RESPONSE] = "bad_response",
        [JR_TOOL_STATUS_INTERNAL_ERROR] = "internal_error",
    };
    return status >= 0 && (size_t)status < sizeof(names) / sizeof(names[0]) &&
           names[status] != NULL ? names[status] : "unknown";
}
