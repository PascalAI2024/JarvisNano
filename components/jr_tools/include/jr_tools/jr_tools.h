/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_tools — bounded asynchronous JarvisMCP function-tool worker.
 *
 * This component is deliberately narrower than a generic code executor. The
 * typed device route accepts only allowlisted tool names and JSON arguments;
 * legacy /act compatibility uses fixed, locally-owned templates. Model-supplied
 * code never crosses either path.
 */
#ifndef JR_TOOLS_JR_TOOLS_H
#define JR_TOOLS_JR_TOOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JR_TOOLS_CALL_ID_TEXT_CAP  96U
#define JR_TOOLS_NAME_CAP          48U
#define JR_TOOLS_ARGS_CAP          768U
#define JR_TOOLS_REQUEST_ID_CAP    97U
/* Leaves bounded headroom for Gemini's toolResponse envelope inside the
 * transport's 4096-byte TX slot, even when the original call id needs JSON
 * escaping. */
#define JR_TOOLS_RESPONSE_CAP      3072U

typedef enum {
    JR_TOOL_STATUS_OK = 0,
    JR_TOOL_STATUS_UNKNOWN_TOOL,
    JR_TOOL_STATUS_INVALID_ARGS,
    JR_TOOL_STATUS_NOT_CONFIGURED,
    JR_TOOL_STATUS_CANCELLED,
    JR_TOOL_STATUS_STALE,
    JR_TOOL_STATUS_NETWORK_ERROR,
    JR_TOOL_STATUS_HTTP_ERROR,
    JR_TOOL_STATUS_BAD_RESPONSE,
    JR_TOOL_STATUS_INTERNAL_ERROR,
} jr_tool_status_t;

typedef struct {
    uint32_t call_id;
    const char *call_id_text; /* Gemini's original id; never substitute hash. */
    const char *name;
    const char *args_json;    /* JSON object; NULL is normalized to {}. */
    uint32_t session_gen;
    /* Set only after an on-device physical approval. This is carried outside
     * model-provided args so Gemini cannot forge a confirmation field. */
    bool physical_confirmed;
} jr_tool_job_t;

typedef struct {
    uint32_t call_id;
    char call_id_text[JR_TOOLS_CALL_ID_TEXT_CAP];
    char name[JR_TOOLS_NAME_CAP];
    uint32_t session_gen;
    jr_tool_status_t status;
    uint32_t duration_ms;
    int http_status; /* 0 unless an HTTP response was received. */
    /* Always a complete JSON object. Errors use
     * {"error":{"code":"...","message":"..."}}. */
    char response_json[JR_TOOLS_RESPONSE_CAP];
} jr_tool_result_t;

typedef struct {
    /* NULL fields are loaded from jr_net typed internal config during init.
     * Empty strings explicitly keep the bridge unconfigured. All values are
     * copied; callers may release their storage after jr_tools_init(). */
    const char *mcp_url;
    const char *mcp_key;
    uint32_t initial_session_gen;
} jr_tools_config_t;

/* Creates the fixed queues and copies/loads configuration. Idempotent. It is
 * valid for the endpoint to be unconfigured; submitted calls then receive a
 * structured JR_TOOL_STATUS_NOT_CONFIGURED result. */
esp_err_t jr_tools_init(const jr_tools_config_t *config);

/* Starts the single persistent worker task. Idempotent and nonblocking. */
esp_err_t jr_tools_start(void);

/* Reload endpoint/key from jr_net internal config. Safe while the worker is
 * running; the worker takes a private snapshot for each HTTPS request. */
esp_err_t jr_tools_reload_config(void);
bool jr_tools_is_configured(void);

/* Nonblocking, deep-copying queue API. ESP_ERR_TIMEOUT means the fixed job
 * queue is full; no caller pointer is retained. */
esp_err_t jr_tools_submit(const jr_tool_job_t *job);

/* session_gen for a job the DEVICE owns (a weather glance, a status probe):
 * never stale, whatever Gemini session is or is not open. A job tagged with
 * the orchestrator's generation is dropped once that session closes, which
 * is right for a model's tool call and wrong for the glass's own fetch —
 * the weather could not refresh on an idle device until this existed. */
#define JR_TOOLS_SESSION_ANY 0xFFFFFFFFu

/* Marks a queued or in-flight call cancelled. Matching prefers the original
 * Gemini id when supplied, avoiding hash-collision ambiguity. The blocking
 * HTTPS operation is not killed cross-task; its result is suppressed and a
 * CANCELLED result is published when the worker regains control. */
esp_err_t jr_tools_cancel(uint32_t call_id, const char *call_id_text);

/* Advancing the generation makes queued/in-flight older jobs stale. */
void jr_tools_set_session_generation(uint32_t session_gen);

/* Returns true and copies one owned result, or false immediately when empty. */
bool jr_tools_poll(jr_tool_result_t *out);

const char *jr_tools_status_name(jr_tool_status_t status);

/* Pure allowlist/template seam for the legacy /act compatibility path. This is
 * public for isolated host tests and diagnostics; it never accepts
 * caller-supplied code. */
typedef enum {
    JR_TOOL_TEMPLATE_OK = 0,
    JR_TOOL_TEMPLATE_UNKNOWN_TOOL,
    JR_TOOL_TEMPLATE_INVALID_ARGS,
    JR_TOOL_TEMPLATE_TOO_LARGE,
    JR_TOOL_TEMPLATE_INTERNAL_ERROR,
} jr_tool_template_status_t;

jr_tool_template_status_t jr_tools_build_code(const char *name,
                                               const char *args_json,
                                               char *out, size_t out_cap);

/* The coordination project the board templates (delegate_task,
 * delegated_tasks, board_poll) address. Not a secret. NULL or "" restores
 * the default; anything outside [A-Za-z0-9._-] or longer than 48 is refused
 * and the previous value stays. Any task; the templates copy it under no
 * lock because a project id changes once per pairing, never per call. */
#define JR_TOOLS_BOARD_PROJECT_CAP     49U
#define JR_TOOLS_BOARD_PROJECT_DEFAULT "jarvisnano-desk"
bool jr_tools_set_board_project(const char *project_id);
const char *jr_tools_board_project(void);

/* Stable idempotency key for physically confirmed mutations. The boot/session
 * namespace prevents provider-local ids (for example, "call_1") from
 * colliding across sessions while retries of the same job remain identical.
 * The output always satisfies [A-Za-z0-9][A-Za-z0-9._:-]{0,95}. */
bool jr_tools_build_request_id(uint32_t boot_nonce, uint32_t session_gen,
                               uint32_t call_id, const char *call_id_text,
                               char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* JR_TOOLS_JR_TOOLS_H */
