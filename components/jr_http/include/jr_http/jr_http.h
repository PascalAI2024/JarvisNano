/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_http — production v5 HTTP/dashboard boundary.
 *
 * Read handlers obtain immutable JSON snapshots through one callback. Write
 * handlers authenticate, validate, then enqueue bounded actions for the app's
 * single-writer task. The sole exception is typed NVS config persistence,
 * which is performed atomically through jr_net before a CONFIG_UPDATED action
 * is emitted. No callback is invoked for a write.
 */
#ifndef JR_HTTP_JR_HTTP_H
#define JR_HTTP_JR_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JR_HTTP_DEFAULT_PORT          80U
#define JR_HTTP_PAIRING_TOKEN_CAP     65U
#define JR_HTTP_SAY_TEXT_CAP          200U
#define JR_HTTP_TELEMETRY_JSON_CAP    4096U

typedef enum {
    JR_HTTP_TELEMETRY_STATUS = 0,
    JR_HTTP_TELEMETRY_GEMINI_LIVE,
    JR_HTTP_TELEMETRY_DISPLAY,
    JR_HTTP_TELEMETRY_TOUCH,
    JR_HTTP_TELEMETRY_TOOLS,
} jr_http_telemetry_kind_t;

/* Produce one complete JSON object in `json`. `written` excludes the trailing
 * NUL. The callback must be nonblocking, read-only, and must never include a
 * raw credential/token. Returning an error yields a stable 503 response. */
typedef esp_err_t (*jr_http_telemetry_fn)(void *ctx,
                                          jr_http_telemetry_kind_t kind,
                                          char *json,
                                          size_t capacity,
                                          size_t *written);

typedef struct {
    uint16_t port; /* zero selects JR_HTTP_DEFAULT_PORT */
    jr_http_telemetry_fn telemetry;
    void *telemetry_ctx;
} jr_http_config_t;

typedef enum {
    JR_HTTP_ACTION_CONFIG_UPDATED = 0,
    JR_HTTP_ACTION_RESTART,
    JR_HTTP_ACTION_GEMINI_SAY,
    JR_HTTP_ACTION_AUDIO_GAIN,
} jr_http_action_kind_t;

enum {
    JR_HTTP_GAIN_MIC       = 1U << 0,
    JR_HTTP_GAIN_REFERENCE = 1U << 1,
    JR_HTTP_GAIN_VOLUME    = 1U << 2,
    JR_HTTP_GAIN_BARGE     = 1U << 3,
};

typedef struct {
    uint8_t field_mask;
    int16_t mic_db;
    int16_t reference_db;
    int16_t volume;
    bool barge_enabled;
} jr_http_audio_gain_action_t;

typedef struct {
    jr_http_action_kind_t kind;
    union {
        struct {
            uint32_t field_mask; /* JR_CFG_F_* bits from jr_net. */
        } config;
        struct {
            char text[JR_HTTP_SAY_TEXT_CAP];
        } say;
        jr_http_audio_gain_action_t gain;
    } data;
} jr_http_action_t;

/* Starts the singleton HTTP server and its bounded action queue. This does not
 * generate a pairing token. Call jr_http_pairing_token_ensure() explicitly as
 * part of physical onboarding if protected writes should be enabled. */
esp_err_t jr_http_start(const jr_http_config_t *config);
/* Stops producers, wakes a blocked action poll with ESP_ERR_INVALID_STATE,
 * then deletes the queue. Safe to call while the single consumer is waiting. */
void jr_http_stop(void);
bool jr_http_is_running(void);

/* Poll one action from the app's sole single-writer consumer. timeout_ms=0 is
 * nonblocking; UINT32_MAX waits until an action or jr_http_stop(). Concurrent
 * consumers are rejected with ESP_ERR_INVALID_STATE. */
esp_err_t jr_http_action_poll(jr_http_action_t *out, uint32_t timeout_ms);

/* Explicit physical-onboarding operation. A newly generated token is returned
 * only once and must be presented locally, then cleared by the caller. Existing
 * hashed tokens cannot be recovered and return created=false with an empty
 * token. This function never logs the token. */
esp_err_t jr_http_pairing_token_ensure(
    char token[JR_HTTP_PAIRING_TOKEN_CAP], size_t capacity, bool *created);

#ifdef __cplusplus
}
#endif

#endif /* JR_HTTP_JR_HTTP_H */
