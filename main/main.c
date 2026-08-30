/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * main.c — JarvisRobot v5 L6 App Orchestration / Composition Root.
 *
 * The ONLY site in the firmware that constructs concrete adapters and injects
 * them into the pure core. It builds the object graph:
 *
 *   NVS  ->  jr_net (Wi-Fi + config)  ->  jr_audio (codec/AEC/ring)
 *        ->  jr_gemini_ws (device WS byte transport)
 *        ->  jr_gemini_client_t (the host-tested framer/parser)  [L2]
 *        ->  jr_orch_t (the pure single-writer pump)             [L3]
 *
 * then spawns ONE pinned FreeRTOS task that loops jr_orch_step() at frame
 * cadence, streams mic frames up while capturing, and reflects the phase on the
 * idle face. A tiny diag HTTP server exposes the StateSnapshot + /api/debug/say
 * + /api/debug/gain. The core never calls a HAL/net/codec function — it only
 * ever receives these structs.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdatomic.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "cJSON.h"

#include "jr_ports/ports.h"
#include "jr_display/jr_display.h"
#include "jr_display/hud_render.h"
#include "jr_core/session.h"
#include "jr_core/mood.h"
#include "jr_core/orchestrator.h"
#include "jr_core/snapshot.h"
#include "jr_core/turn_policy.h"
#include "jr_dsp/dsp.h"
#include "jr_hal/hal.h"
#include "jr_net/jr_net.h"
#include "jr_audio/jr_audio.h"
#include "jr_tools/jr_tools.h"
#include "jr_transport/gemini_live.h"
#include "jr_transport/gemini_device_ws.h"
#include "jr_imu/jr_imu.h"
#include "jr_power/jr_power.h"
#include "jr_rtc/jr_rtc.h"
#include "jr_wake/jr_wake.h"

static const char *TAG = "jarvis_v5";

extern const unsigned char diagnostics_html_start[]
    asm("_binary_diagnostics_html_start");
extern const unsigned char diagnostics_html_end[]
    asm("_binary_diagnostics_html_end");

/* Gemini Live WSS endpoint (v4-proven). The API key is appended at runtime from
 * NVS — NEVER hardcode it in-repo. */
#define GEMINI_WS_BASE \
    "wss://generativelanguage.googleapis.com/ws/" \
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"

#define VOICE_FRAME_SAMPLES  512   /* 32 ms @ 16 kHz — one AEC chunk / uplink frame */
#define INBOX_CAP            16
/* Brief guard against the model's own acoustic tail right after it stops
 * speaking. Keep this SHORT: it gates the user's reply onset (SPEECH_STARTED),
 * so 1100 ms made the device feel deaf exactly when you want to answer —
 * half-duplex. 400 ms covers the tail while letting a prompt human reply
 * commit. Does NOT gate barge (separate path), so lowering it is safe. */
#define VAD_POST_SPEECH_REFRACTORY_MS  400
#define TOOL_ID_CAP          96
#define TOOL_NAME_CAP        64
#define TOOL_ARGS_CAP        2048
#define VOICE_ALWAYS_READY   1
#define AUDIO_DIAG_CAPTURE_MS 1800U

/* The model sees fixed templates plus two server-policy meta-tools. The worker
 * owns HTTPS on-device; no Android/Mac companion participates in the
 * voice -> tool -> voice path. `remember` is intercepted for physical
 * confirmation, and execute_tool can never bypass JarvisMCP server policy. */
static const jr_gemini_fn_decl_t s_device_tool_fns[] = {
    {
        .name = "recall_memory",
        .description = "Search Pascal's Jarvis memory for relevant context.",
        .arg_name = "query",
        .arg_desc = "A concise natural-language memory search query.",
    },
    {
        .name = "remember",
        .description =
            "Save a note to Jarvis memory after Pascal approves on the device.",
        .arg_name = "note",
        .arg_desc =
            "Panel-safe note, at most 47 characters; it is shown before approval.",
    },
    {
        .name = "current_time",
        .description = "Get the current local time from Jarvis.",
        .arg_name = NULL,
        .arg_desc = NULL,
    },
    {
        .name = "search_tools",
        .description =
            "Search Jarvis capabilities when the fixed tools do not fit.",
        .arg_name = "query",
        .arg_desc =
            "A concise description of the capability needed.",
    },
    {
        .name = "execute_tool",
        .description =
            "Run one capability returned by search_tools through Jarvis policy.",
        .params = {
            {
                .name = "tool",
                .type = JR_GEMINI_PT_STRING,
                .description =
                    "Exact canonical service.method returned by search_tools.",
                .required = true,
            },
            {
                .name = "args_json",
                .type = JR_GEMINI_PT_STRING,
                .description =
                    "A JSON object containing only that tool's documented arguments.",
                .required = true,
            },
        },
        .param_count = JR_GEMINI_PARAM_COUNT(2),
    },
    {
        .name = "set_volume",
        .description =
            "Set Jarvis speaker volume from 10 to 100 and persist it.",
        .params = {{
            .name = "level",
            .type = JR_GEMINI_PT_INTEGER,
            .description = "Speaker volume from 10 to 100.",
            .required = true,
        }},
        .param_count = JR_GEMINI_PARAM_COUNT(1),
    },
    {
        .name = "set_brightness",
        .description =
            "Set the display brightness ceiling from 10 to 100 and persist it.",
        .params = {{
            .name = "level",
            .type = JR_GEMINI_PT_INTEGER,
            .description = "Brightness ceiling from 10 to 100.",
            .required = true,
        }},
        .param_count = JR_GEMINI_PARAM_COUNT(1),
    },
    /* ask_user never reaches the HTTPS tool worker: rich_cb intercepts it and
     * it becomes the Asking substate (choice arcs on glass, STATE-05/06). */
    JR_GEMINI_ASK_USER_DECL,
};

#define DEVICE_TOOL_DECL_COUNT \
    (sizeof(s_device_tool_fns) / sizeof(s_device_tool_fns[0]))
#define LOCAL_TOOL_RESULT_CAP 4U
#define LOCAL_TOOL_ERROR_CAP  160U
#define TOOL_CONSENT_TIMEOUT_MS 15000U

typedef struct {
    uint32_t call_id;
    char call_id_text[JR_TOOLS_CALL_ID_TEXT_CAP];
    char name[JR_TOOLS_NAME_CAP];
    uint32_t session_gen;
    jr_tool_status_t status;
    char response_json[LOCAL_TOOL_ERROR_CAP];
} local_tool_result_t;

typedef struct {
    bool active;
    uint32_t call_id;
    char call_id_text[JR_TOOLS_CALL_ID_TEXT_CAP];
    char name[JR_TOOLS_NAME_CAP];
    char args_json[JR_TOOLS_ARGS_CAP];
    uint32_t session_gen;
    uint32_t expires_ms;
    uint32_t presented_ms;
} pending_tool_consent_t;

typedef struct {
    _Atomic bool worker_ready;
    _Atomic uint32_t last_tool_slot;
    _Atomic bool last_status_valid;
    _Atomic int last_status;
    _Atomic int last_http_status;
    _Atomic uint32_t last_duration_ms;
    _Atomic uint32_t calls_received;
    _Atomic uint32_t submitted;
    _Atomic uint32_t submit_rejected;
    _Atomic uint32_t completed;
    _Atomic uint32_t succeeded;
    _Atomic uint32_t failed;
    _Atomic uint32_t cancelled;
    _Atomic uint32_t stale_dropped;
    _Atomic uint32_t responses_sent;
    _Atomic uint32_t response_send_failed;
    _Atomic bool consent_active;
    _Atomic uint32_t consent_prompted;
    _Atomic uint32_t consent_approved;
    _Atomic uint32_t consent_denied;
    _Atomic uint32_t consent_timed_out;
    _Atomic uint32_t consent_cancelled;
} device_tool_diag_t;

static device_tool_diag_t s_tool_diag;
/* Single-writer voice-task fallback lane for queue/unavailable failures. It
 * holds metadata and a fixed error object only, never a key, endpoint, or MCP
 * result payload. */
static local_tool_result_t s_local_tool_results[LOCAL_TOOL_RESULT_CAP];
static size_t s_local_tool_head;
static size_t s_local_tool_count;
static jr_tool_result_t *s_tool_poll_result;
static pending_tool_consent_t s_tool_consent;

/* The OWNED ask_user snapshot (STATE-05/06). The Gemini parse tree dies the
 * moment rich_cb returns, but the Asking window holds the question and options
 * for up to JR_ASK_TIMEOUT_MS (120 s) — so rich_cb copies into this struct and
 * everything downstream (PresentChoices labels, the CHOICE_PICKED echo text,
 * the functionResponse call id) points into it. All writers and readers run on
 * the app task (rich_cb via voice_poll, voice_exec, the input loop): single
 * writer, no locking. `s_ask_prev` is the one-deep history a re-ask needs — the
 * session answers the OLD call id first, but only AFTER rich_cb has already
 * overwritten the live snapshot with the new ask. */
static jr_gemini_ask_t s_ask;
static jr_gemini_ask_t s_ask_prev;

/* Synthetic tap lane for /api/input/tap: ((x+1)<<16)|(y+1), 0 == none. It is
 * drained ahead of the HAL queue by input_next() and flows through the REAL
 * tap handler, so the ask/consent/shade tap paths can be exercised end-to-end
 * over HTTP; only the CST9217 itself is bypassed (which /api/touch covers). */
static _Atomic uint32_t s_sim_touch;

/* Debug-choices request lane: 0 == none, else requested arc count + 1 (so a
 * dismiss, n == 0, encodes as 1). The httpd handler only posts here; the app
 * task drains it — jr_display's choice statics keep exactly ONE writer. */
static _Atomic uint32_t s_debug_choices_req;

/* App-task only: taps are swallowed until this deadline right after an arc was
 * tapped, so the trailing contact of a double-tap cannot reach the mute
 * toggle the instant the arcs synchronously dismiss. */
static uint32_t s_ask_tap_grace_ms;

/* Synthetic shake lane (GEST-03 verification): number of 10 Hz polls that
 * should read as shake-positive. The endpoint posts 2 — exactly the sustained
 * window the detector demands — so the sim exercises the REAL persistence
 * filter, not a bypass. */
static _Atomic uint32_t s_sim_shake;

/* Synthetic flip lane (GEST-02): number of 10 Hz polls that should read as
 * face_down. The endpoint posts enough to satisfy the sustain filter. */
static _Atomic uint32_t s_sim_flip;

/* Gesture layer (app task only): swipe-right watch peek deadline, and the
 * double-tap window for the attention gesture. */
static uint32_t s_watch_peek_until_ms;
static uint32_t s_last_tap_ms;
/* Hold-to-commit: ms at which the current physical contact was confirmed, or 0
 * when no hold is in flight. App task only. */
static uint32_t s_hold_start_ms;
static _Atomic uint32_t s_pairing_claim_until_ms;
/* Set by the boot-button tick (which runs before most UI statics are declared)
 * and serviced in the app loop, where they are in scope. */
static _Atomic bool s_panic_home_request;
#define PAIRING_CLAIM_WINDOW_MS 60000U

static void boot_button_tick(uint32_t now_ms)
{
#if defined(CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C)
    static bool was_down;
    static uint32_t pressed_ms;
    const bool down = gpio_get_level(GPIO_NUM_0) == 0;
    if (down && !was_down) {
        was_down = true;
        pressed_ms = now_ms;
    } else if (!down && was_down) {
        const uint32_t held_ms = now_ms - pressed_ms;
        was_down = false;
        if (held_ms >= 200U && held_ms < 1500U) {
            if (jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_SHADE) {
                jr_display_nav_up();
                jr_display_caption_set("CONTROLS CLOSED");
            } else {
                jr_display_nav_down();
                /* Was caption_clear(). Opening a capturing surface and then
                 * wiping the one line that could say how to leave it is the
                 * same defect the swipe path shed in 80636399 — it just
                 * survived on the button path. Any surface that captures input
                 * captions its own exit, for as long as it is up. */
                jr_display_caption_set("CONTROLS - UP TO CLOSE");
            }
            ESP_LOGI(TAG, "boot button: controls toggle (%lu ms)",
                     (unsigned long)held_ms);
        } else if (held_ms >= 1500U && held_ms < 5000U) {
            atomic_store(&s_pairing_claim_until_ms,
                         now_ms + PAIRING_CLAIM_WINDOW_MS);
            jr_display_nav_down();
            jr_display_caption_set("PAIRING OPEN - 60 S");
            ESP_LOGI(TAG, "boot button: physical pairing window open "
                          "(%lu ms hold)", (unsigned long)held_ms);
        } else if (held_ms >= 5000U) {
            /* PANIC-HOME. A hold past 5 s used to fall off the end of this
             * chain with NO binding at all — you could hold the button
             * forever and nothing happened, which is the worst possible
             * response to someone who is already lost.
             *
             * It is the one input that must work when the glass is confusing:
             * clear every overlay, drop the pairing window, dismiss any
             * surface, and return to the face. Deliberately on the BUTTON
             * rather than the glass, because a button cannot be swallowed by
             * a modal (docs/INPUT_MAP.md §4). */
            atomic_store(&s_pairing_claim_until_ms, 0U);
            atomic_store(&s_panic_home_request, true);
            ESP_LOGI(TAG, "boot button: panic-home (%lu ms hold)",
                     (unsigned long)held_ms);
        }
    }
#else
    (void)now_ms;
#endif
}

/* Speaker volume (app task only) — gesture-adjustable, NVS-persistent. */
static int s_out_vol = 100;
static uint8_t s_brightness_cap = 100;
static _Atomic int s_level_volume_request = -1;
static _Atomic int s_level_brightness_request = -1;

/* Input events can arrive faster than the app loop applies level requests.
 * Step from the pending value when one exists so rapid taps/swipes accumulate
 * instead of repeatedly publishing the same stale applied value. */
static int request_level_step(_Atomic int *request, int applied, int delta)
{
    const int pending = atomic_load(request);
    int level = (pending >= 10 && pending <= 100) ? pending : applied;
    level += delta;
    if (level > 100) level = 100;
    if (level < 10) level = 10;
    atomic_store(request, level);
    return level;
}
static _Atomic int s_ota_attempt_slot = -1;

static void persist_out_vol(uint8_t vol)
{
    nvs_handle_t h;
    if (nvs_open("app", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, "out_vol", vol);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

static void restore_out_vol(void)
{
    nvs_handle_t h;
    uint8_t vol = 0;
    if (nvs_open("app", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "out_vol", &vol) == ESP_OK &&
            vol >= 10 && vol <= 100) {
            s_out_vol = vol;
            jr_audio_set_gains(-1, -1, (int)vol);
            ESP_LOGI(TAG, "volume restored: %u", (unsigned)vol);
        }
        nvs_close(h);
    }
}

static void persist_brightness_cap(uint8_t cap)
{
    nvs_handle_t h;
    if (nvs_open("app", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, "bright_cap", cap);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

static void restore_brightness_cap(void)
{
    nvs_handle_t h;
    uint8_t cap = 0;
    if (nvs_open("app", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "bright_cap", &cap) == ESP_OK &&
            cap >= 10 && cap <= 100) {
            s_brightness_cap = cap;
            ESP_LOGI(TAG, "brightness cap restored: %u", (unsigned)cap);
        }
        nvs_close(h);
    }
}

static void persist_ota_attempt(int slot)
{
    nvs_handle_t h;
    if (nvs_open("app", NVS_READWRITE, &h) == ESP_OK) {
        if (slot == 0 || slot == 1) {
            (void)nvs_set_i8(h, "ota_attempt", (int8_t)slot);
        } else {
            (void)nvs_erase_key(h, "ota_attempt");
        }
        (void)nvs_commit(h);
        nvs_close(h);
    }
    atomic_store(&s_ota_attempt_slot, slot);
}

static void restore_ota_attempt(void)
{
    nvs_handle_t h;
    int8_t slot = -1;
    if (nvs_open("app", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i8(h, "ota_attempt", &slot) != ESP_OK ||
            (slot != 0 && slot != 1)) {
            slot = -1;
        }
        nvs_close(h);
    }
    atomic_store(&s_ota_attempt_slot, (int)slot);
}

/* ---- persistent log ring ------------------------------------------------
 * Every ESP_LOG line also lands in a 128 KB PSRAM ring served at
 * GET /api/logs?tail=N (the contract live-device.py has always probed for).
 * This is the "read the logs AFTER" channel the owner asked for — the C
 * board has no SD slot (those pins became the display/touch resets), and a
 * network-readable ring beats a card anyway: no wear, no monitor babysitting,
 * no serial-port contention with flashing. Lost on reboot by design; panics
 * persist separately via the coredump partition. */
#define LOGRING_CAP (128U * 1024U)
static char *s_logring;                 /* PSRAM, alloc'd in app_main */
static volatile size_t s_logring_head;  /* next write offset */
static volatile size_t s_logring_len;   /* filled bytes, saturates at CAP */
static portMUX_TYPE s_logring_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_logring_prev;

static int logring_vprintf(const char *fmt, va_list ap)
{
    if (s_logring != NULL) {
        char line[256];
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(line, sizeof line, fmt, ap2);
        va_end(ap2);
        if (n > 0) {
            /* IDF tcp_transport ERROR-logs a full send window as
             * transport_poll_write(0). That is WOULD_BLOCK, already handled.
             * Drop only this line from ring and UART; every other log still
             * reaches s_logring_prev. */
            if (strstr(line, "transport_poll_write(0)") != NULL) {
                return n;
            }
            size_t w = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
            portENTER_CRITICAL(&s_logring_mux);
            size_t head = s_logring_head;
            for (size_t i = 0; i < w; ++i) {
                s_logring[(head + i) % LOGRING_CAP] = line[i];
            }
            s_logring_head = (head + w) % LOGRING_CAP;
            s_logring_len = s_logring_len + w > LOGRING_CAP
                                ? LOGRING_CAP : s_logring_len + w;
            portEXIT_CRITICAL(&s_logring_mux);
        }
    }
    return s_logring_prev != NULL ? s_logring_prev(fmt, ap) : 0;
}

/* Operator mode: Codex/Desk owns the glass and bounded interaction surface.
 * Voice and normal gestures pause for a TTL-bounded window. Single taps belong
 * to the presented tool; double-tap is the physical escape hatch back to
 * always-ready Jarvis. OTA may borrow the lease without entering this mode. */
static _Atomic uint32_t s_operator_lease_until_ms;
static _Atomic bool s_operator_mode_active;
static _Atomic uint32_t s_operator_mode_entered_ms;
static _Atomic bool s_ota_active;
static _Atomic uint32_t s_ota_received_bytes;
static _Atomic uint32_t s_ota_total_bytes;
static _Atomic int s_ota_last_error;
static _Atomic bool s_ota_preflight_blocked;
static _Atomic bool s_http_ready;

static bool operator_lease_active(uint32_t now_ms)
{
    uint32_t until = atomic_load(&s_operator_lease_until_ms);
    return until != 0U && (int32_t)(now_ms - until) < 0;
}

static bool operator_mode_active(uint32_t now_ms)
{
    return atomic_load(&s_operator_mode_active) &&
           operator_lease_active(now_ms);
}

/* Defined next to app_main (it owns the persona text); SEND_SETUP refreshes
 * it so every session's instruction carries the current local time. */
static void compose_system_instruction(void);

/* Attract-reel state (POLISH-06). The httpd handler only posts the request;
 * everything else is app-task single-writer. */
static _Atomic bool s_demo_req;
static uint32_t s_demo_start_ms;   /* app task only; 0 = off */
static int      s_demo_step = -1;
static int      s_demo_last_ripple = -1;

/* WebSocket auth state (see endpoint-auth block in app_main). The header
 * buffer holds the API key and must never be logged; the URI always stays bare. */
static char    s_ws_headers[400];
static bool    s_ws_auth_header_ok_logged;

typedef struct {
    jr_event_t ev;
    char call_id_text[TOOL_ID_CAP];
    char tool_name[TOOL_NAME_CAP];
    char tool_args[TOOL_ARGS_CAP];
} inbound_event_t;

/* ---- mapped inbound event queue (single-threaded: only the app task) ----
 * The slots live in PSRAM (heap, allocated at boot): 16 x ~2.2 KB of tool-call
 * buffers is ~36 KB — as static BSS it starved internal SRAM until the voice
 * task stack (20 KB, internal-only) could no longer allocate. Task-context
 * data only, never touched by ISR/DMA, so external RAM is safe. */
typedef struct {
    inbound_event_t *inq;        /* [INBOX_CAP], PSRAM, alloc'd in app_main */
    size_t     head, count;

    /* ws-level synthetic transition tracking */
    jr_ws_state_t last_ws;
    bool          expect_up;

    /* resume handle bookkeeping (goAway/setupComplete) */
    uint32_t      last_resumable_token;
    uint64_t      last_wire_rx_ms;

    bool          capturing;
    bool          activity_open;
} voice_io_t;

/* ---- the composition graph ---- */
typedef struct {
    jr_clock_t                  clock;
    jr_display_t                display;
    jr_input_t                  input;
    jr_audio_source_t           mic;
    jr_audio_sink_t             spk;

    jr_ws_transport_t           ws;
    jr_gemini_config_t          cfg;
    jr_gemini_client_t          client;
    jr_realtime_voice_client_t  rvc;

    jr_orch_t                   orch;
    jr_turn_policy_t            turn;
    voice_io_t                  io;

    char                        url[512];
    jr_face_t                   last_face;
    uint8_t                     last_amp;
    float                       mic_level;
    float                       playback_level;
    uint64_t                    last_playback_chunk_ms;
    uint32_t                    rx_frames;
    uint32_t                    rx_audio_chunks;
    uint32_t                    rx_audio_samples;
    uint32_t                    rx_audio_dropped_samples;
    uint32_t                    rx_text_parts;
    uint32_t                    rx_turn_complete;
    uint32_t                    rx_generation_complete;
    uint32_t                    rx_errors;
    float                       mic_rms;
    uint32_t                    vad_starts;
    uint32_t                    vad_ends;
    uint32_t                    barge_events;
    uint32_t                    barge_candidates;
    bool                        terminal_pending;
} jr_app_t;

static jr_app_t s_app;
static _Atomic uint32_t s_last_tx_drop_ms;

/* diag: say-mailbox drained by the app task */
static QueueHandle_t s_say_q;   /* of char[200] */

/* A text turn armed via /api/debug/say. It must NOT be sent until the session
 * reaches Listening (transport OPEN + setup complete) — sending into a half-open
 * WS corrupts the turn. voice_task flushes it once, on entering Listening. */
static char        s_pending_text[200];
static volatile bool s_pending_text_set;
static char        s_last_said[192];   /* tail of JARVIS's last spoken transcript */
static uint32_t    s_always_ready_rearm_ms;  /* cooldown gate for idle re-arm */

/* Rolling caption accumulator (app task only). The output transcript arrives
 * as fragments; the on-glass chip (STATE-04) shows the TAIL of the current
 * turn so a reader can follow along. Front-clipped in place — a subtitle, not
 * an archive. */
static char s_caption_acc[128];
static void caption_append(const char *part)
{
    size_t have = strlen(s_caption_acc);
    size_t add = strlen(part);
    if (add >= sizeof s_caption_acc) {
        part += add - (sizeof s_caption_acc - 1U);
        add = sizeof s_caption_acc - 1U;
        have = 0;
    } else if (have + add >= sizeof s_caption_acc) {
        size_t drop = have + add - (sizeof s_caption_acc - 1U);
        memmove(s_caption_acc, s_caption_acc + drop, have - drop + 1U);
        have -= drop;
    }
    memcpy(s_caption_acc + have, part, add + 1U);
    jr_display_caption_set(s_caption_acc);
}
static void caption_reset(void)
{
    s_caption_acc[0] = '\0';
    jr_display_caption_clear();
}

/* ======================================================================== *
 *  VAD / barge diagnostic ring log — records every VAD decision with the    *
 *  numbers that drive barge tuning (mic_rms, floor, gate, peak playback,     *
 *  phase, event). Lives in a bounded PSRAM ring and is dumped as CSV through *
 *  /api/diag/vadlog. It lets a real on-device session be pulled and analysed *
 *  offline instead of guessing from serial; the 1.75C has no persistent SD   *
 *  mirror, so fetch it before reboot when it matters.                         *
 * ======================================================================== */
typedef struct __attribute__((packed)) {
    uint32_t t_ms;
    uint8_t  phase;       /* jr_state_t */
    uint8_t  event;       /* jr_tp_event_t: 0 none/1 start/2 end/3 barge */
    uint8_t  barge_on;    /* was barge enabled this frame */
    uint8_t  _pad;
    int16_t  rms;
    int16_t  floor;
    int16_t  gate;        /* barge gate to beat (0 outside Speaking) */
    int16_t  peak_play;   /* peak-held playback ref */
} vadlog_entry_t;
#define VADLOG_CAP 6000                 /* ~3 min at 32 ms/frame; ~66 KB PSRAM */
static vadlog_entry_t *s_vadlog;
static _Atomic uint32_t s_vadlog_seq;   /* monotonic push count               */

static inline int16_t vl_clip(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

static void vadlog_push(uint32_t t, uint8_t phase, uint8_t ev, bool barge_on,
                        float rms, float floor, float gate, float peak)
{
    if (s_vadlog == NULL) {
        return;
    }
    uint32_t i = atomic_fetch_add(&s_vadlog_seq, 1) % VADLOG_CAP;
    vadlog_entry_t *e = &s_vadlog[i];
    e->t_ms = t;
    e->phase = phase;
    e->event = ev;
    e->barge_on = barge_on ? 1 : 0;
    e->_pad = 0;
    e->rms = vl_clip(rms);
    e->floor = vl_clip(floor);
    e->gate = vl_clip(gate);
    e->peak_play = vl_clip(peak);
}
static jr_state_t  s_last_phase = JR_ST_IDLE;   /* for phase-change diag logging */
static TaskHandle_t s_voice_task;
static volatile bool s_voice_task_running;
static volatile uint32_t s_voice_task_heartbeat_ms;
static _Atomic bool s_voice_start_gate;
static bool s_pending_text_inflight;
static uint32_t s_pending_text_retry_ms;
/* Local barge is opt-in. Live 1.75C evidence showed the AEC residual clearing
 * the local 0.10 gate during model speech, forcing Speaking->Listening->Speaking
 * transitions that sound like hiccups. Gemini server VAD remains active and
 * owns normal interruption; /api/debug/gain?barge=1 is the calibration switch
 * for controlled local-gate experiments. */
static volatile bool s_local_barge_enabled = false;

/* Human/diagnostic controls are produced by HTTP and consumed only by the
 * voice task. The orchestrator therefore remains the sole SessionState writer. */
typedef enum {
    VOICE_CONTROL_NONE = 0,
    VOICE_CONTROL_ARM,       /* explicit physical/API unmute */
    VOICE_CONTROL_RESUME,    /* resume only when privacy permits */
    VOICE_CONTROL_DISARM,    /* deliberate privacy mute */
    VOICE_CONTROL_PAUSE,     /* operational stop; privacy unchanged */
} voice_control_request_t;

static _Atomic int s_voice_control_request;
static _Atomic bool s_voice_privacy_paused;
static jr_mood_state_t s_mood;
static _Atomic uint8_t s_mood_id;
static _Atomic uint8_t s_mood_brightness;
static bool s_mood_rest_disarmed;
/* Hoisted out of the IMU block in voice_task: the tap-to-wake path also needs
 * it, to tell "the mood ladder put us to sleep" (tap may undo) apart from
 * "the user flipped the puck face-down" (tap must NOT undo). */
static bool s_flip_muted;
/* T12: judge voice activity on the AEC-clean (pre-uplink-gain) RMS instead of
 * the 6x-amplified buffer, which scales the room's noise bed up with the voice.
 *
 * CALIBRATED ON HARDWARE against Pascal's actual voice, 2026-08-15:
 *
 *   ambient room   21 - 60   (occasional spike ~105)
 *   his speech    188 - 208
 *   onset gate     85        <- sits cleanly between the two
 *
 * Before this, the VAD judged the amplified buffer, where ambient alone read
 * 200-630 and NEVER fell under the silence threshold (48). Speech therefore
 * latched on and never ended, so no turn was ever committed and the device
 * answered nothing while looking perfectly healthy: armed, capturing, socket
 * open, phase stuck in Listening. Enabling this produced immediate
 * Listening -> Thinking -> Speaking cycles on the same hardware.
 *
 * Default ON. Revert live, without a reflash, via
 * POST /api/debug/gain?vadclean=0 (header X-JarvisNano-Control: 1). */
static _Atomic bool s_vad_use_clean = true;
static uint8_t s_bright_now = 100;
static uint8_t s_bright_tgt = 100;
static bool s_rtc_seeded_os;
static bool s_os_seeded_rtc;
static _Atomic bool s_audio_diag_requested;
static _Atomic uint32_t s_audio_diag_until_ms;
static bool s_listen_speech_active;
static _Atomic bool s_ui_shade_open;

/* Touch observability plus the randomized three-round panel/touch proof. */
static _Atomic uint32_t s_touch_events;
static _Atomic uint32_t s_touch_taps;
static _Atomic uint32_t s_touch_long_presses;
static _Atomic uint32_t s_touch_swipes;
static _Atomic uint32_t s_touch_last_kind;
static _Atomic uint32_t s_touch_last_x;
static _Atomic uint32_t s_touch_last_y;
static _Atomic int s_touch_last_dx;
static _Atomic int s_touch_last_dy;
static _Atomic uint32_t s_touch_last_duration_ms;

static _Atomic bool s_touch_challenge_start_requested;
static _Atomic bool s_touch_challenge_cancel_requested;
static _Atomic bool s_touch_challenge_active;
static _Atomic bool s_touch_challenge_verified;
static _Atomic uint32_t s_touch_challenge_expected;
static _Atomic uint32_t s_touch_challenge_correct;
static _Atomic uint32_t s_touch_challenge_attempts;
static _Atomic uint32_t s_touch_challenge_wrong;
static _Atomic uint32_t s_touch_challenge_last_mapped;
static _Atomic uint32_t s_touch_challenge_last_latency_ms;
static _Atomic uint32_t s_touch_challenge_round_started_ms;
static _Atomic uint32_t s_touch_challenge_restore_ms;

#define AGENT_TASK_ID_CAP   49U
#define AGENT_TITLE_CAP     49U
#define AGENT_SUMMARY_CAP   121U
#define AGENT_EVIDENCE_CAP  4U
#define AGENT_LABEL_CAP     25U
#define AGENT_STATE_CAP     12U

typedef struct {
    char label[AGENT_LABEL_CAP];
    char state[AGENT_STATE_CAP];
} agent_evidence_t;

typedef struct {
    bool active;
    char task_id[AGENT_TASK_ID_CAP];
    uint32_t revision;
    char state[AGENT_STATE_CAP];
    uint8_t progress;
    char title[AGENT_TITLE_CAP];
    char summary[AGENT_SUMMARY_CAP];
    agent_evidence_t evidence[AGENT_EVIDENCE_CAP];
    uint8_t evidence_count;
    uint32_t updated_ms;
    uint32_t expires_ms;
    uint32_t updates;
    uint32_t rejects;
} agent_link_state_t;

static SemaphoreHandle_t s_agent_link_lock;
static agent_link_state_t s_agent_link;
/* Revisions are globally monotonic for the lifetime of this boot. Agent Link
 * has one authenticated writer stream; retaining this high-water mark across
 * task switches prevents an older signed-in payload from becoming current. */
static uint32_t s_agent_link_revision_hwm;

/* Brain Link is the backend-neutral optional companion seam. Gemini and the
 * bounded JarvisMCP worker run directly on the physical device today; a paired
 * Mac/Android companion may additionally present a surface and receive button
 * actions. The device credential remains NVS-only and is never returned here. */
#define BRAIN_SESSION_CAP    33U
#define BRAIN_SURFACE_ID_CAP 33U
#define BRAIN_ACTION_ID_CAP  17U
#define BRAIN_EVENT_CAP      8U
#define BRAIN_DESK_FRESH_MS  15000U

typedef struct {
    bool active;
    bool local_owned;
    char session[BRAIN_SESSION_CAP];
    char id[BRAIN_SURFACE_ID_CAP];
    uint32_t inbox_seq;
    uint32_t expires_ms;
    jr_display_surface_t view;
    char action_ids[JR_DISPLAY_SURFACE_ACTION_CAP][BRAIN_ACTION_ID_CAP];
} brain_surface_state_t;

typedef struct {
    uint32_t seq;
    uint32_t ts_ms;
    char session[BRAIN_SESSION_CAP];
    char id[BRAIN_SURFACE_ID_CAP];
    char action_id[BRAIN_ACTION_ID_CAP];
} brain_action_event_t;

static SemaphoreHandle_t s_brain_lock;
static brain_surface_state_t s_brain_surface;
static brain_action_event_t s_brain_events[BRAIN_EVENT_CAP];
static uint32_t s_brain_inbox_seq_hwm;
static uint32_t s_brain_event_seq;
static uint32_t s_brain_last_seen_ms;

static bool device_wall_time(struct tm *out)
{
    if (out == NULL) {
        return false;
    }
    time_t tt = time(NULL);
    localtime_r(&tt, out);
    if (out->tm_year >= 2020 - 1900) {
        return true;
    }
    if (jr_rtc_get(out) == ESP_OK && out->tm_year >= 2020 - 1900) {
        struct timeval tv = { .tv_sec = mktime(out), .tv_usec = 0 };
        if (tv.tv_sec > 0) {
            (void)settimeofday(&tv, NULL);
            s_rtc_seeded_os = true;
        }
        return true;
    }
    return false;
}

static void device_rtc_capture_os_time(void)
{
    if (s_os_seeded_rtc || !jr_rtc_present()) {
        return;
    }
    time_t tt = time(NULL);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    if (tmv.tm_year >= 2020 - 1900 && jr_rtc_set(&tmv) == ESP_OK) {
        s_os_seeded_rtc = true;
        ESP_LOGI(TAG, "RTC seeded from wall clock");
    }
}

static void secure_zero(void *ptr, size_t size);
static bool brain_render_text_safe(const char *value, size_t capacity,
                                   bool allow_space);

static const char *device_tool_last_status(void)
{
    if (!atomic_load(&s_tool_diag.last_status_valid)) {
        return "idle";
    }
    return jr_tools_status_name(
        (jr_tool_status_t)atomic_load(&s_tool_diag.last_status));
}

static uint32_t device_tool_slot(const char *name)
{
    if (name == NULL) {
        return 0U;
    }
    for (uint32_t i = 0U; i < DEVICE_TOOL_DECL_COUNT; ++i) {
        if (strcmp(name, s_device_tool_fns[i].name) == 0) {
            return i + 1U;
        }
    }
    return 0U;
}

static const char *device_tool_last_name(void)
{
    uint32_t slot = atomic_load(&s_tool_diag.last_tool_slot);
    return slot > 0U && slot <= DEVICE_TOOL_DECL_COUNT
        ? s_device_tool_fns[slot - 1U].name : "none";
}

static void device_tool_record_result(const char *name, jr_tool_status_t status,
                                      uint32_t duration_ms, int http_status)
{
    atomic_store(&s_tool_diag.last_tool_slot, device_tool_slot(name));
    atomic_store(&s_tool_diag.last_status, (int)status);
    atomic_store(&s_tool_diag.last_status_valid, true);
    atomic_store(&s_tool_diag.last_duration_ms, duration_ms);
    atomic_store(&s_tool_diag.last_http_status, http_status);
    atomic_fetch_add(&s_tool_diag.completed, 1U);
    if (status == JR_TOOL_STATUS_OK) {
        atomic_fetch_add(&s_tool_diag.succeeded, 1U);
    } else {
        atomic_fetch_add(&s_tool_diag.failed, 1U);
    }
}

static void device_tool_drop_local_results(void)
{
    if (s_local_tool_count > 0U) {
        atomic_fetch_add(&s_tool_diag.stale_dropped,
                         (uint32_t)s_local_tool_count);
        memset(s_local_tool_results, 0, sizeof(s_local_tool_results));
        s_local_tool_head = 0U;
        s_local_tool_count = 0U;
    }
}

static void device_tool_queue_error(uint32_t call_id, const char *call_id_text,
                                    const char *name, uint32_t session_gen,
                                    jr_tool_status_t status, const char *code,
                                    const char *message)
{
    if (s_local_tool_count >= LOCAL_TOOL_RESULT_CAP) {
        atomic_fetch_add(&s_tool_diag.failed, 1U);
        atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
        return;
    }

    local_tool_result_t *slot = &s_local_tool_results[
        (s_local_tool_head + s_local_tool_count) % LOCAL_TOOL_RESULT_CAP];
    memset(slot, 0, sizeof(*slot));
    slot->call_id = call_id;
    slot->session_gen = session_gen;
    slot->status = status;
    strlcpy(slot->call_id_text, call_id_text ? call_id_text : "",
            sizeof(slot->call_id_text));
    strlcpy(slot->name, name ? name : "unknown",
            sizeof(slot->name));
    (void)snprintf(slot->response_json, sizeof(slot->response_json),
                   "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                   code, message);
    s_local_tool_count++;
}

static void device_tool_queue_local_error(const jr_command_t *cmd,
                                          esp_err_t submit_error)
{
    const char *code = "worker_unavailable";
    const char *message = "On-device tool worker is unavailable";
    jr_tool_status_t status = JR_TOOL_STATUS_INTERNAL_ERROR;
    if (submit_error == ESP_ERR_TIMEOUT) {
        code = "worker_busy";
        message = "On-device tool queue is busy";
    } else if (submit_error == ESP_ERR_INVALID_ARG ||
               submit_error == ESP_ERR_INVALID_SIZE) {
        code = "invalid_call";
        message = "Tool call metadata is invalid";
        status = JR_TOOL_STATUS_INVALID_ARGS;
    }
    device_tool_queue_error(cmd->call_id, cmd->call_id_text, cmd->tool_name,
                            cmd->session_gen, status, code, message);
}

/* Runs only on the single voice owner task, after the orchestrator has
 * completed its current fixpoint. This avoids recursively injecting a result
 * while DispatchToolCall's command list is still being executed. */
static void device_tool_drain_results(jr_app_t *a, uint64_t now)
{
    while (s_local_tool_count > 0U) {
        local_tool_result_t result =
            s_local_tool_results[s_local_tool_head];
        memset(&s_local_tool_results[s_local_tool_head], 0,
               sizeof(s_local_tool_results[s_local_tool_head]));
        s_local_tool_head = (s_local_tool_head + 1U) % LOCAL_TOOL_RESULT_CAP;
        s_local_tool_count--;

        device_tool_record_result(result.name, result.status, 0U, 0);
        if (jr_session_gen_is_stale(a->orch.session.session_gen,
                                    result.session_gen)) {
            atomic_fetch_add(&s_tool_diag.stale_dropped, 1U);
            memset(&result, 0, sizeof(result));
            continue;
        }
        jr_event_t event = jr_event(JR_EV_TOOL_RESULT_READY);
        event.call_id = result.call_id;
        event.call_id_text = result.call_id_text;
        event.tool_name = result.name;
        event.tool_response = result.response_json;
        event.session_gen = result.session_gen;
        jr_orch_inject(&a->orch, event, now);
        memset(&result, 0, sizeof(result));
    }

    if (s_tool_poll_result == NULL) {
        return;
    }
    while (jr_tools_poll(s_tool_poll_result)) {
        jr_tool_result_t *result = s_tool_poll_result;
        device_tool_record_result(result->name, result->status, result->duration_ms,
                                  result->http_status);
        if (result->status == JR_TOOL_STATUS_CANCELLED) {
            atomic_fetch_add(&s_tool_diag.cancelled, 1U);
            memset(result, 0, sizeof(*result));
            continue;
        }
        if (result->status == JR_TOOL_STATUS_STALE ||
            jr_session_gen_is_stale(a->orch.session.session_gen,
                                    result->session_gen)) {
            atomic_fetch_add(&s_tool_diag.stale_dropped, 1U);
            memset(result, 0, sizeof(*result));
            continue;
        }

        jr_event_t event = jr_event(JR_EV_TOOL_RESULT_READY);
        event.call_id = result->call_id;
        event.call_id_text = result->call_id_text;
        event.tool_name = result->name;
        event.tool_response = result->response_json;
        event.session_gen = result->session_gen;
        jr_orch_inject(&a->orch, event, now);
        /* jr_orch_inject synchronously builds/sends the response before it
         * returns, so the owned buffer can be wiped immediately. */
        memset(result, 0, sizeof(*result));
    }
}

typedef enum {
    TOOL_CONSENT_ALLOW = 0,
    TOOL_CONSENT_DENY,
    TOOL_CONSENT_TIMEOUT,
    TOOL_CONSENT_CANCEL,
} tool_consent_outcome_t;

/* s_brain_lock must be held. The panel state and the pending authority are
 * cleared in the same transaction, so no remote writer can slip a replacement
 * card between a physical tap and submission. */
static void device_tool_resolve_consent_locked(tool_consent_outcome_t outcome)
{
    if (!s_tool_consent.active || !s_brain_surface.local_owned) {
        return;
    }

    if (outcome == TOOL_CONSENT_ALLOW) {
        jr_tool_job_t job = {
            .call_id = s_tool_consent.call_id,
            .call_id_text = s_tool_consent.call_id_text,
            .name = s_tool_consent.name,
            .args_json = s_tool_consent.args_json,
            .session_gen = s_tool_consent.session_gen,
            .physical_confirmed = true,
        };
        esp_err_t submitted = atomic_load(&s_tool_diag.worker_ready)
            ? jr_tools_submit(&job) : ESP_ERR_INVALID_STATE;
        atomic_fetch_add(&s_tool_diag.consent_approved, 1U);
        if (submitted == ESP_OK) {
            atomic_fetch_add(&s_tool_diag.submitted, 1U);
        } else {
            jr_command_t failed = {
                .call_id = s_tool_consent.call_id,
                .call_id_text = s_tool_consent.call_id_text,
                .tool_name = s_tool_consent.name,
                .session_gen = s_tool_consent.session_gen,
            };
            atomic_fetch_add(&s_tool_diag.submit_rejected, 1U);
            device_tool_queue_local_error(&failed, submitted);
        }
    } else if (outcome == TOOL_CONSENT_DENY ||
               outcome == TOOL_CONSENT_TIMEOUT) {
        const bool timed_out = outcome == TOOL_CONSENT_TIMEOUT;
        device_tool_queue_error(
            s_tool_consent.call_id, s_tool_consent.call_id_text,
            s_tool_consent.name, s_tool_consent.session_gen,
            JR_TOOL_STATUS_CANCELLED,
            timed_out ? "consent_timeout" : "consent_denied",
            timed_out ? "Physical confirmation timed out"
                      : "User denied physical confirmation");
        atomic_fetch_add(&s_tool_diag.consent_denied, 1U);
        if (timed_out) {
            atomic_fetch_add(&s_tool_diag.consent_timed_out, 1U);
        }
    } else {
        /* Gemini cancelled the function or the session closed. Denial is the
         * safe outcome, but a cancelled Gemini call must not receive a late
         * toolResponse for an id the server has already withdrawn. */
        atomic_fetch_add(&s_tool_diag.consent_cancelled, 1U);
    }

    jr_display_surface_dismiss();
    s_brain_surface.active = false;
    s_brain_surface.local_owned = false;
    secure_zero(&s_tool_consent, sizeof(s_tool_consent));
    atomic_store(&s_tool_diag.consent_active, false);
    configASSERT(!jr_display_surface_is_active());
}

static bool device_tool_present_consent(const jr_command_t *cmd, uint32_t now)
{
    char note[JR_DISPLAY_SURFACE_BODY_CAP] = {0};
    const char *parse_end = NULL;
    cJSON *args = cJSON_ParseWithOpts(cmd->tool_args ? cmd->tool_args : "{}",
                                     &parse_end, true);
    cJSON *note_item = cJSON_GetObjectItemCaseSensitive(args, "note");
    bool note_valid = cJSON_IsObject(args) && args->child == note_item &&
        note_item != NULL && note_item->next == NULL &&
        cJSON_IsString(note_item) && note_item->valuestring != NULL &&
        strnlen(note_item->valuestring, 48U) < 48U &&
        brain_render_text_safe(note_item->valuestring,
                               JR_DISPLAY_SURFACE_BODY_CAP, true);
    if (note_valid) {
        strlcpy(note, note_item->valuestring, sizeof(note));
    }
    if (cJSON_IsString(note_item) && note_item->valuestring != NULL) {
        secure_zero(note_item->valuestring, strlen(note_item->valuestring));
    }
    cJSON_Delete(args);
    if (!note_valid) {
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_INVALID_ARGS, "consent_unrenderable",
            "Note must fit exactly on the physical confirmation panel");
        secure_zero(note, sizeof(note));
        return true;
    }
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_INTERNAL_ERROR, "consent_unavailable",
            "Physical confirmation is unavailable");
        secure_zero(note, sizeof(note));
        return true;
    }
    if (s_tool_consent.active || s_brain_surface.local_owned) {
        xSemaphoreGive(s_brain_lock);
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_CANCELLED, "consent_busy",
            "Another physical confirmation is pending");
        secure_zero(note, sizeof(note));
        return true;
    }

    /* A deterministic test frame suppresses the ordinary surface compositor,
     * and the touch challenge consumes taps before cards. A security prompt
     * must therefore take exclusive local ownership of both lanes. */
    atomic_store(&s_touch_challenge_start_requested, false);
    atomic_store(&s_touch_challenge_cancel_requested, false);
    atomic_store(&s_touch_challenge_active, false);
    atomic_store(&s_touch_challenge_restore_ms, 0U);
    s_ui_shade_open = false;
    if (jr_display_set_test_pattern(JR_DISPLAY_TEST_OFF) != ESP_OK) {
        xSemaphoreGive(s_brain_lock);
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_INTERNAL_ERROR, "consent_unavailable",
            "Physical confirmation display is unavailable");
        secure_zero(note, sizeof(note));
        return true;
    }

    /* Security prompts outrank optional Desk cards. Preemption is atomic under
     * the same Brain -> display lock order used by remote present/dismiss. */
    if (s_brain_surface.active || jr_display_surface_is_active()) {
        jr_display_surface_dismiss();
        s_brain_surface.active = false;
    }
    brain_surface_state_t surface = {0};
    surface.active = true;
    surface.local_owned = true;
    surface.expires_ms = now + TOOL_CONSENT_TIMEOUT_MS;
    surface.view.kind = JR_DISPLAY_SURFACE_CONSENT;
    surface.view.action_count = 2U;
    strlcpy(surface.session, "device", sizeof(surface.session));
    strlcpy(surface.id, "memory-consent", sizeof(surface.id));
    strlcpy(surface.view.title, "SAVE MEMORY?", sizeof(surface.view.title));
    strlcpy(surface.view.body, note, sizeof(surface.view.body));
    strlcpy(surface.action_ids[0], "deny", sizeof(surface.action_ids[0]));
    strlcpy(surface.action_ids[1], "allow", sizeof(surface.action_ids[1]));
    strlcpy(surface.view.action_labels[0], "DENY",
            sizeof(surface.view.action_labels[0]));
    strlcpy(surface.view.action_labels[1], "ALLOW",
            sizeof(surface.view.action_labels[1]));

    esp_err_t shown = jr_display_surface_present(&surface.view);
    if (shown != ESP_OK) {
        xSemaphoreGive(s_brain_lock);
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_INTERNAL_ERROR, "consent_unavailable",
            "Physical confirmation could not be displayed");
        secure_zero(note, sizeof(note));
        secure_zero(&surface, sizeof(surface));
        return true;
    }

    secure_zero(&s_tool_consent, sizeof(s_tool_consent));
    s_tool_consent.active = true;
    s_tool_consent.call_id = cmd->call_id;
    s_tool_consent.session_gen = cmd->session_gen;
    s_tool_consent.expires_ms = surface.expires_ms;
    s_tool_consent.presented_ms = now;
    strlcpy(s_tool_consent.call_id_text,
            cmd->call_id_text ? cmd->call_id_text : "",
            sizeof(s_tool_consent.call_id_text));
    strlcpy(s_tool_consent.name, cmd->tool_name ? cmd->tool_name : "remember",
            sizeof(s_tool_consent.name));
    strlcpy(s_tool_consent.args_json, cmd->tool_args ? cmd->tool_args : "{}",
            sizeof(s_tool_consent.args_json));
    s_brain_surface = surface;
    secure_zero(note, sizeof(note));
    secure_zero(&surface, sizeof(surface));
    atomic_store(&s_tool_diag.consent_active, true);
    atomic_fetch_add(&s_tool_diag.consent_prompted, 1U);
    configASSERT(s_brain_surface.active == jr_display_surface_is_active());
    xSemaphoreGive(s_brain_lock);
    return true;
}

static bool device_tool_cancel_pending_consent(uint32_t call_id,
                                               const char *call_id_text)
{
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool matches = false;
    if (s_tool_consent.active) {
        if (call_id_text != NULL && call_id_text[0] != '\0' &&
            s_tool_consent.call_id_text[0] != '\0') {
            matches = strcmp(call_id_text, s_tool_consent.call_id_text) == 0;
        } else {
            matches = call_id != 0U && call_id == s_tool_consent.call_id;
        }
    }
    if (matches) {
        device_tool_resolve_consent_locked(TOOL_CONSENT_CANCEL);
    }
    xSemaphoreGive(s_brain_lock);
    return matches;
}

static void device_tool_abort_pending_consent(void)
{
    if (s_brain_lock != NULL &&
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_tool_consent.active) {
            device_tool_resolve_consent_locked(TOOL_CONSENT_CANCEL);
        }
        xSemaphoreGive(s_brain_lock);
    }
}

/* ======================================================================== *
 *  inbound event mapping (mirrors host/test_soak.c soak_rich_cb)           *
 * ======================================================================== */
static void inq_push(voice_io_t *io, jr_event_t e)
{
    if (io->inq == NULL || io->count >= INBOX_CAP) {
        return;   /* bounded; drop under a pathological burst */
    }
    inbound_event_t *slot = &io->inq[(io->head + io->count) % INBOX_CAP];
    memset(slot, 0, sizeof *slot);
    slot->ev = e;
    if (e.call_id_text != NULL) {
        strlcpy(slot->call_id_text, e.call_id_text, sizeof slot->call_id_text);
        slot->ev.call_id_text = slot->call_id_text;
    }
    if (e.tool_name != NULL) {
        strlcpy(slot->tool_name, e.tool_name, sizeof slot->tool_name);
        slot->ev.tool_name = slot->tool_name;
    }
    if (e.tool_args != NULL) {
        size_t n = strlcpy(slot->tool_args, e.tool_args, sizeof slot->tool_args);
        if (n >= sizeof slot->tool_args) {
            strlcpy(slot->tool_args,
                    "{\"_jarvis_error\":\"tool arguments exceed device limit\"}",
                    sizeof slot->tool_args);
        }
        slot->ev.tool_args = slot->tool_args;
    }
    io->count++;
}

static jr_error_kind_t map_gem_err(jr_gemini_error_kind_t k)
{
    switch (k) {
    case JR_GEMINI_ERRK_QUOTA:     return JR_ERRK_QUOTA;
    case JR_GEMINI_ERRK_AUTH:      return JR_ERRK_AUTH;
    case JR_GEMINI_ERRK_PROTOCOL:  return JR_ERRK_PROTOCOL;
    case JR_GEMINI_ERRK_TRANSIENT: return JR_ERRK_TRANSIENT;
    default:                       return JR_ERRK_UNKNOWN;
    }
}

static bool handle_local_level_tool(jr_app_t *app,
                                    const jr_gemini_event_t *event)
{
    if (event->tool_name == NULL ||
        (strcmp(event->tool_name, "set_volume") != 0 &&
         strcmp(event->tool_name, "set_brightness") != 0)) {
        return false;
    }

    int level = -1;
    const char *parse_end = NULL;
    cJSON *args = cJSON_ParseWithOpts(
        event->tool_args != NULL ? event->tool_args : "{}",
        &parse_end, true);
    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "level");
    bool valid = args != NULL && cJSON_IsObject(args) &&
        cJSON_IsNumber(item) &&
        item->valuedouble == (double)item->valueint &&
        item->valueint >= 10 && item->valueint <= 100;
    if (valid) {
        level = item->valueint;
        if (strcmp(event->tool_name, "set_volume") == 0) {
            atomic_store(&s_level_volume_request, level);
        } else {
            atomic_store(&s_level_brightness_request, level);
        }
    }
    cJSON_Delete(args);

    char payload[112];
    if (valid) {
        snprintf(payload, sizeof payload,
                 "{\"ok\":true,\"level\":%d,\"persisted\":true}", level);
    } else {
        strlcpy(payload,
            "{\"error\":{\"code\":\"invalid_level\","
            "\"message\":\"level must be an integer from 10 to 100\"}}",
            sizeof payload);
    }
    if (event->call_id_text != NULL && event->call_id_text[0] != '\0') {
        char *frame = jr_gemini_build_tool_response(
            event->call_id_text, event->tool_name, payload);
        if (frame != NULL) {
            (void)jr_gemini_send_frame(&app->client, frame, strlen(frame));
            free(frame);
            atomic_fetch_add(&s_tool_diag.responses_sent, 1U);
        } else {
            atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
        }
    }
    device_tool_record_result(
        event->tool_name,
        valid ? JR_TOOL_STATUS_OK : JR_TOOL_STATUS_INVALID_ARGS,
        0U, 0);
    ESP_LOGI(TAG, "local tool=%s level=%d valid=%d",
             event->tool_name, level, (int)valid);
    return true;
}

/* Map ONE rich transport event to the L3 vocabulary and enqueue it. For audio
 * chunks we feed the playback ring DIRECTLY here (same thread) because the
 * parser frees its decoded pcm the moment jr_gemini_pump_rx returns — so the
 * queued event carries no pcm pointer. */
static void rich_cb(void *u, const jr_gemini_event_t *ge)
{
    jr_app_t *a = (jr_app_t *)u;
    voice_io_t *io = &a->io;
    a->rx_frames++;
    if (ge->kind == JR_GEV_AUDIO_CHUNK) {
        ESP_LOGD(TAG, "rx: audio samples=%u", (unsigned)ge->pcm_len);
    } else if (ge->kind == JR_GEV_RESUMPTION_UPDATE ||
               ge->kind == JR_GEV_UNKNOWN) {
        ESP_LOGD(TAG, "rx: server event kind=%d", (int)ge->kind);
    } else {
        ESP_LOGI(TAG, "rx: server event kind=%d", (int)ge->kind);
    }
    jr_event_t e = jr_event(JR_EV_HEARTBEAT);
    switch (ge->kind) {
    case JR_GEV_SETUP_COMPLETE:
        if (s_ws_headers[0] != '\0' && !s_ws_auth_header_ok_logged) {
            s_ws_auth_header_ok_logged = true;
            ESP_LOGI(TAG, "auth: x-goog-api-key header ACCEPTED — the key is "
                     "out of the URL and out of the logs");
        }
        e = jr_event(JR_EV_SETUP_COMPLETE);
        e.resumption_token = io->last_resumable_token;
        break;
    case JR_GEV_AUDIO_CHUNK:
        a->rx_audio_chunks++;
        a->rx_audio_samples += (uint32_t)ge->pcm_len;
        if (ge->pcm != NULL && ge->pcm_len > 0) {
            a->playback_level = jr_dsp_rms(ge->pcm, ge->pcm_len);
            a->last_playback_chunk_ms = jr_clock_now_ms(&a->clock);
            int accepted = jr_audio_sink_write(&a->spk, ge->pcm,
                                               ge->pcm_len);   /* stage now */
            if (accepted < 0) {
                accepted = 0;
            }
            if ((size_t)accepted < ge->pcm_len) {
                uint32_t dropped = (uint32_t)(ge->pcm_len - (size_t)accepted);
                a->rx_audio_dropped_samples += dropped;
                ESP_LOGW(TAG,
                         "playback ring full: dropped=%u accepted=%u samples",
                         (unsigned)dropped, (unsigned)accepted);
            }
        }
        e = jr_event(JR_EV_SERVER_AUDIO_CHUNK);
        e.pcm = NULL;                      /* already fed; core just transitions */
        e.pcm_len = ge->pcm_len;
        e.sample_rate = ge->sample_rate;
        break;
    case JR_GEV_INTERRUPTED:
        a->terminal_pending = false;
        caption_reset();   /* barged: those words were cut off mid-air */
        e = jr_event(JR_EV_SERVER_INTERRUPTED);
        break;
    case JR_GEV_TURN_COMPLETE:
        a->rx_turn_complete++;
        s_caption_acc[0] = '\0';   /* next turn starts a fresh caption; the
                                    * finished one stays readable until the
                                    * phase leaves Speaking */
        if (jr_audio_playback_pending()) {
            a->terminal_pending = true;
            e = jr_event(JR_EV_HEARTBEAT);
        } else {
            e = jr_event(JR_EV_SERVER_TURN_COMPLETE);
        }
        break;
    case JR_GEV_GENERATION_COMPLETE:
        a->rx_generation_complete++;
        s_caption_acc[0] = '\0';
        if (jr_audio_playback_pending()) {
            a->terminal_pending = true;
            e = jr_event(JR_EV_HEARTBEAT);
        } else {
            e = jr_event(JR_EV_SERVER_TURN_COMPLETE);
        }
        break;
    case JR_GEV_TOOL_CALL:
        if (handle_local_level_tool(a, ge)) {
            e = jr_event(JR_EV_HEARTBEAT);
            break;
        }
        if (ge->tool_name != NULL &&
            strcmp(ge->tool_name, JR_GEMINI_ASK_USER_TOOL) == 0) {
            /* Snapshot NOW — the parse tree dies when this callback returns,
             * and the arcs hold these strings for up to 120 s. Validate into
             * a LOCAL first: a malformed ask arriving mid-ask must not
             * destroy the live snapshot the open question still renders and
             * answers from. */
            jr_gemini_ask_t tmp;
            if (jr_gemini_event_to_ask(ge, &tmp)) {
                if (s_ask.call_id_hash != 0U &&
                    s_ask.call_id_hash != ge->call_id) {
                    /* One frame can batch several ask_user calls; each
                     * rotation can push a still-unanswered ask off the far
                     * end of the two-deep history before the session's
                     * answer-the-old-one command ever executes. Answer it
                     * HERE, at eviction, so no call id is ever stranded —
                     * the later SendChoiceResult miss is then a no-op. */
                    if (s_ask_prev.call_id[0] != '\0' && !s_ask_prev.answered) {
                        ESP_LOGW(TAG, "ask_user: evicting unanswered ask; "
                                 "sending empty answer");
                        char *evf = jr_gemini_build_ask_user_response(
                            s_ask_prev.call_id, NULL);
                        if (evf != NULL) {
                            (void)jr_gemini_send_frame(&a->client, evf,
                                                       strlen(evf));
                            free(evf);
                        }
                    }
                    s_ask_prev = s_ask;
                }
                s_ask = tmp;
                if (s_ask.truncated) {
                    ESP_LOGW(TAG, "ask_user: fields clipped to UI caps");
                }
                ESP_LOGI(TAG, "ask_user: \"%s\" (%u options)",
                         s_ask.question, (unsigned)s_ask.count);
                e = jr_event(JR_EV_ASK_OPENED);
                e.call_id = ge->call_id;
                /* Deliberately NO call_id_text: inq_push would repoint it at
                 * a recycled inbox slot, and the session BORROWS that pointer
                 * for the whole ask. voice_exec resolves hash -> s_ask. */
                break;
            }
            /* Unusable ask (no question, no options, or no text id). Answer
             * immediately with an empty string so the server does not hold
             * the turn open waiting for a functionResponse that can never
             * come. An id-less call cannot even be answered — log and drop. */
            if (ge->call_id_text != NULL && ge->call_id_text[0] != '\0') {
                ESP_LOGW(TAG, "ask_user: malformed call; sending empty answer");
                char *frame =
                    jr_gemini_build_ask_user_response(ge->call_id_text, NULL);
                if (frame != NULL) {
                    (void)jr_gemini_send_frame(&a->client, frame,
                                               strlen(frame));
                    free(frame);
                }
            } else {
                ESP_LOGW(TAG, "ask_user: id-less call; nothing to answer");
            }
            e = jr_event(JR_EV_HEARTBEAT);
            break;
        }
        e = jr_event(JR_EV_SERVER_TOOL_CALL);
        e.call_id = ge->call_id;
        e.call_id_text = ge->call_id_text;
        e.tool_name = ge->tool_name;
        e.tool_args = ge->tool_args;
        break;
    case JR_GEV_TOOL_CANCEL:
        e = jr_event(JR_EV_SERVER_TOOL_CALL);
        e.is_cancellation = true;
        e.call_id = ge->call_id;
        e.call_id_text = ge->call_id_text;
        break;
    case JR_GEV_GO_AWAY:
        e = jr_event(JR_EV_SERVER_GO_AWAY);
        e.resumption_token = io->last_resumable_token;
        break;
    case JR_GEV_RESUMPTION_UPDATE:
        if (ge->resumable && ge->resumption_token) {
            io->last_resumable_token = ge->resumption_token;
        }
        e = jr_event(JR_EV_HEARTBEAT);
        e.resumption_token = ge->resumption_token;
        break;
    case JR_GEV_ERROR:
        a->rx_errors++;
        e = jr_event(JR_EV_SERVER_ERROR);
        e.error_kind = map_gem_err(ge->error_kind);
        e.code = ge->code;
        break;
    case JR_GEV_TEXT:
        a->rx_text_parts++;
        /* Output-audio transcript: what JARVIS actually said. Log it (proof of
         * English), retain the tail for /api/gemini/live diag, and stream it
         * onto the glass as the live caption (STATE-04). */
        if (ge->text && ge->text[0]) {
            ESP_LOGI(TAG, "jarvis says: %.*s", 160, ge->text);
            strlcpy(s_last_said, ge->text, sizeof s_last_said);
            caption_append(ge->text);
        }
        e = jr_event(JR_EV_HEARTBEAT);
        break;
    case JR_GEV_UNKNOWN:
    default:
        e = jr_event(JR_EV_HEARTBEAT);
        break;
    }
    inq_push(io, e);
}

/* ======================================================================== *
 *  the injected jr_orch_io_t                                               *
 * ======================================================================== */

/* poll_inbound: synthesize ws connect/close transitions, pump ONE ws frame
 * through the real parser (rich_cb enqueues mapped events), then hand back the
 * first queued event. Returns false only when nothing is available. */
static bool voice_poll(void *ctx, jr_event_t *out)
{
    jr_app_t *a = (jr_app_t *)ctx;
    voice_io_t *io = &a->io;

    if (io->count == 0) {
        /* (a) ws-level state transitions -> synthetic Connected / TransportClosed */
        if (io->expect_up) {
            jr_ws_state_t st = a->ws.state(a->ws.ctx);
            if (st == JR_WS_OPEN && io->last_ws != JR_WS_OPEN) {
                io->last_ws = JR_WS_OPEN;
                inq_push(io, jr_event(JR_EV_CONNECTED));
            } else if (st == JR_WS_CLOSED || st == JR_WS_ERROR) {
                io->last_ws = st;
                io->expect_up = false;
                inq_push(io, jr_event(JR_EV_TRANSPORT_CLOSED));
            }
        }
        /* WebSocket PONGs are genuine downlink liveness even though they do
         * not carry Gemini JSON. Fold a changed wire timestamp into the core's
         * ordinary heartbeat vocabulary so an application-idle but healthy
         * session is not recycled every 45 seconds. */
        uint64_t wire_rx_ms = a->ws.last_rx_ms
            ? a->ws.last_rx_ms(a->ws.ctx) : 0;
        if (io->count == 0 && wire_rx_ms != 0 &&
            wire_rx_ms != io->last_wire_rx_ms) {
            io->last_wire_rx_ms = wire_rx_ms;
            inq_push(io, jr_event(JR_EV_HEARTBEAT));
        }
        /* (b) drain one inbound server frame into mapped events */
        if (io->count == 0) {
            jr_gemini_pump_rx(&a->client);
        }
    }
    if (io->count == 0) {
        return false;
    }
    *out = io->inq[io->head].ev;
    io->head = (io->head + 1) % INBOX_CAP;
    io->count--;
    return true;
}

/* Resolve the core's uint32 ask handle back to the owned snapshot. The
 * ASK_OPENED event deliberately carries no call_id_text (the inbox would
 * repoint a borrowed string at a recycled slot), so the text id, question and
 * options live only here. Checks the live snapshot first, then the one-deep
 * re-ask history. */
static jr_gemini_ask_t *ask_by_hash(uint32_t call_id)
{
    if (call_id != 0U && s_ask.call_id_hash == call_id &&
        s_ask.call_id[0] != '\0') {
        return &s_ask;
    }
    if (call_id != 0U && s_ask_prev.call_id_hash == call_id &&
        s_ask_prev.call_id[0] != '\0') {
        return &s_ask_prev;
    }
    return NULL;
}

/* exec: run one externally-visible command against the real ports. */
static void voice_exec(void *ctx, const jr_command_t *cmd)
{
    jr_app_t *a = (jr_app_t *)ctx;
    voice_io_t *io = &a->io;
    switch (cmd->kind) {
    case JR_CMD_CONNECT:
        /* Connecting is the generation boundary. In-flight results from an
         * older socket can never cross into this Gemini session. */
        jr_tools_set_session_generation(a->orch.session.session_gen);
        ESP_LOGI(TAG, "exec: CONNECT -> ws.connect(%.40s...) auth=%s", a->url,
                 s_ws_headers[0] != '\0' ? "header" : "none");
        jr_gemini_reset_tx(&a->client);
        io->last_ws = JR_WS_CONNECTING;
        io->expect_up = true;
        if (a->ws.connect(a->ws.ctx, a->url) != JR_OK) {
            ESP_LOGW(TAG, "exec: CONNECT failed synchronously");
        }
        break;
    case JR_CMD_SEND_SETUP: {
        ESP_LOGI(TAG, "exec: SEND_SETUP");
        compose_system_instruction();   /* each session knows what time it is */
        char *setup = jr_gemini_build_setup(&a->cfg);
        if (setup) {
            (void)jr_gemini_send_frame(&a->client, setup, strlen(setup));
            free(setup);
        }
        break;
    }
    case JR_CMD_SEND_ACTIVITY_START:
        if (!io->activity_open) {
            uint32_t drops = a->client.live.tx_drops;
            jr_err_t r = a->rvc.send_control(a->rvc.ctx, JR_RVC_CTRL_ACTIVITY_START);
            if (r == JR_OK ||
                (r == JR_ERR_WOULD_BLOCK && a->client.live.tx_drops == drops)) {
                io->activity_open = true;
            }
        }
        break;
    case JR_CMD_SEND_ACTIVITY_END:
        if (io->activity_open) {
            uint32_t drops = a->client.live.tx_drops;
            jr_err_t r = a->rvc.send_control(a->rvc.ctx, JR_RVC_CTRL_ACTIVITY_END);
            if (r == JR_OK ||
                (r == JR_ERR_WOULD_BLOCK && a->client.live.tx_drops == drops)) {
                io->activity_open = false;
            }
        }
        break;
    case JR_CMD_SEND_AUDIO_STREAM_END:
        a->rvc.send_control(a->rvc.ctx, JR_RVC_CTRL_AUDIO_STREAM_END);
        break;
    case JR_CMD_SEND_TEXT:
        if (cmd->text) {
            a->rvc.send_text(a->rvc.ctx, cmd->text);
        }
        break;
    case JR_CMD_SEND_AUDIO:
        if (cmd->pcm && cmd->pcm_len) {
            a->rvc.send_audio(a->rvc.ctx, cmd->pcm, cmd->pcm_len);
        }
        break;
    case JR_CMD_SEND_TOOL_RESPONSE: {
        if (jr_session_gen_is_stale(a->orch.session.session_gen,
                                    cmd->session_gen) ||
            cmd->call_id_text == NULL || cmd->call_id_text[0] == '\0' ||
            cmd->tool_name == NULL || cmd->tool_name[0] == '\0' ||
            cmd->tool_response == NULL) {
            atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
            break;
        }
        char *frame = jr_gemini_build_tool_response(cmd->call_id_text,
                                                    cmd->tool_name,
                                                    cmd->tool_response);
        if (frame == NULL) {
            atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
            break;
        }
        uint32_t drops_before = a->client.live.tx_drops;
        jr_err_t sent = jr_gemini_send_frame(&a->client, frame, strlen(frame));
        bool accepted = sent == JR_OK ||
            (sent == JR_ERR_WOULD_BLOCK &&
             a->client.live.tx_drops == drops_before);
        free(frame);
        if (accepted) {
            atomic_fetch_add(&s_tool_diag.responses_sent, 1U);
        } else {
            atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
            ESP_LOGW(TAG, "tool response was not accepted by Gemini transport");
        }
        break;
    }
    case JR_CMD_DISPATCH_TOOL_CALL: {
        atomic_fetch_add(&s_tool_diag.calls_received, 1U);
        if (cmd->tool_name != NULL &&
            strcmp(cmd->tool_name, "remember") == 0) {
            /* Validate the exact displayed note before asking; only the later
             * physical tap sets jr_tool_job_t.physical_confirmed. */
            char validation[768];
            jr_tool_template_status_t template_status = jr_tools_build_code(
                cmd->tool_name, cmd->tool_args, validation,
                sizeof(validation));
            secure_zero(validation, sizeof(validation));
            if (template_status == JR_TOOL_TEMPLATE_OK) {
                (void)device_tool_present_consent(cmd,
                    (uint32_t)jr_clock_now_ms(&a->clock));
                break;
            }
            /* Invalid model args contain no useful authority and can go to the
             * worker's bounded parser without troubling the user. */
        }
        jr_tool_job_t job = {
            .call_id = cmd->call_id,
            .call_id_text = cmd->call_id_text,
            .name = cmd->tool_name,
            .args_json = cmd->tool_args,
            .session_gen = cmd->session_gen,
        };
        esp_err_t submitted = atomic_load(&s_tool_diag.worker_ready)
            ? jr_tools_submit(&job) : ESP_ERR_INVALID_STATE;
        if (submitted == ESP_OK) {
            atomic_fetch_add(&s_tool_diag.submitted, 1U);
        } else {
            atomic_fetch_add(&s_tool_diag.submit_rejected, 1U);
            device_tool_queue_local_error(cmd, submitted);
            ESP_LOGW(TAG, "on-device tool dispatch rejected: %s",
                     esp_err_to_name(submitted));
        }
        break;
    }
    case JR_CMD_CANCEL_TOOL_CALL: {
        if (device_tool_cancel_pending_consent(cmd->call_id,
                                               cmd->call_id_text)) {
            break;
        }
        esp_err_t cancelled = jr_tools_cancel(cmd->call_id, cmd->call_id_text);
        if (cancelled != ESP_OK) {
            ESP_LOGW(TAG, "on-device tool cancellation rejected: %s",
                     esp_err_to_name(cancelled));
        }
        break;
    }
    case JR_CMD_CLOSE_TRANSPORT:
        /* Invalidate work immediately, before a reconnect advances the core's
         * generation. The next Connecting state will use exactly gen + 1. */
        jr_tools_set_session_generation(a->orch.session.session_gen + 1U);
        device_tool_abort_pending_consent();
        device_tool_drop_local_results();
        caption_reset();
        io->expect_up = false;
        io->last_ws = JR_WS_CLOSED;
        io->activity_open = false;
        a->terminal_pending = false;
        a->rvc.close(a->rvc.ctx);
        io->head = io->count = 0;   /* drop mapped-but-undrained events */
        break;
    case JR_CMD_START_CAPTURE:
        io->capturing = true;
        break;
    case JR_CMD_PAUSE_CAPTURE:
        io->capturing = false;
        break;
    case JR_CMD_MUTE_DAC_NOW:
        jr_audio_sink_mute_now(&a->spk);
        break;
    case JR_CMD_UNMUTE_DAC:
        jr_audio_dac_unmute();
        break;
    case JR_CMD_FLUSH_PLAYBACK_RING:
        jr_audio_flush_playback();
        break;
    case JR_CMD_FEED_PLAYBACK:
        if (cmd->pcm && cmd->pcm_len) {   /* usually NULL: audio staged in rich_cb */
            jr_audio_sink_write(&a->spk, cmd->pcm, cmd->pcm_len);
        }
        break;
    case JR_CMD_PRESENT_CHOICES: {
        const jr_gemini_ask_t *ask = ask_by_hash(cmd->call_id);
        if (ask == NULL || ask->count == 0U) {
            ESP_LOGW(TAG, "present-choices: no owned ask for id=%08x",
                     (unsigned)cmd->call_id);
            break;
        }
        const char *labels[JR_GEMINI_ASK_USER_MAX_CHOICES];
        for (uint8_t i = 0; i < ask->count; ++i) {
            labels[i] = ask->options[i];
        }
        /* Question + labels are borrowed from the owned snapshot, which lives
         * until a NEW ask overwrites it — always after this one is resolved. */
        jr_display_present_choices(ask->question, labels, (int)ask->count);
        break;
    }
    case JR_CMD_DISMISS_CHOICES:
        jr_display_dismiss_choices();   /* idempotent by contract */
        break;
    case JR_CMD_SEND_CHOICE_RESULT: {
        jr_gemini_ask_t *ask = ask_by_hash(cmd->call_id);
        if (ask == NULL) {
            /* Not an error by itself: the flush-on-evict path in rich_cb may
             * already have answered an ask this command arrives late for. */
            ESP_LOGW(TAG, "choice-result: no owned ask for id=%08x "
                     "(already answered at eviction?)",
                     (unsigned)cmd->call_id);
            break;
        }
        if (ask->answered) {
            break;   /* exactly one functionResponse per call id */
        }
        const char *answer = (cmd->choice_index == JR_CHOICE_NONE)
            ? NULL : cmd->choice_text;
        char *frame = jr_gemini_build_ask_user_response(ask->call_id, answer);
        if (frame == NULL) {
            atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
            break;
        }
        uint32_t drops_before = a->client.live.tx_drops;
        jr_err_t sent = jr_gemini_send_frame(&a->client, frame, strlen(frame));
        bool accepted = sent == JR_OK ||
            (sent == JR_ERR_WOULD_BLOCK &&
             a->client.live.tx_drops == drops_before);
        free(frame);
        if (accepted) {
            ask->answered = true;
            atomic_fetch_add(&s_tool_diag.responses_sent, 1U);
            ESP_LOGI(TAG, "ask answered: \"%s\"", answer ? answer : "");
        } else {
            atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
            ESP_LOGW(TAG, "choice result was not accepted by Gemini transport");
        }
        break;
    }
    default:
        /* EmitDiag / PublishSnapshot / timers: timers are handled inside the
         * orchestrator; the remaining observability commands are no-ops here. */
        break;
    }
}

/* ======================================================================== *
 *  face presentation (phase -> coarse face)                                *
 * ======================================================================== */
static jr_face_t phase_to_face(jr_state_t p)
{
    switch (p) {
    case JR_ST_LISTENING: return JR_FACE_LISTENING;
    case JR_ST_THINKING:
    case JR_ST_CONNECTING:
    case JR_ST_HANDSHAKING:
    case JR_ST_RECONNECTING: return JR_FACE_THINKING;
    case JR_ST_SPEAKING:  return JR_FACE_SPEAKING;
    /* Asking keeps the LISTENING face: the device is waiting on the human, and
     * the choice arcs are drawn over it. Falling through to `default` here sent
     * the face to IDLE at the exact moment the arcs went up — the switch has a
     * default, so adding the enumerator produced no compiler warning and the
     * behaviour changed silently. */
    case JR_ST_ASKING:    return JR_FACE_LISTENING;
    case JR_ST_BACKOFF:
    case JR_ST_FATAL:     return JR_FACE_ERROR;
    default:              return JR_FACE_IDLE;
    }
}

static jr_display_agent_state_t agent_state_to_display(const char *state)
{
    if (strcmp(state, "working") == 0)   return JR_DISPLAY_AGENT_WORKING;
    if (strcmp(state, "verifying") == 0) return JR_DISPLAY_AGENT_VERIFYING;
    if (strcmp(state, "waiting") == 0)   return JR_DISPLAY_AGENT_WAITING;
    if (strcmp(state, "succeeded") == 0) return JR_DISPLAY_AGENT_SUCCEEDED;
    if (strcmp(state, "failed") == 0)    return JR_DISPLAY_AGENT_FAILED;
    return JR_DISPLAY_AGENT_NONE;
}

typedef struct {
    bool ok;
    const char *reason;
    const esp_partition_t *running;
    const esp_partition_t *target;
    uint8_t active_slot;
    uint8_t target_slot;
} ota_preflight_t;

static ota_preflight_t ota_preflight(void)
{
    ota_preflight_t result = {
        .ok = false,
        .reason = "power telemetry unavailable",
        .running = esp_ota_get_running_partition(),
        .target = esp_ota_get_next_update_partition(NULL),
        .active_slot = 0xFFU,
        .target_slot = 0xFFU,
    };
    if (result.running != NULL) {
        if (strcmp(result.running->label, "ota_0") == 0) {
            result.active_slot = 0U;
        } else if (strcmp(result.running->label, "ota_1") == 0) {
            result.active_slot = 1U;
        }
    }
    if (result.target != NULL) {
        result.target_slot =
            strcmp(result.target->label, "ota_0") == 0 ? 0U : 1U;
    } else {
        result.reason = "no inactive OTA slot";
        return result;
    }
    if (!jr_net_is_connected()) {
        result.reason = "network unavailable";
        return result;
    }
    jr_power_t power = {0};
    if (jr_power_read(&power) != ESP_OK) {
        return result;
    }
    if (!power.usb_present &&
        (!power.present || power.percent == 0xFFU || power.percent < 30U)) {
        result.reason = "connect power or charge battery above 30 percent";
        return result;
    }
    if (heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 4096U) {
        result.reason = "insufficient contiguous internal memory";
        return result;
    }
    result.ok = true;
    result.reason = "ready";
    return result;
}

static void publish_shell_state(uint32_t now_ms)
{
    static bool cached_active;
    static uint8_t cached_progress;
    static jr_display_agent_state_t cached_state;
    static char cached_title[13] = "STANDBY";
    static uint32_t next_status_ms;

    if (s_agent_link_lock != NULL &&
        xSemaphoreTake(s_agent_link_lock, 0) == pdTRUE) {
        if (s_agent_link.active &&
            (int32_t)(now_ms - s_agent_link.expires_ms) >= 0) {
        }
        cached_active = s_agent_link.active;
        cached_progress = s_agent_link.progress;
        cached_state = agent_state_to_display(s_agent_link.state);
        strlcpy(cached_title,
                s_agent_link.active && s_agent_link.title[0] != '\0'
                    ? s_agent_link.title : "STANDBY",
                sizeof(cached_title));
        xSemaphoreGive(s_agent_link_lock);
    }
    uint32_t pairing_until = atomic_load(&s_pairing_claim_until_ms);
    if ((int32_t)(pairing_until - now_ms) > 0) {
        cached_active = true;
        cached_progress = 100U;
        cached_state = JR_DISPLAY_AGENT_WAITING;
        strlcpy(cached_title, "PAIR DEVICE", sizeof(cached_title));
    } else if (pairing_until != 0U) {
        atomic_store(&s_pairing_claim_until_ms, 0U);
    }
    if (operator_mode_active(now_ms)) {
        cached_active = true;
        cached_progress = 100U;
        cached_state = JR_DISPLAY_AGENT_WORKING;
        strlcpy(cached_title, "CODEX", sizeof(cached_title));
    }
    jr_display_set_shell_state(s_ui_shade_open, cached_active,
                               cached_progress, cached_state);
    /* DESK is on the mode ring again, so its feed is restored — and unlike
     * TOOLS it was never fake: cached_title/progress/state come from the real
     * agent-link push. TOOLS stays unfed on purpose; it now renders an honest
     * "READY 0 / LAST NONE" instead of the hardcoded
     * {SEARCH, MEMORY, WEATHER, MORE} that never reflected a tool that ran.
     * An empty screen telling the truth beats a full one that lies.
     *
     * The TOOLS feed in particular was fake: a hardcoded
     * {SEARCH, MEMORY, WEATHER, MORE} seeded ONCE behind a latch, with `recent`
     * pinned to 0, so it never reflected a tool that actually ran. That is the
     * "search screen" the owner asked the point of. There wasn't one.
     *
     * cached_title/progress/state also reach the shell via
     * jr_display_set_shell_state(), which draws the agent rim segments. */
    jr_display_desk_set_task(cached_title, cached_progress, cached_state);

    /* TOOLS, with real content at last (N7.14). The petals are the FIRST FOUR
     * ENTRIES OF THE ACTUAL DECLARED CATALOG — s_device_tool_fns, the same
     * array handed to Gemini — and the lit one is the tool that genuinely last
     * executed, from device_tool_last_name()/last_tool_slot.
     *
     * The old feed was a hardcoded {SEARCH, MEMORY, WEATHER, MORE} seeded once
     * with `recent` pinned to 0, so it never reflected anything that ran. This
     * re-derives every pass, so when nothing has run yet NOTHING is lit —
     * which is the honest answer, not a decorative default. */
    {
        const char *tool_names[JR_DISPLAY_TOOLS_MAX];
        int tool_n = (int)DEVICE_TOOL_DECL_COUNT;
        if (tool_n > JR_DISPLAY_TOOLS_MAX) {
            tool_n = JR_DISPLAY_TOOLS_MAX;
        }
        for (int i = 0; i < tool_n; ++i) {
            tool_names[i] = s_device_tool_fns[i].name;
        }
        const uint32_t slot = atomic_load(&s_tool_diag.last_tool_slot);
        /* slot is 1-based; 0 means nothing has run. -1 lights no petal. */
        const int recent = (slot > 0U && (int)slot <= tool_n)
                               ? (int)slot - 1 : -1;
        jr_display_tools_set(tool_names, tool_n, recent);
    }
    const jr_state_snapshot_t *snapshot = jr_orch_snapshot(&s_app.orch);
    jr_display_jarvis_set_session(
        s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN,
        (uint16_t)(snapshot->transitions / 2U), now_ms / 1000U);
    if ((int32_t)(now_ms - next_status_ms) >= 0) {
        wifi_ap_record_t ap = {0};
        const bool net_up = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        jr_display_set_status(
            (uint8_t)s_out_vol, net_up, net_up ? ap.rssi : 0,
            (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));

        const ota_preflight_t preflight = ota_preflight();
        const esp_partition_t *running = preflight.running;
        esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
        const bool have_image_state = running != NULL &&
            esp_ota_get_state_partition(running, &image_state) == ESP_OK;
        const uint8_t slot = preflight.active_slot;
        jr_display_ota_state_t ota_display = JR_DISPLAY_OTA_IDLE;
        uint8_t ota_percent = 0U;
        const uint32_t ota_total = atomic_load(&s_ota_total_bytes);
        if (preflight.ok) {
            atomic_store(&s_ota_preflight_blocked, false);
        }
        if (atomic_load(&s_ota_active)) {
            ota_display = JR_DISPLAY_OTA_RECEIVING;
            if (ota_total > 0U) {
                const uint64_t scaled =
                    (uint64_t)atomic_load(&s_ota_received_bytes) * 100U;
                ota_percent = (uint8_t)(scaled / ota_total);
            }
        } else if (atomic_load(&s_ota_preflight_blocked)) {
            ota_display = JR_DISPLAY_OTA_BLOCKED;
        } else if (atomic_load(&s_ota_last_error) != ESP_OK) {
            ota_display = JR_DISPLAY_OTA_FAILED;
        } else if (have_image_state &&
                   image_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ota_display = JR_DISPLAY_OTA_PROBATION;
        } else if (have_image_state && image_state == ESP_OTA_IMG_VALID) {
            const int attempted = atomic_load(&s_ota_attempt_slot);
            const esp_partition_t *last_invalid =
                esp_ota_get_last_invalid_partition();
            if ((attempted == 0 || attempted == 1) &&
                slot != (uint8_t)attempted && last_invalid != NULL) {
                ota_display = JR_DISPLAY_OTA_ROLLED_BACK;
            } else {
                ota_display = JR_DISPLAY_OTA_VALID;
            }
            ota_percent = 100U;
        }
        jr_display_ota_set(
            ota_display, ota_percent, slot, preflight.target_slot,
            preflight.ok);
        next_status_ms = now_ms + 1000U;
    }
}

/* Map a PCM RMS (jr_dsp_rms: 0..32768) to the HUD's 0..255 amplitude with
 * sqrt compression so quiet speech still moves the ring. k is per-lane:
 * mic frames (~85 RMS at speech onset per the tuned VAD) use k=8; Gemini
 * 24 kHz playback (typ. 2000-8000 RMS) uses k=3.2. */
static uint8_t rms_to_amp(float rms, float k)
{
    if (rms <= 1.0f) {
        return 0;
    }
    float a = k * sqrtf(rms);
    return a >= 255.0f ? 255 : (uint8_t)a;
}

/* ======================================================================== *
 *  diag HTTP: snapshot + /api/debug/say + /api/debug/gain                  *
 * ======================================================================== */
static bool agent_require_auth(httpd_req_t *req);

static esp_err_t diag_get_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    const jr_state_snapshot_t *s = jr_orch_snapshot(&s_app.orch);
    uint64_t now = jr_clock_now_ms(&s_app.clock);
    uint32_t now32 = (uint32_t)now;
    bool voice_alive = s_voice_task_running &&
                       (uint32_t)(now32 - s_voice_task_heartbeat_ms) < 1000;
    jr_state_t phase = jr_orch_phase(&s_app.orch);
    bool voice_armed = phase != JR_ST_IDLE && phase != JR_ST_DRAINING &&
                       phase != JR_ST_FATAL;
    /* Always 0: this firmware is always-ready listening (VOICE_ALWAYS_READY),
     * so there is no listen window to count down. The deadline it used to read
     * was assigned only ever 0 at four sites. Kept as a field so existing
     * tooling keeps parsing; do not build a countdown rim on it. */
    const uint32_t auto_idle_ms = 0U;
    uint32_t audio_until = atomic_load(&s_audio_diag_until_ms);
    bool audio_diag_running = (int32_t)(audio_until - now32) > 0;
    unsigned stack_hwm = s_voice_task ?
        (unsigned)uxTaskGetStackHighWaterMark(s_voice_task) : 0;
    bool tools_ready = atomic_load(&s_tool_diag.worker_ready);
    bool tools_configured = tools_ready && jr_tools_is_configured();
    /* Transcript tail, defanged for direct embedding: quotes, backslashes and
     * control bytes become spaces. Lossy on purpose — this is a diag peek, not
     * a transcript API. */
    char said_safe[128];   /* httpd stack is the tight one; clip, don't grow */
    for (size_t i = 0; i < sizeof said_safe; ++i) {
        char ch = s_last_said[i];
        said_safe[i] = (ch != '\0' && (ch == '"' || ch == '\\' ||
                        (unsigned char)ch < 0x20)) ? ' ' : ch;
        if (ch == '\0') break;
    }
    said_safe[sizeof said_safe - 1] = '\0';
    char buf[2304];
    int n = snprintf(buf, sizeof buf,
        "{\"phase\":\"%s\",\"transitions\":%u,\"deaths\":%u,\"reconnects\":%u,"
        "\"fail_count\":%u,\"last_reason\":\"%s\",\"last_error_kind\":%d,"
        "\"aec_us\":%u,\"ws_connected\":%s,\"capturing\":%s,"
        "\"voice_armed\":%s,\"always_ready\":true,"
        "\"privacy_paused\":%s,\"auto_idle_ms\":%u,\"shade_open\":%s,"
        "\"audio_diag_queued\":%s,\"audio_diag_running\":%s,"
        "\"uptime_ms\":%llu,\"voice_task_running\":%s,\"voice_task_alive\":%s,"
        "\"voice_stack_hwm\":%u,\"free_internal_heap\":%u,"
        "\"largest_internal_block\":%u,\"free_psram\":%u,"
        "\"tx_queue_depth\":%u,\"tx_would_block\":%u,\"tx_drops\":%u,"
        "\"rx_parse_errors\":%u,\"rx_alloc_failures\":%u,\"mic_rms\":%.1f,"
        "\"vad_floor\":%.1f,\"vad_starts\":%u,\"vad_ends\":%u,"
        "\"barge_enabled\":%s,\"barge_candidates\":%u,\"barge_events\":%u,"
        "\"activity_open\":%s,\"playback_pending\":%s,\"dac_muted\":%s,"
        "\"terminal_pending\":%s,"
        "\"rx_frames\":%u,\"audio_chunks\":%u,"
        "\"audio_samples\":%u,\"audio_dropped_samples\":%u,"
        "\"text_parts\":%u,\"turn_complete\":%u,"
        "\"generation_complete\":%u,\"server_errors\":%u,"
        "\"last_said\":\"%.120s\","
        "\"tools\":{\"execution\":\"on_device\",\"worker_ready\":%s,"
        "\"configured\":%s,\"declared\":%u,\"last_tool\":\"%s\","
        "\"last_status\":\"%s\","
        "\"last_http_status\":%d,\"last_duration_ms\":%u,"
        "\"calls_received\":%u,\"submitted\":%u,\"submit_rejected\":%u,"
        "\"completed\":%u,\"succeeded\":%u,\"failed\":%u,"
        "\"cancelled\":%u,\"stale_dropped\":%u,\"responses_sent\":%u,"
        "\"response_send_failed\":%u,\"consent_active\":%s,"
        "\"consent_prompted\":%u,\"consent_approved\":%u,"
        "\"consent_denied\":%u,\"consent_timed_out\":%u,"
        "\"consent_cancelled\":%u}}",
        jr_state_name(s->phase), (unsigned)s->transitions, (unsigned)s->deaths,
        (unsigned)s->reconnects, (unsigned)s->fail_count,
        jr_event_name(s->last_reason), (int)s->last_error_kind,
        (unsigned)jr_audio_last_aec_us(),
        s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN ? "true" : "false",
        s_app.io.capturing ? "true" : "false",
        voice_armed ? "true" : "false",
        atomic_load(&s_voice_privacy_paused) ? "true" : "false",
        (unsigned)auto_idle_ms,
        s_ui_shade_open ? "true" : "false",
        atomic_load(&s_audio_diag_requested) ? "true" : "false",
        audio_diag_running ? "true" : "false",
        (unsigned long long)now,
        s_voice_task_running ? "true" : "false",
        voice_alive ? "true" : "false",
        stack_hwm,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)jr_gemini_txq_depth(&s_app.client),
        (unsigned)s_app.client.live.tx_would_block,
        (unsigned)s_app.client.live.tx_drops,
        (unsigned)s_app.client.live.rx_parse_errors,
        (unsigned)s_app.client.live.rx_alloc_failures,
        (double)s_app.mic_rms,
        (double)s_app.turn.noise_floor,
        (unsigned)s_app.vad_starts,
        (unsigned)s_app.vad_ends,
        s_local_barge_enabled ? "true" : "false",
        (unsigned)s_app.barge_candidates,
        (unsigned)s_app.barge_events,
        s_app.io.activity_open ? "true" : "false",
        jr_audio_playback_pending() ? "true" : "false",
        jr_audio_dac_muted() ? "true" : "false",
        s_app.terminal_pending ? "true" : "false",
        (unsigned)s_app.rx_frames,
        (unsigned)s_app.rx_audio_chunks,
        (unsigned)s_app.rx_audio_samples,
        (unsigned)s_app.rx_audio_dropped_samples,
        (unsigned)s_app.rx_text_parts,
        (unsigned)s_app.rx_turn_complete,
        (unsigned)s_app.rx_generation_complete,
        (unsigned)s_app.rx_errors,
        said_safe,
        tools_ready ? "true" : "false",
        tools_configured ? "true" : "false",
        (unsigned)DEVICE_TOOL_DECL_COUNT,
        device_tool_last_name(),
        device_tool_last_status(),
        atomic_load(&s_tool_diag.last_http_status),
        (unsigned)atomic_load(&s_tool_diag.last_duration_ms),
        (unsigned)atomic_load(&s_tool_diag.calls_received),
        (unsigned)atomic_load(&s_tool_diag.submitted),
        (unsigned)atomic_load(&s_tool_diag.submit_rejected),
        (unsigned)atomic_load(&s_tool_diag.completed),
        (unsigned)atomic_load(&s_tool_diag.succeeded),
        (unsigned)atomic_load(&s_tool_diag.failed),
        (unsigned)atomic_load(&s_tool_diag.cancelled),
        (unsigned)atomic_load(&s_tool_diag.stale_dropped),
        (unsigned)atomic_load(&s_tool_diag.responses_sent),
        (unsigned)atomic_load(&s_tool_diag.response_send_failed),
        atomic_load(&s_tool_diag.consent_active) ? "true" : "false",
        (unsigned)atomic_load(&s_tool_diag.consent_prompted),
        (unsigned)atomic_load(&s_tool_diag.consent_approved),
        (unsigned)atomic_load(&s_tool_diag.consent_denied),
        (unsigned)atomic_load(&s_tool_diag.consent_timed_out),
        (unsigned)atomic_load(&s_tool_diag.consent_cancelled));
    if (n < 0 || (size_t)n >= sizeof buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status encoding failed");
        return ESP_OK;
    }
    size_t response_len = (size_t)n;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, response_len);
    return ESP_OK;
}

static esp_err_t dashboard_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'self'; style-src 'self' 'unsafe-inline'; "
        "script-src 'self' 'unsafe-inline'; connect-src 'self'; "
        "img-src 'none'; object-src 'none'; base-uri 'none'; "
        "form-action 'none'; frame-ancestors 'none'");
    size_t length = (size_t)(diagnostics_html_end - diagnostics_html_start);
    if (length > 0U && diagnostics_html_start[length - 1U] == '\0') {
        length--;
    }
    return httpd_resp_send(req, (const char *)diagnostics_html_start, length);
}

static bool query_int(httpd_req_t *req, const char *key, int *out)
{
    char q[256];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) {
        return false;
    }
    char v[32];
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) {
        return false;
    }
    *out = atoi(v);
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* esp_http_server extracts the raw query value but does not percent-decode it.
 * Decode in place so the diagnostic endpoint sends the words the caller
 * supplied rather than making Gemini pronounce "%20". */
static bool url_decode_in_place(char *s)
{
    char *src = s;
    char *dst = s;
    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        if (*src == '%') {
            int hi = hex_nibble(src[1]);
            int lo = src[1] != '\0' ? hex_nibble(src[2]) : -1;
            if (hi < 0 || lo < 0) {
                return false;
            }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    return true;
}

/* Mutating diagnostics remain intentionally trusted-LAN rather than bearer
 * authenticated, but they are POST-only and require a non-simple intent
 * header. That blocks link prefetchers and drive-by cross-origin form/fetch
 * requests while keeping the bench CLI and same-origin cockpit usable. */
static bool control_intent_required(httpd_req_t *req)
{
    static const char *header = "X-JarvisNano-Control";
    char value[8] = {0};
    size_t length = httpd_req_get_hdr_value_len(req, header);
    if (length != 1U ||
        httpd_req_get_hdr_value_str(req, header, value, sizeof value) != ESP_OK ||
        strcmp(value, "1") != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"explicit local control intent required\"}");
        return false;
    }
    if (atomic_load(&s_tool_diag.consent_active) &&
        strcmp(req->uri, "/api/brain/inbox") != 0) {
        httpd_resp_set_status(req, "423 Locked");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"physical consent owns controls\"}");
        return false;
    }
    return true;
}

static esp_err_t say_get_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char q[256];
    char text[200] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK ||
        httpd_query_key_value(q, "text", text, sizeof text) != ESP_OK ||
        text[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?text=");
        return ESP_OK;
    }
    if (!url_decode_in_place(text) || text[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ?text= encoding");
        return ESP_OK;
    }
    /* Hand off to the single-writer app task. Never report success when the
     * queue/task is unavailable; diagnostics must describe reality. */
    if (s_say_q == NULL || !s_voice_task_running ||
        xQueueSend(s_say_q, text, 0) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"voice queue unavailable\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t gain_get_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    int mic = -1, ref = -1, vol = -1, barge = -1, vadclean = -1, pbgain = -1;
    int speakmic = -1;
    query_int(req, "mic", &mic);
    query_int(req, "ref", &ref);
    query_int(req, "vol", &vol);
    query_int(req, "barge", &barge);
    query_int(req, "vadclean", &vadclean);
    query_int(req, "pbgain", &pbgain);
    query_int(req, "speakmic", &speakmic);
    jr_audio_set_gains(mic, ref, vol);
    if (speakmic >= 0) {
        jr_audio_set_speak_mic_db(speakmic);
    }
    if (barge >= 0) {
        s_local_barge_enabled = barge != 0;
    }
    if (vadclean >= 0) {
        atomic_store(&s_vad_use_clean, vadclean != 0);
    }
    if (pbgain >= 0) {
        jr_audio_set_playback_gain_percent(pbgain);
    }
    char buf[224];
    int n = snprintf(buf, sizeof buf,
                     "{\"ok\":true,\"mic\":%d,\"ref\":%d,\"vol\":%d,"
                     "\"barge\":%s,\"vadclean\":%s,\"pbgain\":%d}",
                     mic, ref, vol, s_local_barge_enabled ? "true" : "false",
                     atomic_load(&s_vad_use_clean) ? "true" : "false",
                     jr_audio_playback_gain_percent());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t device_levels_get_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    char body[96];
    int n = snprintf(body, sizeof body,
                     "{\"volume\":%d,\"brightness\":%u}",
                     s_out_vol, (unsigned)s_brightness_cap);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t device_levels_post_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    int volume = -1;
    int brightness = -1;
    const bool have_volume = query_int(req, "volume", &volume);
    const bool have_brightness = query_int(req, "brightness", &brightness);
    if ((!have_volume && !have_brightness) ||
        (have_volume && (volume < 10 || volume > 100)) ||
        (have_brightness && (brightness < 10 || brightness > 100))) {
        httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "volume/brightness must be 10..100");
        return ESP_OK;
    }
    if (have_volume) {
        atomic_store(&s_level_volume_request, volume);
    }
    if (have_brightness) {
        atomic_store(&s_level_brightness_request, brightness);
    }
    char body[112];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"volume\":%d,\"brightness\":%d,"
        "\"privacy_unchanged\":true}",
        have_volume ? volume : -1,
        have_brightness ? brightness : -1);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static const char *ota_image_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static esp_err_t device_health_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const jr_state_t phase = jr_orch_phase(&s_app.orch);
    const bool privacy = atomic_load(&s_voice_privacy_paused);
    const bool operator_active = operator_mode_active(now);
    const jr_state_snapshot_t *snapshot = jr_orch_snapshot(&s_app.orch);
    const bool voice_alive = s_voice_task_running &&
        (uint32_t)(now - s_voice_task_heartbeat_ms) < 2000U;
    const uint32_t largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t free_psram =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    jr_display_diag_t display = {0};
    (void)jr_display_get_diag(&display);
    const bool tool_status_valid =
        atomic_load(&s_tool_diag.last_status_valid);
    const jr_tool_status_t tool_status = tool_status_valid
        ? (jr_tool_status_t)atomic_load(&s_tool_diag.last_status)
        : JR_TOOL_STATUS_OK;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *last_invalid =
        esp_ota_get_last_invalid_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    const bool ota_state_known = running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK;
    const int ota_last_error = atomic_load(&s_ota_last_error);
    const uint32_t last_tx_drop_ms = atomic_load(&s_last_tx_drop_ms);
    const bool recent_tx_drop = last_tx_drop_ms != 0U &&
        (uint32_t)(now - last_tx_drop_ms) < 10000U;

    const char *verdict = "ok";
    bool repairable = false;
    if (privacy || s_flip_muted) {
        verdict = "privacy-muted";
    } else if (operator_active) {
        verdict = "operator-active";
    } else if (!voice_alive ||
               ((phase == JR_ST_IDLE || phase == JR_ST_BACKOFF ||
                 phase == JR_ST_FATAL) && !s_mood_rest_disarmed)) {
        verdict = "session-dead";
        repairable = true;
    } else if (recent_tx_drop) {
        verdict = "uplink-dropping";
    } else if (largest < 8192U || free_psram < 2U * 1024U * 1024U) {
        verdict = "memory-critical";
    } else if (display.flush_errors > 0U || display.actual_fps < 12U) {
        verdict = "display-fault";
    } else if (jr_audio_dac_muted()) {
        verdict = "audio-fault";
        repairable = true;
    } else if (tool_status_valid && tool_status != JR_TOOL_STATUS_OK) {
        verdict = "tool-fault";
    }

    char body[960];
    int n = snprintf(body, sizeof body,
        "{\"verdict\":\"%s\",\"repairable\":%s,"
        "\"privacy\":%s,\"flip_muted\":%s,\"operator\":%s,"
        "\"phase\":\"%s\",\"voice_alive\":%s,"
        "\"levels\":{\"volume\":%d,\"brightness_cap\":%u,"
        "\"brightness_actual\":%u},"
        "\"memory\":{\"largest_internal\":%u,\"free_psram\":%u},"
        "\"transport\":{\"would_block\":%u,\"drops\":%u,\"deaths\":%u},"
        "\"display\":{\"fps\":%u,\"flush_errors\":%u},"
        "\"ota\":{\"active\":%s,\"running\":\"%s\",\"boot\":\"%s\","
        "\"state\":\"%s\",\"received\":%u,\"total\":%u,"
        "\"last_error\":\"%s\",\"last_invalid\":\"%s\"},"
        "\"tools\":{\"status\":\"%s\",\"http\":%d}}",
        verdict, repairable ? "true" : "false",
        privacy ? "true" : "false",
        s_flip_muted ? "true" : "false",
        operator_active ? "true" : "false",
        jr_state_name(phase), voice_alive ? "true" : "false",
        s_out_vol, (unsigned)s_brightness_cap,
        (unsigned)atomic_load(&s_mood_brightness),
        (unsigned)largest, (unsigned)free_psram,
        (unsigned)s_app.client.live.tx_would_block,
        (unsigned)s_app.client.live.tx_drops,
        (unsigned)snapshot->deaths,
        (unsigned)display.actual_fps, (unsigned)display.flush_errors,
        atomic_load(&s_ota_active) ? "true" : "false",
        running != NULL ? running->label : "unknown",
        boot != NULL ? boot->label : "unknown",
        ota_state_known ? ota_image_state_name(ota_state) : "unknown",
        (unsigned)atomic_load(&s_ota_received_bytes),
        (unsigned)atomic_load(&s_ota_total_bytes),
        esp_err_to_name((esp_err_t)ota_last_error),
        last_invalid != NULL ? last_invalid->label : "none",
        jr_tools_status_name(tool_status),
        tool_status_valid ? atomic_load(&s_tool_diag.last_http_status) : 0);
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "health response too large");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t voice_control_handler(httpd_req_t *req)
{
    if (!control_intent_required(req) || !agent_require_auth(req)) {
        return ESP_OK;
    }
    int armed = -1;
    int resume = -1;
    const bool have_armed = query_int(req, "armed", &armed);
    const bool have_resume = query_int(req, "resume", &resume);
    if (have_resume && resume == 1) {
        atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"ok\":true,\"queued\":true,\"resume\":\"privacy-safe\"}");
        return ESP_OK;
    }
    if (!have_armed || (armed != 0 && armed != 1) || have_resume) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                           "use ?armed=0|1 or paired ?resume=1");
        return ESP_OK;
    }
    atomic_store(&s_voice_control_request,
                 armed ? VOICE_CONTROL_RESUME : VOICE_CONTROL_DISARM);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, armed
        ? "{\"ok\":true,\"queued\":true,\"resume\":\"privacy-safe\"}"
        : "{\"ok\":true,\"queued\":true,\"armed\":false}");
    return ESP_OK;
}

static const char *audio_tap_name(jr_audio_tap_kind_t kind)
{
    switch (kind) {
    case JR_AUDIO_TAP_MIC_CLEAN: return "mic-clean";
    case JR_AUDIO_TAP_MIC_RAW:   return "mic-raw";
    case JR_AUDIO_TAP_REFERENCE: return "reference";
    case JR_AUDIO_TAP_PLAYBACK:  return "playback";
    default:                     return "unknown";
    }
}

static bool audio_tap_parse(const char *name, jr_audio_tap_kind_t *kind)
{
    for (int i = 0; i < JR_AUDIO_TAP_COUNT; ++i) {
        if (strcmp(name, audio_tap_name((jr_audio_tap_kind_t)i)) == 0) {
            *kind = (jr_audio_tap_kind_t)i;
            return true;
        }
    }
    return false;
}

static esp_err_t audio_self_test_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    if (atomic_load(&s_voice_privacy_paused) || s_flip_muted) {
        httpd_resp_set_status(req, "423 Locked");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"privacy blocks self-test\"}");
        return ESP_OK;
    }
    bool already_queued = atomic_exchange(&s_audio_diag_requested, true);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, already_queued
        ? "{\"ok\":true,\"queued\":true,\"already_queued\":true}"
        : "{\"ok\":true,\"queued\":true,\"capture_ms\":1800,"
          "\"evidence\":[\"electrical_reference\",\"acoustic_microphone\"]}");
    return ESP_OK;
}

static esp_err_t audio_taps_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    jr_audio_tap_info_t info[JR_AUDIO_TAP_COUNT];
    for (int i = 0; i < JR_AUDIO_TAP_COUNT; ++i) {
        esp_err_t err = jr_audio_diag_get_info((jr_audio_tap_kind_t)i, &info[i]);
        if (err != ESP_OK) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_type(req, "application/json");
            char body[128];
            int n = snprintf(body, sizeof body,
                             "{\"available\":false,\"error\":\"%s\"}",
                             esp_err_to_name(err));
            httpd_resp_send(req, body, n);
            return ESP_OK;
        }
    }

    char body[1024];
    size_t used = 0;
    int n = snprintf(body, sizeof body,
                     "{\"available\":true,\"capture_source\":"
                     "\"codec_single_owner_taps\",\"taps\":{");
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "encoding failed");
        return ESP_OK;
    }
    used = (size_t)n;
    for (int i = 0; i < JR_AUDIO_TAP_COUNT && used < sizeof body; ++i) {
        n = snprintf(body + used, sizeof body - used,
            "%s\"%s\":{\"sample_rate\":%u,\"available_samples\":%u,"
            "\"capacity_samples\":%u,"
            "\"total_samples\":%llu,\"peak\":%d,\"rms\":%.2f,"
            "\"clipped_samples\":%u}",
            i == 0 ? "" : ",", audio_tap_name((jr_audio_tap_kind_t)i),
            (unsigned)info[i].sample_rate,
            (unsigned)info[i].available_samples,
            (unsigned)info[i].capacity_samples,
            (unsigned long long)info[i].total_samples,
            (int)info[i].peak, (double)info[i].rms,
            (unsigned)info[i].clipped_samples);
        if (n < 0 || (size_t)n >= sizeof body - used) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "encoding overflow");
            return ESP_OK;
        }
        used += (size_t)n;
    }
    n = snprintf(body + used, sizeof body - used, "}}");
    if (n < 0 || (size_t)n >= sizeof body - used) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "encoding overflow");
        return ESP_OK;
    }
    used += (size_t)n;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, used);
    return ESP_OK;
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static esp_err_t audio_tap_wav_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    char query[96];
    char source[24] = {0};
    jr_audio_tap_kind_t kind;
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "source", source, sizeof source) != ESP_OK ||
        !audio_tap_parse(source, &kind)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "missing ?source=mic-clean|mic-raw|reference|playback");
        return ESP_OK;
    }

    jr_audio_tap_info_t info;
    esp_err_t err = jr_audio_diag_get_info(kind, &info);
    size_t capacity = err == ESP_OK ? info.capacity_samples : 0U;
    int16_t *pcm = capacity > 0
        ? heap_caps_malloc(capacity * sizeof(int16_t),
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : NULL;
    if (pcm == NULL ||
        jr_audio_diag_copy(kind, pcm, capacity, &info) != ESP_OK ||
        info.available_samples == 0U) {
        heap_caps_free(pcm);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"available\":false,\"error\":\"audio tap empty\"}");
        return ESP_OK;
    }

    size_t samples = info.available_samples;
    uint32_t data_bytes = (uint32_t)(samples * sizeof(int16_t));
    uint8_t header[44] = {0};
    memcpy(header + 0, "RIFF", 4);
    write_le32(header + 4, 36U + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16U);
    write_le16(header + 20, 1U);
    write_le16(header + 22, 1U);
    write_le32(header + 24, info.sample_rate);
    write_le32(header + 28, info.sample_rate * 2U);
    write_le16(header + 32, 2U);
    write_le16(header + 34, 16U);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_bytes);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Jarvis-Audio-Source", source);
    httpd_resp_set_hdr(req, "X-Jarvis-Evidence-Level",
        kind == JR_AUDIO_TAP_REFERENCE ? "electrical" :
        kind == JR_AUDIO_TAP_MIC_RAW || kind == JR_AUDIO_TAP_MIC_CLEAN
            ? "acoustic" : "software");
    err = httpd_resp_send_chunk(req, (const char *)header, sizeof header);
    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t sent = 0;
    while (err == ESP_OK && sent < data_bytes) {
        size_t chunk = data_bytes - sent;
        if (chunk > 4096U) {
            chunk = 4096U;
        }
        err = httpd_resp_send_chunk(req, (const char *)bytes + sent, chunk);
        sent += chunk;
    }
    heap_caps_free(pcm);
    if (err != ESP_OK) {
        (void)httpd_resp_send_chunk(req, NULL, 0);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const char *touch_kind_name(jr_input_kind_t kind)
{
    switch (kind) {
    case JR_INPUT_TAP:        return "tap";
    case JR_INPUT_LONG_PRESS: return "long_press";
    case JR_INPUT_SWIPE:      return "swipe";
    case JR_INPUT_PRESS_DOWN: return "press_down";
    case JR_INPUT_PRESS_UP:   return "press_up";
    default:                  return "none";
    }
}

static int touch_sector_from_point(uint16_t x, uint16_t y)
{
    int dx = 2 * (int)x - 465;
    int dy = 2 * (int)y - 465;
    int ax = abs(dx);
    int ay = abs(dy);
    if (ax * 1000 < ay * 414) {
        return dy < 0 ? 0 : 4;
    }
    if (ay * 1000 < ax * 414) {
        return dx > 0 ? 2 : 6;
    }
    if (dx > 0) {
        return dy < 0 ? 1 : 3;
    }
    return dy < 0 ? 7 : 5;
}


/* /api/diag/tasks — per-task stack high-water marks, for right-sizing stacks.
 *
 * "High-water" is FreeRTOS's minimum-ever-free figure in BYTES on ESP-IDF: the
 * closest that task has come to overflowing since it started. Slack = stack
 * size minus peak usage, and slack in INTERNAL RAM is exactly what the TLS
 * handshake is starving for (see docs/JARVISNANO_OS_PLAN.md "Internal RAM
 * budget"). Sorted by headroom so the biggest reclaim candidates come first.
 *
 * Read this AFTER exercising the device (a full voice turn, a display
 * transition) — a task that has not yet hit its worst case reports misleadingly
 * generous headroom, and shrinking a stack on that basis is how you get a
 * stack-overflow panic three weeks later. */
static esp_err_t tasks_diag_handler(httpd_req_t *req)
{
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *snap = calloc(count, sizeof *snap);
    if (snap == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    const UBaseType_t got = uxTaskGetSystemState(snap, count, NULL);

    char *body = malloc(4096);
    if (body == NULL) {
        free(snap);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    int off = snprintf(body, 4096,
        "{\"free_internal\":%u,\"largest_internal_block\":%u,\"tasks\":[",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    for (UBaseType_t i = 0; i < got && off > 0 && off < 4000; ++i) {
        off += snprintf(body + off, (size_t)(4096 - off),
            "%s{\"name\":\"%s\",\"prio\":%u,\"stack_free\":%u}",
            i ? "," : "",
            snap[i].pcTaskName ? snap[i].pcTaskName : "?",
            (unsigned)snap[i].uxCurrentPriority,
            (unsigned)snap[i].usStackHighWaterMark);
    }
    if (off > 0 && off < 4090) {
        off += snprintf(body + off, (size_t)(4096 - off), "]}");
    }
    free(snap);

    if (off <= 0 || off >= 4090) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task encoding overflow");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, off);
    free(body);
    return ESP_OK;
}

/* /api/sensors — live IMU + battery telemetry. Both reads are non-blocking
 * snapshot copies (their sampler tasks own the shared I2C bus), so this handler
 * never parks the httpd task on a transaction. Phase 1 acceptance gate. */
/* POST /api/display/choices?n=3 — present test arcs, or n=0 to dismiss.
 *
 * Exists so the choice-arc RENDERING can be proven on glass independently of a
 * live Gemini ask_user call. The labels are static, so they satisfy the borrow
 * contract in jr_display.h without any lifetime games. */
static esp_err_t choices_debug_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    int n = 3;
    if (!query_int(req, "n", &n) || n < 0 || n > HUD_CHOICE_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "n must be 0..3");
        return ESP_OK;
    }
    /* MARSHALLED, not applied here: the choice statics are single-writer (the
     * app task), and the httpd task writing them raced the flush. The app
     * task drains this next loop; poll /api/display/choices/hit (or a
     * snapshot) for the applied state. A queued-but-undrained request is
     * simply replaced — last writer wins, matching the old semantics. */
    atomic_store(&s_debug_choices_req, (uint32_t)n + 1U);
    char body[96];
    int len = snprintf(body, sizeof body, "{\"queued\":%d,\"active\":%s}",
                       n, jr_display_choices_active() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

/* POST /api/input/tap?x=..&y=.. — inject one synthetic tap through the real
 * input handler (see input_next). This is the finger-free end of the ask loop:
 * present arcs, sim-tap one, and the full CHOICE_PICKED -> functionResponse
 * path runs exactly as it would under glass. */
static esp_err_t tap_sim_handler(httpd_req_t *req)
{
    if (!control_intent_required(req) || !agent_require_auth(req)) {
        return ESP_OK;
    }
    int shake = 0;
    if (query_int(req, "shake", &shake) && shake == 1) {
        atomic_store(&s_sim_shake, 2U);   /* two 10 Hz polls = a real shake */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"queued\":true,\"gesture\":\"shake\"}");
        return ESP_OK;
    }
    int flip = 0;
    if (query_int(req, "flip", &flip) && flip == 1) {
        atomic_store(&s_sim_flip, 30U);   /* ~3 s face-down, then auto face-up */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"queued\":true,\"gesture\":\"flip\"}");
        return ESP_OK;
    }
    int x = -1, y = -1;
    if (!query_int(req, "x", &x) || !query_int(req, "y", &y) ||
        x < 0 || x >= 466 || y < 0 || y >= 466) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need x and y in 0..465");
        return ESP_OK;
    }
    /* CAS-from-zero: a second tap posted before the app task drains the first
     * must be REFUSED, not silently swallowed after being acknowledged. */
    uint32_t expected = 0U;
    const uint32_t packed = (((uint32_t)x + 1U) << 16) | ((uint32_t)y + 1U);
    bool queued = atomic_compare_exchange_strong(&s_sim_touch, &expected,
                                                 packed);
    if (!queued) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    char body[64];
    int len = snprintf(body, sizeof body,
                       "{\"queued\":%s,\"x\":%d,\"y\":%d}",
                       queued ? "true" : "false", x, y);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

/* POST /api/demo — run the 27 s attract reel (POLISH-06). Starts only from a
 * quiet Listening/Idle; a live ask aborts it and any tap ends it. */
static esp_err_t demo_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    atomic_store(&s_demo_req, true);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"queued\":true,\"reel_s\":27}");
    return ESP_OK;
}

/* GET /api/display/choices/hit?x=..&y=.. — resolve a panel point to an arc.
 * Lets the hit geometry be checked against the rendered pixels without a finger. */
static esp_err_t choices_hit_handler(httpd_req_t *req)
{
    int x = -1, y = -1;
    if (!query_int(req, "x", &x) || !query_int(req, "y", &y)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need x and y");
        return ESP_OK;
    }
    const int idx = jr_display_choice_hit(x, y);
    char body[96];
    int len = snprintf(body, sizeof body, "{\"x\":%d,\"y\":%d,\"index\":%d}", x, y, idx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

/* POST /api/display/hud?on=0|1 — A/B the HUD's frame-rate cost on real glass. */
static esp_err_t hud_toggle_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    int on = 1;
    if (!query_int(req, "on", &on) || (on != 0 && on != 1)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "on must be 0 or 1");
        return ESP_OK;
    }
    jr_display_set_hud_enabled(on != 0);
    char body[64];
    int n = snprintf(body, sizeof body, "{\"hud\":%s}", on ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t sensors_handler(httpd_req_t *req)
{
    /* Start the samplers on first use rather than at boot — see the note in
     * app_main(). Both calls are idempotent. The first request after boot
     * therefore reports available=false while the samplers warm up (one 10 ms
     * period for the IMU, one 5 s period for the battery); the next is live. */
    (void)jr_imu_start();
    (void)jr_power_start();

    jr_imu_t imu = {0};
    jr_power_t bat = {0};
    const bool have_imu = jr_imu_read(&imu) == ESP_OK;
    const bool have_bat = jr_power_read(&bat) == ESP_OK;

    char body[768];
    int n = snprintf(body, sizeof body,
        "{\"imu\":{\"available\":%s,\"present\":%s,\"i2c_addr\":\"0x%02X\","
        "\"raw\":{\"ax\":%d,\"ay\":%d,\"az\":%d},"
        "\"g\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
        "\"pitch_deg\":%.1f,\"roll_deg\":%.1f,\"orientation\":\"%s\","
        "\"motion_mg\":%.1f,\"moving\":%s,\"shake\":%s,"
        "\"sample_seq\":%u,\"age_ms\":%u},"
        "\"battery\":{\"available\":%s,\"present\":%s,\"charging\":%s,"
        "\"usb_present\":%s,\"percent\":%d,\"millivolts\":%u,"
        "\"sample_seq\":%u,\"age_ms\":%u}}",
        have_imu ? "true" : "false",
        imu.present ? "true" : "false",
        (unsigned)imu.i2c_addr,
        (int)imu.ax, (int)imu.ay, (int)imu.az,
        (double)imu.gx, (double)imu.gy, (double)imu.gz,
        (double)imu.pitch_deg, (double)imu.roll_deg,
        imu.orientation ? imu.orientation : "unknown",
        (double)imu.motion_mg,
        imu.moving ? "true" : "false",
        imu.shake ? "true" : "false",
        (unsigned)imu.sample_seq, (unsigned)imu.age_ms,
        have_bat ? "true" : "false",
        bat.present ? "true" : "false",
        bat.charging ? "true" : "false",
        bat.usb_present ? "true" : "false",
        bat.percent == 0xFF ? -1 : (int)bat.percent,
        (unsigned)bat.millivolts,
        (unsigned)bat.sample_seq, (unsigned)bat.age_ms);
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "sensor status encoding failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t touch_status_handler(httpd_req_t *req)
{
    bool active = atomic_load(&s_touch_challenge_active);
    bool verified = atomic_load(&s_touch_challenge_verified);
    char body[768];
    int n = snprintf(body, sizeof body,
        "{\"available\":true,\"events\":%u,\"taps\":%u,"
        "\"long_presses\":%u,\"swipes\":%u,\"last\":{"
        "\"kind\":\"%s\",\"x\":%u,\"y\":%u,\"dx\":%d,"
        "\"dy\":%d,\"duration_ms\":%u},\"shade_open\":%s,"
        "\"panel_touch_challenge\":{\"pending\":%s,\"active\":%s,"
        "\"verified\":%s,\"evidence_level\":\"%s\","
        "\"expected_sector\":%u,\"correct_rounds\":%u,"
        "\"attempts\":%u,\"wrong\":%u,\"last_mapped_sector\":%u,"
        "\"last_latency_ms\":%u}}",
        (unsigned)atomic_load(&s_touch_events),
        (unsigned)atomic_load(&s_touch_taps),
        (unsigned)atomic_load(&s_touch_long_presses),
        (unsigned)atomic_load(&s_touch_swipes),
        touch_kind_name((jr_input_kind_t)atomic_load(&s_touch_last_kind)),
        (unsigned)atomic_load(&s_touch_last_x),
        (unsigned)atomic_load(&s_touch_last_y),
        atomic_load(&s_touch_last_dx), atomic_load(&s_touch_last_dy),
        (unsigned)atomic_load(&s_touch_last_duration_ms),
        s_ui_shade_open ? "true" : "false",
        atomic_load(&s_touch_challenge_start_requested) ? "true" : "false",
        active ? "true" : "false", verified ? "true" : "false",
        verified ? "physical_human_challenge" :
                   active ? "control_path_waiting_for_human" : "software",
        (unsigned)atomic_load(&s_touch_challenge_expected),
        (unsigned)atomic_load(&s_touch_challenge_correct),
        (unsigned)atomic_load(&s_touch_challenge_attempts),
        (unsigned)atomic_load(&s_touch_challenge_wrong),
        (unsigned)atomic_load(&s_touch_challenge_last_mapped),
        (unsigned)atomic_load(&s_touch_challenge_last_latency_ms));
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "touch status encoding failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t panel_touch_control_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char query[64];
    char action[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "action", action, sizeof action) != ESP_OK) {
        return touch_status_handler(req);
    }
    if (strcmp(action, "start") == 0) {
        atomic_store(&s_touch_challenge_cancel_requested, false);
        atomic_store(&s_touch_challenge_start_requested, true);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":true,\"queued\":true,\"rounds_required\":3}"
        );
        return ESP_OK;
    }
    if (strcmp(action, "cancel") == 0) {
        atomic_store(&s_touch_challenge_start_requested, false);
        atomic_store(&s_touch_challenge_cancel_requested, true);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"cancelled\":true}");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "?action=start|cancel");
    return ESP_OK;
}

static esp_err_t ui_shade_control_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char query[64];
    char action[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "action", action, sizeof action) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "?action=open|close|toggle");
        return ESP_OK;
    }
    bool open = atomic_load(&s_ui_shade_open);
    if (strcmp(action, "open") == 0) {
        open = true;
    } else if (strcmp(action, "close") == 0) {
        open = false;
    } else if (strcmp(action, "toggle") == 0) {
        open = !open;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "?action=open|close|toggle");
        return ESP_OK;
    }
    atomic_store(&s_ui_shade_open, open);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, open
        ? "{\"ok\":true,\"shade_open\":true}"
        : "{\"ok\":true,\"shade_open\":false}");
    return ESP_OK;
}

static void secure_zero(void *ptr, size_t size)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (size-- > 0U) {
        *p++ = 0U;
    }
}

/* cJSON's parser is recursive. Bound structural depth before parsing so a
 * paired LAN client cannot turn a small (<=1536-byte) but pathological body
 * into an HTTP-task stack overflow. The accepted Agent Link schema needs only
 * root -> evidence array -> evidence object (depth three). */
static bool json_depth_within(const char *json, size_t length,
                              unsigned max_depth)
{
    unsigned depth = 0U;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char ch = json[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if (ch == '}' || ch == ']') {
            if (depth == 0U) {
                return false;
            }
            depth--;
        }
    }
    return !in_string && depth == 0U;
}

static esp_err_t pairing_claim_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t until = atomic_load(&s_pairing_claim_until_ms);
    if (until == 0U || (int32_t)(until - now) <= 0) {
        atomic_store(&s_pairing_claim_until_ms, 0U);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"hold BOOT 1.5-5 seconds; retry within 60 seconds\"}");
        return ESP_OK;
    }

    int rotate = 0;
    (void)query_int(req, "rotate", &rotate);
    if (rotate != 0) {
        esp_err_t clear_err = jr_cfg_set(JR_CFG_PAIRING_TOKEN, "");
        if (clear_err != ESP_OK) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "pairing rotation failed");
            return ESP_OK;
        }
    }
    char token[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    bool created = false;
    esp_err_t err = jr_net_pairing_token_ensure(token, sizeof token, &created);
    if (err != ESP_OK || !created || token[0] == '\0') {
        secure_zero(token, sizeof token);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, rotate == 0
            ? "{\"ok\":false,\"error\":\"token already exists; claim with rotate=1\"}"
            : "{\"ok\":false,\"error\":\"new token unavailable\"}");
        return ESP_OK;
    }
    atomic_store(&s_pairing_claim_until_ms, 0U);
    s_ui_shade_open = false;
    if (VOICE_ALWAYS_READY) {
        atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
    }
    char body[128];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":true,\"token\":\"%s\",\"one_time\":true}",
                     token);
    secure_zero(token, sizeof token);
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "pairing response failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    secure_zero(body, sizeof body);
    return ESP_OK;
}

/* ── DEV MODE ────────────────────────────────────────────────────────────────
 * Set to 0 before shipping. One digit, and grep for JR_DEV_OPEN_DIAGNOSTICS.
 *
 * While this is 1 the pairing token is NOT required on the diagnostic and
 * control endpoints — /api/logs, /api/cockpit, the /api/audio routes,
 * /api/debug/input and the rest answer any caller that can reach the device
 * on the LAN. That is
 * the whole point: during bring-up the token was pure friction, it lives in a
 * keychain entry that may not exist on a fresh machine, and claiming a new one
 * needs a physical long-press on the very device you are trying to debug
 * remotely.
 *
 * What it costs: anything on your network can read the logs, hear the mic taps,
 * drive the display and inject input. On a home LAN behind a router that is a
 * considered trade; on any shared or public network it is not. The boot log
 * says so loudly on every boot so this cannot ship unnoticed.
 *
 * The X-JarvisNano-Control header gate on mutating POSTs is deliberately NOT
 * bypassed — it costs a caller nothing and still blocks drive-by cross-origin
 * requests and link prefetchers. */
#define JR_DEV_OPEN_DIAGNOSTICS 1

static bool agent_require_auth(httpd_req_t *req)
{
#if JR_DEV_OPEN_DIAGNOSTICS
    (void)req;
    return true;
#else
    static const char *header = "X-JarvisNano-Token";
    size_t length = httpd_req_get_hdr_value_len(req, header);
    if (length == 0U || length >= JR_CFG_PAIRING_TOKEN_CAP) {
        httpd_resp_set_status(req, length == 0U
            ? "401 Unauthorized" : "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"pairing token required\"}");
        return false;
    }
    char token[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    bool matches = false;
    esp_err_t err = httpd_req_get_hdr_value_str(req, header, token,
                                                 sizeof token);
    if (err == ESP_OK) {
        err = jr_net_pairing_token_verify(token, &matches);
    }
    secure_zero(token, sizeof token);
    if (err != ESP_OK || !matches) {
        httpd_resp_set_status(req, err == ESP_OK
            ? "403 Forbidden" : "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, err == ESP_OK
            ? "{\"ok\":false,\"error\":\"pairing token rejected\"}"
            : "{\"ok\":false,\"error\":\"pairing not initialised\"}");
        return false;
    }
    return true;
#endif /* JR_DEV_OPEN_DIAGNOSTICS */
}

static bool url_ends_with(const char *url, const char *suffix)
{
    if (url == NULL || suffix == NULL) return false;
    size_t url_len = strlen(url);
    size_t suffix_len = strlen(suffix);
    return suffix_len <= url_len &&
        strcmp(url + url_len - suffix_len, suffix) == 0;
}

static const char *device_tool_route_kind(const char *url)
{
    if (url_ends_with(url, "/device/v1/invoke")) return "typed_device";
    if (url_ends_with(url, "/act")) return "legacy_fixed_template";
    return "none";
}

static void device_tool_config_snapshot(bool *configured, bool *typed,
                                        bool *legacy)
{
    jr_net_config_t stored = {0};
    bool ready = atomic_load(&s_tool_diag.worker_ready);
    bool loaded = jr_cfg_load(&stored, JR_CFG_VIEW_INTERNAL) == ESP_OK;
    const char *kind = loaded
        ? device_tool_route_kind(stored.jarvis_mcp_url) : "none";
    *configured = ready && jr_tools_is_configured();
    *typed = *configured && strcmp(kind, "typed_device") == 0;
    *legacy = *configured && strcmp(kind, "legacy_fixed_template") == 0;
    secure_zero(&stored, sizeof(stored));
}

static void device_tool_config_reply(httpd_req_t *req)
{
    bool configured = false;
    bool typed = false;
    bool legacy = false;
    device_tool_config_snapshot(&configured, &typed, &legacy);
    const char *kind = typed ? "typed_device" :
        legacy ? "legacy_fixed_template" : "none";
    char body[192];
    int n = snprintf(body, sizeof(body),
        "{\"configured\":%s,\"route_kind\":\"%s\","
        "\"typed_device\":%s,\"legacy_fixed_template\":%s}",
        configured ? "true" : "false", kind,
        typed ? "true" : "false", legacy ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body,
                    n > 0 && (size_t)n < sizeof(body) ? n : 0);
}

static esp_err_t device_tool_config_get_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    device_tool_config_reply(req);
    return ESP_OK;
}

static esp_err_t device_tool_config_post_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    enum { TOOL_CONFIG_BODY_CAP = 768 };
    size_t length = req->content_len;
    if (length == 0U || length >= TOOL_CONFIG_BODY_CAP) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid tools config length");
        return ESP_OK;
    }
    char *raw = heap_caps_malloc(TOOL_CONFIG_BODY_CAP,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tools config buffer unavailable");
        return ESP_OK;
    }
    size_t received = 0U;
    while (received < length) {
        int got = httpd_req_recv(req, raw + received, length - received);
        if (got <= 0) {
            secure_zero(raw, TOOL_CONFIG_BODY_CAP);
            heap_caps_free(raw);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "tools config body read failed");
            return ESP_OK;
        }
        received += (size_t)got;
    }
    raw[length] = '\0';
    cJSON *root = json_depth_within(raw, length, 3U)
        ? cJSON_ParseWithLengthOpts(raw, length + 1U, NULL, true) : NULL;
    secure_zero(raw, TOOL_CONFIG_BODY_CAP);
    heap_caps_free(raw);

    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    bool valid = cJSON_IsObject(root) && cJSON_IsString(url) &&
        cJSON_IsString(key) && url->valuestring != NULL &&
        key->valuestring != NULL;
    unsigned fields = 0U;
    bool saw_url = false;
    bool saw_key = false;
    for (cJSON *item = valid ? root->child : NULL;
         item != NULL; item = item->next) {
        fields++;
        if (item->string != NULL && strcmp(item->string, "url") == 0 &&
            !saw_url) saw_url = true;
        else if (item->string != NULL && strcmp(item->string, "key") == 0 &&
                 !saw_key) saw_key = true;
        else valid = false;
    }
    valid = valid && fields == 2U && saw_url && saw_key;

    jr_net_config_t next = {0};
    if (valid) {
        size_t url_len = strnlen(url->valuestring, sizeof(next.jarvis_mcp_url));
        size_t key_len = strnlen(key->valuestring, sizeof(next.jarvis_mcp_key));
        bool clearing = url_len == 0U && key_len == 0U;
        valid = url_len < sizeof(next.jarvis_mcp_url) &&
            key_len < sizeof(next.jarvis_mcp_key) &&
            ((clearing) ||
             (url_len > 8U && key_len >= 32U &&
              strncmp(url->valuestring, "https://", 8U) == 0 &&
              strcmp(device_tool_route_kind(url->valuestring), "none") != 0));
        if (valid) {
            strlcpy(next.jarvis_mcp_url, url->valuestring,
                    sizeof(next.jarvis_mcp_url));
            strlcpy(next.jarvis_mcp_key, key->valuestring,
                    sizeof(next.jarvis_mcp_key));
            valid = jr_cfg_validate(JR_CFG_JARVIS_MCP_URL,
                                    next.jarvis_mcp_url) == ESP_OK &&
                jr_cfg_validate(JR_CFG_JARVIS_MCP_KEY,
                                next.jarvis_mcp_key) == ESP_OK;
        }
    }
    if (cJSON_IsString(key) && key->valuestring != NULL) {
        secure_zero(key->valuestring, strlen(key->valuestring));
    }
    cJSON_Delete(root);
    if (!valid) {
        secure_zero(&next, sizeof(next));
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"invalid bounded tools config\"}");
        return ESP_OK;
    }

    esp_err_t applied = jr_cfg_apply(&next,
        JR_CFG_F_JARVIS_MCP_URL | JR_CFG_F_JARVIS_MCP_KEY);
    secure_zero(&next, sizeof(next));
    if (applied == ESP_OK) {
        applied = jr_tools_reload_config();
    }
    if (applied != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"tools config unavailable\"}");
        return ESP_OK;
    }
    device_tool_config_reply(req);
    return ESP_OK;
}

static bool agent_text_safe(const char *value, size_t capacity,
                            bool allow_space)
{
    if (value == NULL) {
        return false;
    }
    size_t length = strnlen(value, capacity);
    if (length == 0U || length >= capacity) {
        return false;
    }
    if (strstr(value, "http://") != NULL || strstr(value, "https://") != NULL ||
        strstr(value, "<script") != NULL) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20U || c == 0x7fU || c == '<' || c == '>' ||
            c == '\"' || c == '\\' || (!allow_space && c == ' ')) {
            return false;
        }
    }
    return true;
}

/* Surface text must match the panel's deliberately tiny 5x7 glyph set. The
 * transport rejects characters that would otherwise turn into invisible
 * spaces on the physical display. */
static bool brain_render_text_safe(const char *value, size_t capacity,
                                   bool allow_space)
{
    if (!agent_text_safe(value, capacity, allow_space)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' ||
            *p == ':' || *p == '/' || *p == '?' || *p == '!' ||
            *p == '+' || *p == ',' || *p == '\'' || *p == '(' ||
            *p == ')' || *p == '&' || *p == '%' ||
            (allow_space && *p == ' ')) {
            continue;
        }
        return false;
    }
    return true;
}

static bool agent_state_valid(const char *state)
{
    return state != NULL &&
        (strcmp(state, "working") == 0 || strcmp(state, "verifying") == 0 ||
         strcmp(state, "waiting") == 0 || strcmp(state, "succeeded") == 0 ||
         strcmp(state, "failed") == 0);
}

static bool evidence_state_valid(const char *state)
{
    return state != NULL &&
        (strcmp(state, "pass") == 0 || strcmp(state, "working") == 0 ||
         strcmp(state, "wait") == 0 || strcmp(state, "fail") == 0);
}

static bool brain_surface_kind_parse(const char *name,
                                     jr_display_surface_kind_t *out)
{
    if (name == NULL || out == NULL) return false;
    if (strcmp(name, "notice") == 0) *out = JR_DISPLAY_SURFACE_NOTICE;
    else if (strcmp(name, "progress") == 0) *out = JR_DISPLAY_SURFACE_PROGRESS;
    else if (strcmp(name, "result") == 0) *out = JR_DISPLAY_SURFACE_RESULT;
    else if (strcmp(name, "choice") == 0) *out = JR_DISPLAY_SURFACE_CHOICE;
    else if (strcmp(name, "consent") == 0) *out = JR_DISPLAY_SURFACE_CONSENT;
    else return false;
    return true;
}

static const char *brain_surface_kind_name(jr_display_surface_kind_t kind)
{
    switch (kind) {
    case JR_DISPLAY_SURFACE_PROGRESS: return "progress";
    case JR_DISPLAY_SURFACE_RESULT:   return "result";
    case JR_DISPLAY_SURFACE_CHOICE:   return "choice";
    case JR_DISPLAY_SURFACE_CONSENT:  return "consent";
    case JR_DISPLAY_SURFACE_NOTICE:
    default:                          return "notice";
    }
}

static void brain_surface_expire(uint32_t now)
{
    if (s_brain_lock != NULL &&
        xSemaphoreTake(s_brain_lock, 0) == pdTRUE) {
        if (s_brain_surface.active &&
            (int32_t)(now - s_brain_surface.expires_ms) >= 0) {
            if (s_brain_surface.local_owned && s_tool_consent.active) {
                device_tool_resolve_consent_locked(TOOL_CONSENT_TIMEOUT);
                xSemaphoreGive(s_brain_lock);
                return;
            }
            /* Brain state and the glass compositor are one transaction. Keep
             * the lock order brain -> display everywhere so a newer present
             * cannot be erased by an older deferred dismiss. */
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
            configASSERT(!jr_display_surface_is_active());
        }
        xSemaphoreGive(s_brain_lock);
    }
}

/* Returns true whenever a Desk surface owns the tap, even if the tap missed a
 * choice button. This prevents a card interaction from accidentally toggling
 * the always-ready microphone underneath it. */
static bool brain_surface_handle_tap(uint16_t x, uint16_t y, uint32_t now,
                                     bool physical, uint32_t emitted_ms)
{
    /* Read glass ownership before taking the Brain mutex. If the mutex is
     * contended, swallowing a tap is safer than letting a visible card toggle
     * the always-ready microphone underneath it. */
    bool glass_owned = jr_display_surface_is_active();
    bool owned = glass_owned;
    if (s_brain_lock == NULL) {
        return glass_owned;
    }
    if (xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        /* A contended Brain transaction may be installing a card right now.
         * Lose one tap rather than route it to the microphone underneath. */
        return true;
    }
    if (s_brain_surface.active &&
        (int32_t)(now - s_brain_surface.expires_ms) < 0) {
        owned = true;
        int action_index = jr_display_surface_hit_test(x, y);
        if (s_brain_surface.local_owned) {
            if (!physical || emitted_ms < s_tool_consent.presented_ms) {
                ESP_LOGW(TAG, "consent ignored non-physical/stale tap");
                xSemaphoreGive(s_brain_lock);
                return true;
            }
            if (action_index == 0) {
                device_tool_resolve_consent_locked(TOOL_CONSENT_DENY);
            } else if (action_index == 1) {
                device_tool_resolve_consent_locked(TOOL_CONSENT_ALLOW);
            }
            xSemaphoreGive(s_brain_lock);
            return true;
        }
        uint8_t count = s_brain_surface.view.action_count;
        const char *action_id = NULL;
        if (action_index >= 0 && action_index < count) {
            action_id = s_brain_surface.action_ids[action_index];
        } else if (count == 0U &&
                   s_brain_surface.view.kind != JR_DISPLAY_SURFACE_PROGRESS) {
            action_id = "dismiss";
        }
        if (action_id != NULL) {
            uint32_t seq = ++s_brain_event_seq;
            brain_action_event_t *event =
                &s_brain_events[(seq - 1U) % BRAIN_EVENT_CAP];
            memset(event, 0, sizeof *event);
            event->seq = seq;
            event->ts_ms = now;
            strlcpy(event->session, s_brain_surface.session,
                    sizeof event->session);
            strlcpy(event->id, s_brain_surface.id, sizeof event->id);
            strlcpy(event->action_id, action_id, sizeof event->action_id);
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
            configASSERT(!jr_display_surface_is_active());
        }
    } else if (s_brain_surface.active) {
        /* The expiry edge still owns this physical tap. Otherwise the user can
         * touch a card that has not yet been repainted away and accidentally
         * stop or resume voice in the same input event. */
        owned = true;
        if (s_brain_surface.local_owned && s_tool_consent.active) {
            device_tool_resolve_consent_locked(TOOL_CONSENT_TIMEOUT);
        } else {
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
        }
        configASSERT(!jr_display_surface_is_active());
    } else if (glass_owned) {
        /* Defensive repair for any pre-existing state/glass skew. */
        jr_display_surface_dismiss();
        configASSERT(!jr_display_surface_is_active());
    }
    xSemaphoreGive(s_brain_lock);
    return owned;
}

static bool operator_mode_release(uint32_t now, const char *reason,
                                  bool physical_feedback,
                                  bool only_if_expired)
{
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (only_if_expired &&
        (!atomic_load(&s_operator_mode_active) ||
         operator_lease_active(now))) {
        xSemaphoreGive(s_brain_lock);
        return false;
    }

    atomic_store(&s_operator_mode_active, false);
    atomic_store(&s_operator_mode_entered_ms, 0U);
    atomic_store(&s_operator_lease_until_ms, 0U);
    if (s_brain_surface.active && !s_brain_surface.local_owned) {
        jr_display_surface_dismiss();
        s_brain_surface.active = false;
    }
    const bool privacy_held =
        atomic_load(&s_voice_privacy_paused) || s_flip_muted;
    atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
    if (privacy_held) {
        jr_display_caption_set("MUTED - HOLD TO RESUME");
    } else {
        jr_mood_poke_awake(&s_mood, now);
        jr_display_caption_set("LISTENING");
    }
    xSemaphoreGive(s_brain_lock);

    if (physical_feedback && !privacy_held) {
        jr_display_bloom();
        (void)jr_audio_diag_play_chirp(160U, 8U);
    }
    ESP_LOGI(TAG, "operator: Codex mode released (%s)", reason);
    return true;
}

static esp_err_t agent_link_get_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    agent_link_state_t state = {0};
    uint32_t revision_hwm = 0U;
    uint32_t next_revision = 0U;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_agent_link_lock == NULL ||
        xSemaphoreTake(s_agent_link_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "agent link unavailable");
        return ESP_OK;
    }
    if (s_agent_link.active &&
        (int32_t)(now - s_agent_link.expires_ms) >= 0) {
        s_agent_link.active = false;
    }
    state = s_agent_link;
    revision_hwm = s_agent_link_revision_hwm;
    next_revision = revision_hwm + 1U;
    xSemaphoreGive(s_agent_link_lock);

    char body[1024];
    size_t used = 0U;
    int n;
    if (!state.active) {
        n = snprintf(body, sizeof body,
            "{\"active\":false,\"revision_hwm\":%u,\"next_revision\":%u,"
            "\"updates\":%u,\"rejects\":%u}",
            (unsigned)revision_hwm, (unsigned)next_revision,
            (unsigned)state.updates, (unsigned)state.rejects);
    } else {
        uint32_t ttl_ms = (int32_t)(state.expires_ms - now) > 0
            ? state.expires_ms - now : 0U;
        n = snprintf(body, sizeof body,
            "{\"active\":true,\"task_id\":\"%s\",\"revision\":%u,"
            "\"state\":\"%s\",\"progress\":%u,\"title\":\"%s\","
            "\"summary\":\"%s\",\"ttl_ms\":%u,\"evidence\":[",
            state.task_id, (unsigned)state.revision, state.state,
            (unsigned)state.progress, state.title, state.summary,
            (unsigned)ttl_ms);
        if (n > 0 && (size_t)n < sizeof body) {
            used = (size_t)n;
            for (uint8_t i = 0; i < state.evidence_count; ++i) {
                n = snprintf(body + used, sizeof body - used,
                    "%s{\"label\":\"%s\",\"state\":\"%s\"}",
                    i == 0 ? "" : ",", state.evidence[i].label,
                    state.evidence[i].state);
                if (n < 0 || (size_t)n >= sizeof body - used) {
                    used = sizeof body;
                    break;
                }
                used += (size_t)n;
            }
            if (used < sizeof body) {
                n = snprintf(body + used, sizeof body - used,
                             "],\"revision_hwm\":%u,\"next_revision\":%u,"
                             "\"updates\":%u,\"rejects\":%u}",
                             (unsigned)revision_hwm,
                             (unsigned)next_revision,
                             (unsigned)state.updates,
                             (unsigned)state.rejects);
                if (n > 0 && (size_t)n < sizeof body - used) {
                    used += (size_t)n;
                    n = (int)used;
                } else {
                    n = -1;
                }
            } else {
                n = -1;
            }
        }
    }
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "agent link encoding failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t agent_link_post_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 1536) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "JSON payload must be 1..1536 bytes");
        return ESP_OK;
    }
    size_t length = (size_t)req->content_len;
    char *payload = heap_caps_malloc(length + 1U,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "device busy");
        return ESP_OK;
    }
    size_t received = 0U;
    while (received < length) {
        int got = httpd_req_recv(req, payload + received, length - received);
        if (got <= 0) {
            secure_zero(payload, length + 1U);
            heap_caps_free(payload);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)got;
    }
    payload[length] = '\0';
    cJSON *root = json_depth_within(payload, length, 6U)
        ? cJSON_ParseWithLengthOpts(payload, length + 1U, NULL, true)
        : NULL;
    secure_zero(payload, length + 1U);
    heap_caps_free(payload);

    agent_link_state_t next = {0};
    uint32_t ttl_s = 900U;
    bool valid = root != NULL && cJSON_IsObject(root);
    cJSON *task_id = cJSON_GetObjectItemCaseSensitive(root, "task_id");
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    cJSON *progress = cJSON_GetObjectItemCaseSensitive(root, "progress");
    cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summary");
    cJSON *evidence = cJSON_GetObjectItemCaseSensitive(root, "evidence");
    cJSON *ttl = cJSON_GetObjectItemCaseSensitive(root, "ttl_s");
    enum {
        AGENT_FIELD_TASK_ID  = 1U << 0,
        AGENT_FIELD_REVISION = 1U << 1,
        AGENT_FIELD_STATE    = 1U << 2,
        AGENT_FIELD_PROGRESS = 1U << 3,
        AGENT_FIELD_TITLE    = 1U << 4,
        AGENT_FIELD_SUMMARY  = 1U << 5,
        AGENT_FIELD_EVIDENCE = 1U << 6,
        AGENT_FIELD_TTL      = 1U << 7,
    };
    const unsigned required_fields = AGENT_FIELD_TASK_ID |
        AGENT_FIELD_REVISION | AGENT_FIELD_STATE | AGENT_FIELD_PROGRESS |
        AGENT_FIELD_TITLE | AGENT_FIELD_SUMMARY;
    unsigned seen_fields = 0U;
    for (cJSON *it = valid ? root->child : NULL; it != NULL; it = it->next) {
        unsigned field = 0U;
        if (it->string == NULL) {
            valid = false;
        } else if (strcmp(it->string, "task_id") == 0) {
            field = AGENT_FIELD_TASK_ID;
        } else if (strcmp(it->string, "revision") == 0) {
            field = AGENT_FIELD_REVISION;
        } else if (strcmp(it->string, "state") == 0) {
            field = AGENT_FIELD_STATE;
        } else if (strcmp(it->string, "progress") == 0) {
            field = AGENT_FIELD_PROGRESS;
        } else if (strcmp(it->string, "title") == 0) {
            field = AGENT_FIELD_TITLE;
        } else if (strcmp(it->string, "summary") == 0) {
            field = AGENT_FIELD_SUMMARY;
        } else if (strcmp(it->string, "evidence") == 0) {
            field = AGENT_FIELD_EVIDENCE;
        } else if (strcmp(it->string, "ttl_s") == 0) {
            field = AGENT_FIELD_TTL;
        } else {
            valid = false;
        }
        if (field != 0U) {
            if ((seen_fields & field) != 0U) {
                valid = false;
            }
            seen_fields |= field;
        }
    }
    valid = valid && (seen_fields & required_fields) == required_fields &&
        cJSON_IsString(task_id) && cJSON_IsNumber(revision) &&
        revision->valuedouble == (double)revision->valueint &&
        revision->valueint >= 1 && cJSON_IsString(state) &&
        cJSON_IsNumber(progress) &&
        progress->valuedouble == (double)progress->valueint &&
        progress->valueint >= 0 && progress->valueint <= 100 &&
        cJSON_IsString(title) && cJSON_IsString(summary) &&
        agent_text_safe(task_id->valuestring, sizeof next.task_id, false) &&
        agent_text_safe(title->valuestring, sizeof next.title, true) &&
        agent_text_safe(summary->valuestring, sizeof next.summary, true) &&
        agent_state_valid(state->valuestring);
    if (valid && ttl != NULL) {
        valid = cJSON_IsNumber(ttl) &&
            ttl->valuedouble == (double)ttl->valueint &&
            ttl->valueint >= 30 && ttl->valueint <= 3600;
        if (valid) {
            ttl_s = (uint32_t)ttl->valueint;
        }
    }
    if (valid && evidence != NULL) {
        valid = cJSON_IsArray(evidence) &&
                cJSON_GetArraySize(evidence) <= AGENT_EVIDENCE_CAP;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, evidence) {
            cJSON *label = cJSON_GetObjectItemCaseSensitive(item, "label");
            cJSON *ev_state = cJSON_GetObjectItemCaseSensitive(item, "state");
            unsigned item_fields = 0U;
            for (cJSON *field = cJSON_IsObject(item) ? item->child : NULL;
                 field != NULL; field = field->next) {
                if (field->string == NULL ||
                    (strcmp(field->string, "label") != 0 &&
                     strcmp(field->string, "state") != 0)) {
                    valid = false;
                }
                item_fields++;
            }
            valid = valid && cJSON_IsObject(item) && item_fields == 2U &&
                    label != NULL && ev_state != NULL && label != ev_state &&
                    cJSON_IsString(label) && cJSON_IsString(ev_state) &&
                    agent_text_safe(label->valuestring,
                                    AGENT_LABEL_CAP, true) &&
                    evidence_state_valid(ev_state->valuestring);
            if (!valid) {
                break;
            }
            strlcpy(next.evidence[next.evidence_count].label,
                    label->valuestring, AGENT_LABEL_CAP);
            strlcpy(next.evidence[next.evidence_count].state,
                    ev_state->valuestring, AGENT_STATE_CAP);
            next.evidence_count++;
        }
    }
    if (!valid) {
        cJSON_Delete(root);
        if (s_agent_link_lock != NULL &&
            xSemaphoreTake(s_agent_link_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_agent_link.rejects++;
            xSemaphoreGive(s_agent_link_lock);
        }
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"invalid bounded Agent Link payload\"}");
        return ESP_OK;
    }

    strlcpy(next.task_id, task_id->valuestring, sizeof next.task_id);
    next.revision = (uint32_t)revision->valueint;
    strlcpy(next.state, state->valuestring, sizeof next.state);
    next.progress = (uint8_t)progress->valueint;
    strlcpy(next.title, title->valuestring, sizeof next.title);
    strlcpy(next.summary, summary->valuestring, sizeof next.summary);
    cJSON_Delete(root);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    next.active = true;
    next.updated_ms = now;
    next.expires_ms = now + ttl_s * 1000U;

    if (s_agent_link_lock == NULL ||
        xSemaphoreTake(s_agent_link_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "agent link busy");
        return ESP_OK;
    }
    uint32_t expected_revision = s_agent_link_revision_hwm + 1U;
    if (next.revision != expected_revision) {
        uint32_t revision_hwm = s_agent_link_revision_hwm;
        s_agent_link.rejects++;
        xSemaphoreGive(s_agent_link_lock);
        char conflict[160];
        int conflict_len = snprintf(conflict, sizeof conflict,
            "{\"ok\":false,\"error\":\"revision must equal next_revision\","
            "\"revision_hwm\":%u,\"next_revision\":%u}",
            (unsigned)revision_hwm, (unsigned)expected_revision);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        if (conflict_len > 0 && (size_t)conflict_len < sizeof conflict) {
            httpd_resp_send(req, conflict, conflict_len);
        } else {
            httpd_resp_sendstr(req,
                "{\"ok\":false,\"error\":\"invalid revision sequence\"}");
        }
        return ESP_OK;
    }
    next.updates = s_agent_link.updates + 1U;
    next.rejects = s_agent_link.rejects;
    s_agent_link_revision_hwm = next.revision;
    s_agent_link = next;
    xSemaphoreGive(s_agent_link_lock);

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"accepted\":true}");
    return ESP_OK;
}

static esp_err_t brain_inbox_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 1536) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "brain envelope must be 1..1536 bytes");
        return ESP_OK;
    }
    size_t length = (size_t)req->content_len;
    char *raw = heap_caps_malloc(length + 1U,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "brain inbox busy");
        return ESP_OK;
    }
    size_t received = 0U;
    while (received < length) {
        int got = httpd_req_recv(req, raw + received, length - received);
        if (got <= 0) {
            secure_zero(raw, length + 1U);
            heap_caps_free(raw);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "brain body read failed");
            return ESP_OK;
        }
        received += (size_t)got;
    }
    raw[length] = '\0';
    cJSON *root = json_depth_within(raw, length, 5U)
        ? cJSON_ParseWithLengthOpts(raw, length + 1U, NULL, true) : NULL;
    secure_zero(raw, length + 1U);
    heap_caps_free(raw);

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *ttl_ms = cJSON_GetObjectItemCaseSensitive(root, "ttl_ms");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    bool valid = root != NULL && cJSON_IsObject(root);
    unsigned seen = 0U;
    enum {
        BF_VERSION = 1U << 0, BF_TYPE = 1U << 1, BF_SEQ = 1U << 2,
        BF_SESSION = 1U << 3, BF_ID = 1U << 4, BF_TTL = 1U << 5,
        BF_PAYLOAD = 1U << 6,
    };
    for (cJSON *it = valid ? root->child : NULL; it != NULL; it = it->next) {
        unsigned bit = 0U;
        if (it->string == NULL) valid = false;
        else if (strcmp(it->string, "v") == 0) bit = BF_VERSION;
        else if (strcmp(it->string, "type") == 0) bit = BF_TYPE;
        else if (strcmp(it->string, "seq") == 0) bit = BF_SEQ;
        else if (strcmp(it->string, "session") == 0) bit = BF_SESSION;
        else if (strcmp(it->string, "id") == 0) bit = BF_ID;
        else if (strcmp(it->string, "ttl_ms") == 0) bit = BF_TTL;
        else if (strcmp(it->string, "payload") == 0) bit = BF_PAYLOAD;
        else valid = false;
        if (bit != 0U) {
            if ((seen & bit) != 0U) valid = false;
            seen |= bit;
        }
    }
    const unsigned required = BF_VERSION | BF_TYPE | BF_SEQ | BF_SESSION |
        BF_ID | BF_TTL | BF_PAYLOAD;
    valid = valid && seen == required && cJSON_IsNumber(version) &&
        version->valuedouble == 1.0 && cJSON_IsString(type) &&
        cJSON_IsNumber(seq) && seq->valuedouble == (double)seq->valueint &&
        seq->valueint >= 1 && cJSON_IsString(session) && cJSON_IsString(id) &&
        agent_text_safe(session->valuestring, BRAIN_SESSION_CAP, false) &&
        agent_text_safe(id->valuestring, BRAIN_SURFACE_ID_CAP, false) &&
        cJSON_IsNumber(ttl_ms) &&
        ttl_ms->valuedouble == (double)ttl_ms->valueint &&
        ttl_ms->valueint >= 1000 && ttl_ms->valueint <= 600000 &&
        cJSON_IsObject(payload);

    bool dismiss = valid && strcmp(type->valuestring, "surface.dismiss") == 0;
    bool update = valid && strcmp(type->valuestring, "surface.update") == 0;
    bool present = valid &&
        (strcmp(type->valuestring, "surface.present") == 0 || update);
    valid = valid && (dismiss || present);

    brain_surface_state_t next = {0};
    if (valid && present) {
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(payload, "kind");
        cJSON *title = cJSON_GetObjectItemCaseSensitive(payload, "title");
        cJSON *body = cJSON_GetObjectItemCaseSensitive(payload, "body");
        cJSON *actions = cJSON_GetObjectItemCaseSensitive(payload, "actions");
        unsigned p_seen = 0U;
        enum { PF_KIND=1U, PF_TITLE=2U, PF_BODY=4U, PF_ACTIONS=8U };
        for (cJSON *it = payload->child; it != NULL; it = it->next) {
            unsigned bit = 0U;
            if (it->string == NULL) valid = false;
            else if (strcmp(it->string, "kind") == 0) bit = PF_KIND;
            else if (strcmp(it->string, "title") == 0) bit = PF_TITLE;
            else if (strcmp(it->string, "body") == 0) bit = PF_BODY;
            else if (strcmp(it->string, "actions") == 0) bit = PF_ACTIONS;
            else valid = false;
            if (bit != 0U) {
                if ((p_seen & bit) != 0U) valid = false;
                p_seen |= bit;
            }
        }
        valid = valid && p_seen == (PF_KIND|PF_TITLE|PF_BODY|PF_ACTIONS) &&
            cJSON_IsString(kind) && cJSON_IsString(title) &&
            cJSON_IsString(body) && cJSON_IsArray(actions) &&
            cJSON_GetArraySize(actions) <= JR_DISPLAY_SURFACE_ACTION_CAP &&
            brain_surface_kind_parse(kind->valuestring, &next.view.kind) &&
            brain_render_text_safe(title->valuestring,
                                   JR_DISPLAY_SURFACE_TITLE_CAP, true) &&
            brain_render_text_safe(body->valuestring,
                                   JR_DISPLAY_SURFACE_BODY_CAP, true);
        cJSON *action = NULL;
        cJSON_ArrayForEach(action, actions) {
            cJSON *action_id = cJSON_GetObjectItemCaseSensitive(action, "id");
            cJSON *label = cJSON_GetObjectItemCaseSensitive(action, "label");
            unsigned fields = 0U;
            for (cJSON *field = cJSON_IsObject(action) ? action->child : NULL;
                 field != NULL; field = field->next) {
                if (field->string == NULL ||
                    (strcmp(field->string, "id") != 0 &&
                     strcmp(field->string, "label") != 0)) valid = false;
                fields++;
            }
            valid = valid && cJSON_IsObject(action) && fields == 2U &&
                cJSON_IsString(action_id) && cJSON_IsString(label) &&
                agent_text_safe(action_id->valuestring,
                                BRAIN_ACTION_ID_CAP, false) &&
                brain_render_text_safe(label->valuestring,
                                       JR_DISPLAY_SURFACE_LABEL_CAP, true);
            if (!valid) break;
            uint8_t index = next.view.action_count++;
            strlcpy(next.action_ids[index], action_id->valuestring,
                    sizeof next.action_ids[index]);
            strlcpy(next.view.action_labels[index], label->valuestring,
                    sizeof next.view.action_labels[index]);
        }
        if (valid) {
            if (next.view.kind == JR_DISPLAY_SURFACE_CONSENT) {
                valid = next.view.action_count == 2U;
            } else if (next.view.kind == JR_DISPLAY_SURFACE_CHOICE) {
                valid = next.view.action_count >= 2U;
            } else if (next.view.kind == JR_DISPLAY_SURFACE_PROGRESS) {
                valid = next.view.action_count == 0U;
            }
        }
        if (valid) {
            strlcpy(next.view.title, title->valuestring,
                    sizeof next.view.title);
            strlcpy(next.view.body, body->valuestring,
                    sizeof next.view.body);
        }
    } else if (valid && dismiss && payload->child != NULL) {
        valid = false;
    }

    if (!valid) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"invalid bounded brain envelope\"}");
        return ESP_OK;
    }
    uint32_t envelope_seq = (uint32_t)seq->valueint;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    strlcpy(next.session, session->valuestring, sizeof next.session);
    strlcpy(next.id, id->valuestring, sizeof next.id);
    next.inbox_seq = envelope_seq;
    next.expires_ms = now + (uint32_t)ttl_ms->valueint;
    next.active = present;
    cJSON_Delete(root);

    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "brain link unavailable");
        return ESP_OK;
    }
    if (s_brain_surface.local_owned) {
        /* A paired network client is still not a finger on this panel. Local
         * write consent cannot be replaced, updated, or dismissed remotely. */
        xSemaphoreGive(s_brain_lock);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"physical consent owns panel\"}");
        return ESP_OK;
    }
    uint32_t expected = s_brain_inbox_seq_hwm + 1U;
    if (envelope_seq != expected) {
        xSemaphoreGive(s_brain_lock);
        char conflict[128];
        int n = snprintf(conflict, sizeof conflict,
            "{\"ok\":false,\"error\":\"seq must equal next_inbox_seq\","
            "\"next_inbox_seq\":%u}", (unsigned)expected);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, conflict,
                        n > 0 && (size_t)n < sizeof conflict ? n : 0);
        return ESP_OK;
    }
    /* Updates and dismissals are conditional operations. A delayed client may
     * not mutate a newer surface merely because it owns the same pairing key. */
    if ((update || dismiss) &&
        (!s_brain_surface.active ||
         strcmp(s_brain_surface.session, next.session) != 0 ||
         strcmp(s_brain_surface.id, next.id) != 0)) {
        xSemaphoreGive(s_brain_lock);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"surface target is not active\"}");
        return ESP_OK;
    }
    esp_err_t surface_err = ESP_OK;
    if (present) {
        surface_err = jr_display_surface_present(&next.view);
    } else {
        jr_display_surface_dismiss();
    }
    if (surface_err != ESP_OK) {
        xSemaphoreGive(s_brain_lock);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"panel surface unavailable\"}");
        return ESP_OK;
    }
    s_brain_inbox_seq_hwm = envelope_seq;
    s_brain_last_seen_ms = now;
    if (present) {
        s_brain_surface = next;
    } else {
        s_brain_surface.active = false;
    }
    configASSERT(s_brain_surface.active == jr_display_surface_is_active());
    uint32_t next_seq = s_brain_inbox_seq_hwm + 1U;
    xSemaphoreGive(s_brain_lock);

    char accepted[96];
    int n = snprintf(accepted, sizeof accepted,
        "{\"ok\":true,\"accepted\":true,\"next_inbox_seq\":%u}",
        (unsigned)next_seq);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, accepted, n);
    return ESP_OK;
}

static esp_err_t brain_outbox_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) return ESP_OK;
    int after_query = 0;
    (void)query_int(req, "after", &after_query);
    uint32_t after = after_query > 0 ? (uint32_t)after_query : 0U;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    brain_surface_state_t surface = {0};
    brain_action_event_t events[BRAIN_EVENT_CAP] = {0};
    size_t event_count = 0U;
    uint32_t latest = 0U;
    uint32_t next_inbox = 1U;
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "brain link unavailable");
        return ESP_OK;
    }
    s_brain_last_seen_ms = now;
    if (s_brain_surface.active && !s_brain_surface.local_owned &&
        (int32_t)(now - s_brain_surface.expires_ms) >= 0) {
        jr_display_surface_dismiss();
        s_brain_surface.active = false;
        configASSERT(!jr_display_surface_is_active());
    }
    surface = s_brain_surface;
    latest = s_brain_event_seq;
    next_inbox = s_brain_inbox_seq_hwm + 1U;
    uint32_t earliest = latest >= BRAIN_EVENT_CAP
        ? latest - BRAIN_EVENT_CAP + 1U : 1U;
    uint32_t first = after + 1U;
    if (first < earliest) first = earliest;
    for (uint32_t seq_no = first;
         seq_no <= latest && event_count < BRAIN_EVENT_CAP; ++seq_no) {
        brain_action_event_t *event =
            &s_brain_events[(seq_no - 1U) % BRAIN_EVENT_CAP];
        if (event->seq == seq_no) events[event_count++] = *event;
    }
    xSemaphoreGive(s_brain_lock);

    char *body = heap_caps_malloc(3072U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "brain outbox busy");
        return ESP_OK;
    }
    int n = snprintf(body, 3072U,
        "{\"v\":1,\"voice_route\":\"cloud_gemini\","
        "\"desk_connected\":true,\"next_after\":%u,"
        "\"next_inbox_seq\":%u,\"surface\":{\"active\":%s",
        (unsigned)latest, (unsigned)next_inbox,
        surface.active ? "true" : "false");
    size_t used = n > 0 ? (size_t)n : 3072U;
    if (surface.active && used < 3072U) {
        n = snprintf(body + used, 3072U - used,
            ",\"session\":\"%s\",\"id\":\"%s\",\"kind\":\"%s\","
            "\"title\":\"%s\",\"body\":\"%s\",\"ttl_ms\":%u",
            surface.session, surface.id,
            brain_surface_kind_name(surface.view.kind), surface.view.title,
            surface.view.body,
            (unsigned)((int32_t)(surface.expires_ms - now) > 0
                ? surface.expires_ms - now : 0U));
        used += n > 0 ? (size_t)n : 3072U;
    }
    if (used < 3072U) {
        n = snprintf(body + used, 3072U - used, "},\"events\":[");
        used += n > 0 ? (size_t)n : 3072U;
    }
    for (size_t i = 0; i < event_count && used < 3072U; ++i) {
        n = snprintf(body + used, 3072U - used,
            "%s{\"v\":1,\"seq\":%u,\"type\":\"surface.action\","
            "\"session\":\"%s\",\"id\":\"%s\","
            "\"payload\":{\"action_id\":\"%s\",\"ts_ms\":%u}}",
            i == 0U ? "" : ",", (unsigned)events[i].seq,
            events[i].session, events[i].id, events[i].action_id,
            (unsigned)events[i].ts_ms);
        used += n > 0 ? (size_t)n : 3072U;
    }
    if (used < 3072U) {
        n = snprintf(body + used, 3072U - used, "]}");
        used += n > 0 ? (size_t)n : 3072U;
    }
    if (used >= 3072U) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "brain outbox encoding overflow");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, used);
    heap_caps_free(body);
    return err;
}

static esp_err_t cockpit_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const jr_state_snapshot_t *voice = jr_orch_snapshot(&s_app.orch);
    jr_display_diag_t display = {0};
    jr_net_status_t net = {0};
    agent_link_state_t agent = {0};
    uint32_t agent_revision_hwm = 0U;
    uint32_t agent_next_revision = 0U;
    brain_surface_state_t brain_surface = {0};
    uint32_t brain_inbox_next = 1U;
    uint32_t brain_event_cursor = 0U;
    bool desk_connected = false;
    (void)jr_display_get_diag(&display);
    (void)jr_net_get_status(&net);
    if (s_agent_link_lock != NULL &&
        xSemaphoreTake(s_agent_link_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_agent_link.active &&
            (int32_t)(now - s_agent_link.expires_ms) >= 0) {
            s_agent_link.active = false;
        }
        agent = s_agent_link;
        agent_revision_hwm = s_agent_link_revision_hwm;
        agent_next_revision = agent_revision_hwm + 1U;
        xSemaphoreGive(s_agent_link_lock);
    }
    if (s_brain_lock != NULL &&
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_brain_surface.active && !s_brain_surface.local_owned &&
            (int32_t)(now - s_brain_surface.expires_ms) >= 0) {
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
            configASSERT(!jr_display_surface_is_active());
        }
        brain_surface = s_brain_surface;
        brain_inbox_next = s_brain_inbox_seq_hwm + 1U;
        brain_event_cursor = s_brain_event_seq;
        desk_connected = s_brain_last_seen_ms != 0U &&
            (uint32_t)(now - s_brain_last_seen_ms) <= BRAIN_DESK_FRESH_MS;
        xSemaphoreGive(s_brain_lock);
    }

    char *body = heap_caps_malloc(4096U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "cockpit snapshot unavailable");
        return ESP_OK;
    }
    /* Always 0: this firmware is always-ready listening (VOICE_ALWAYS_READY),
     * so there is no listen window to count down. The deadline it used to read
     * was assigned only ever 0 at four sites. Kept as a field so existing
     * tooling keeps parsing; do not build a countdown rim on it. */
    const uint32_t auto_idle_ms = 0U;
    bool challenge_active = atomic_load(&s_touch_challenge_active);
    bool challenge_verified = atomic_load(&s_touch_challenge_verified);
    bool tools_ready = atomic_load(&s_tool_diag.worker_ready);
    bool tools_configured = tools_ready && jr_tools_is_configured();
    size_t used = 0U;
    int n = snprintf(body, 4096U,
        "{\"uptime_ms\":%u,\"memory\":{\"free_internal\":%u,"
        "\"largest_internal_block\":%u,\"free_psram\":%u},"
        "\"network\":{\"connected\":%s,"
        "\"ip\":\"%s\",\"rssi\":%d},\"voice\":{\"phase\":\"%s\","
        "\"voice_armed\":%s,\"always_ready\":true,"
        "\"privacy_paused\":%s,\"mood\":\"%s\",\"brightness\":%u,"
        "\"rtc\":%s,\"capturing\":%s,\"ws_connected\":%s,"
        "\"auto_idle_ms\":%u,\"mic_rms\":%.1f,\"clean_rms\":%.1f,"
        "\"vad_clean\":%s,"
        "\"vad_starts\":%u,"
        "\"audio_diag_running\":%s},\"tools\":{"
        "\"execution\":\"on_device\",\"worker_ready\":%s,"
        "\"configured\":%s,\"declared\":%u,\"last_tool\":\"%s\","
        "\"last_status\":\"%s\",\"last_http_status\":%d,"
        "\"last_duration_ms\":%u,"
        "\"calls_received\":%u,\"submitted\":%u,\"submit_rejected\":%u,"
        "\"completed\":%u,"
        "\"succeeded\":%u,\"failed\":%u,\"cancelled\":%u,"
        "\"stale_dropped\":%u,\"responses_sent\":%u,"
        "\"response_send_failed\":%u,\"consent_active\":%s,"
        "\"consent_prompted\":%u,\"consent_approved\":%u,"
        "\"consent_denied\":%u,\"consent_timed_out\":%u,"
        "\"consent_cancelled\":%u},\"display\":{\"init\":\"%s\","
        "\"actual_fps\":%u,\"flush_completions\":%u,\"flush_errors\":%u,"
        "\"requested_face\":%d,\"applied_face\":%d},\"touch\":{"
        "\"events\":%u,\"last\":{\"kind\":\"%s\",\"x\":%u,\"y\":%u},"
        "\"shade_open\":%s,\"panel_touch_challenge\":{\"pending\":%s,"
        "\"active\":%s,\"verified\":%s,\"correct_rounds\":%u,"
        "\"wrong\":%u,\"expected_sector\":%u,\"last_latency_ms\":%u}},"
        "\"agent\":{\"active\":%s,\"revision_hwm\":%u,"
        "\"next_revision\":%u",
        (unsigned)now,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        net.sta_connected ? "true" : "false", net.sta_ip,
        (int)net.rssi, jr_state_name(voice->phase),
        voice->phase != JR_ST_IDLE && voice->phase != JR_ST_DRAINING &&
            voice->phase != JR_ST_FATAL ? "true" : "false",
        atomic_load(&s_voice_privacy_paused) ? "true" : "false",
        jr_mood_name((jr_mood_t)atomic_load(&s_mood_id)),
        (unsigned)atomic_load(&s_mood_brightness),
        jr_rtc_present() ? "true" : "false",
        s_app.io.capturing ? "true" : "false",
        s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN ? "true" : "false",
        (unsigned)auto_idle_ms, (double)s_app.mic_rms,
        (double)jr_audio_clean_rms(),
        atomic_load(&s_vad_use_clean) ? "true" : "false",
        (unsigned)s_app.vad_starts,
        (int32_t)(atomic_load(&s_audio_diag_until_ms) - now) > 0
            ? "true" : "false",
        tools_ready ? "true" : "false",
        tools_configured ? "true" : "false",
        (unsigned)DEVICE_TOOL_DECL_COUNT,
        device_tool_last_name(),
        device_tool_last_status(),
        atomic_load(&s_tool_diag.last_http_status),
        (unsigned)atomic_load(&s_tool_diag.last_duration_ms),
        (unsigned)atomic_load(&s_tool_diag.calls_received),
        (unsigned)atomic_load(&s_tool_diag.submitted),
        (unsigned)atomic_load(&s_tool_diag.submit_rejected),
        (unsigned)atomic_load(&s_tool_diag.completed),
        (unsigned)atomic_load(&s_tool_diag.succeeded),
        (unsigned)atomic_load(&s_tool_diag.failed),
        (unsigned)atomic_load(&s_tool_diag.cancelled),
        (unsigned)atomic_load(&s_tool_diag.stale_dropped),
        (unsigned)atomic_load(&s_tool_diag.responses_sent),
        (unsigned)atomic_load(&s_tool_diag.response_send_failed),
        atomic_load(&s_tool_diag.consent_active) ? "true" : "false",
        (unsigned)atomic_load(&s_tool_diag.consent_prompted),
        (unsigned)atomic_load(&s_tool_diag.consent_approved),
        (unsigned)atomic_load(&s_tool_diag.consent_denied),
        (unsigned)atomic_load(&s_tool_diag.consent_timed_out),
        (unsigned)atomic_load(&s_tool_diag.consent_cancelled),
        display.init_state == JR_DISPLAY_INIT_READY ? "ready" :
        display.init_state == JR_DISPLAY_INIT_STARTING ? "starting" :
        display.init_state == JR_DISPLAY_INIT_FAILED ? "failed" : "stopped",
        (unsigned)display.actual_fps,
        (unsigned)display.flush_completions,
        (unsigned)display.flush_errors,
        (int)display.requested_face, (int)display.applied_face,
        (unsigned)atomic_load(&s_touch_events),
        touch_kind_name((jr_input_kind_t)atomic_load(&s_touch_last_kind)),
        (unsigned)atomic_load(&s_touch_last_x),
        (unsigned)atomic_load(&s_touch_last_y),
        s_ui_shade_open ? "true" : "false",
        atomic_load(&s_touch_challenge_start_requested) ? "true" : "false",
        challenge_active ? "true" : "false",
        challenge_verified ? "true" : "false",
        (unsigned)atomic_load(&s_touch_challenge_correct),
        (unsigned)atomic_load(&s_touch_challenge_wrong),
        (unsigned)atomic_load(&s_touch_challenge_expected),
        (unsigned)atomic_load(&s_touch_challenge_last_latency_ms),
        agent.active ? "true" : "false",
        (unsigned)agent_revision_hwm, (unsigned)agent_next_revision);
    if (n < 0 || (size_t)n >= 4096U) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "cockpit encoding failed");
        return ESP_OK;
    }
    used = (size_t)n;
    if (agent.active) {
        uint32_t ttl_ms = (int32_t)(agent.expires_ms - now) > 0
            ? agent.expires_ms - now : 0U;
        n = snprintf(body + used, 4096U - used,
            ",\"task_id\":\"%s\",\"revision\":%u,\"state\":\"%s\","
            "\"progress\":%u,\"title\":\"%s\",\"summary\":\"%s\","
            "\"ttl_ms\":%u,\"evidence\":[",
            agent.task_id, (unsigned)agent.revision, agent.state,
            (unsigned)agent.progress, agent.title, agent.summary,
            (unsigned)ttl_ms);
        if (n < 0 || (size_t)n >= 4096U - used) {
            used = 4096U;
        } else {
            used += (size_t)n;
            for (uint8_t i = 0; i < agent.evidence_count; ++i) {
                n = snprintf(body + used, 4096U - used,
                    "%s{\"label\":\"%s\",\"state\":\"%s\"}",
                    i == 0 ? "" : ",", agent.evidence[i].label,
                    agent.evidence[i].state);
                if (n < 0 || (size_t)n >= 4096U - used) {
                    used = 4096U;
                    break;
                }
                used += (size_t)n;
            }
            if (used < 4096U) {
                n = snprintf(body + used, 4096U - used, "]");
                used += n > 0 ? (size_t)n : 4096U;
            }
        }
    }
    if (used < 4096U) {
        uint32_t surface_ttl_ms = brain_surface.active &&
            (int32_t)(brain_surface.expires_ms - now) > 0
            ? brain_surface.expires_ms - now : 0U;
        n = snprintf(body + used, 4096U - used,
            "},\"brain\":{\"voice_route\":\"cloud_gemini\","
            "\"desk_connected\":%s,\"private_android_ready\":false,"
            "\"private_android_reason\":\"BLE firmware not enabled\","
            "\"next_inbox_seq\":%u,\"event_cursor\":%u,"
            "\"surface_active\":%s,\"surface_kind\":\"%s\","
            "\"surface_title\":\"%s\",\"surface_ttl_ms\":%u}}",
            desk_connected ? "true" : "false",
            (unsigned)brain_inbox_next, (unsigned)brain_event_cursor,
            brain_surface.active ? "true" : "false",
            brain_surface.active
                ? brain_surface_kind_name(brain_surface.view.kind) : "none",
            brain_surface.active ? brain_surface.view.title : "",
            (unsigned)surface_ttl_ms);
        if (n > 0 && (size_t)n < 4096U - used) {
            used += (size_t)n;
        } else {
            used = 4096U;
        }
    }
    if (used >= 4096U) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "cockpit encoding overflow");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, used);
    heap_caps_free(body);
    return err;
}

/* Presenter observability: init state, applied face/bucket, flush + asset
 * counters, actual fps. The "never ask the user what the screen is doing"
 * endpoint — pairs with the /api/gemini/live voice snapshot. */
/* Dump the VAD/barge ring as CSV, oldest -> newest. Columns:
 * t_ms,phase,event,barge_on,rms,floor,gate,peak_play. Streamed in chunks so a
 * 6000-row log never needs a big contiguous buffer. This is the barge-tuning
 * data source: have the user talk to the device, then GET this. */
static esp_err_t vadlog_csv_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    if (s_vadlog == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "vadlog unavailable");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "t_ms,phase,event,barge_on,rms,floor,gate,peak_play\n");

    uint32_t seq = atomic_load(&s_vadlog_seq);
    uint32_t total = seq < VADLOG_CAP ? seq : VADLOG_CAP;
    uint32_t start = seq < VADLOG_CAP ? 0 : seq % VADLOG_CAP;  /* oldest slot */
    char line[96];
    for (uint32_t k = 0; k < total; ++k) {
        const vadlog_entry_t *e = &s_vadlog[(start + k) % VADLOG_CAP];
        int n = snprintf(line, sizeof line, "%u,%u,%u,%u,%d,%d,%d,%d\n",
                         (unsigned)e->t_ms, (unsigned)e->phase, (unsigned)e->event,
                         (unsigned)e->barge_on, (int)e->rms, (int)e->floor,
                         (int)e->gate, (int)e->peak_play);
        if (n > 0 && httpd_resp_send_chunk(req, line, (size_t)n) != ESP_OK) {
            break;   /* client hung up */
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t display_diag_handler(httpd_req_t *req)
{
    jr_display_diag_t d;
    if (jr_display_get_diag(&d) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "diag failed");
        return ESP_OK;
    }
    static const char *init_names[] = { "stopped", "starting", "ready", "failed" };
    const char *init_name = d.init_state <= JR_DISPLAY_INIT_FAILED
                                ? init_names[d.init_state] : "?";
    char buf[512];
    int n = snprintf(buf, sizeof buf,
        "{\"init\":\"%s\",\"last_error\":\"%s\",\"task_running\":%s,"
        "\"blanked\":%s,\"requested_face\":%d,\"applied_face\":%d,"
        "\"requested_amplitude\":%u,\"applied_bucket\":%u,"
        "\"requests\":%u,\"state_changes\":%u,\"segment_sets\":%u,"
        "\"asset_load_failures\":%u,\"flush_submissions\":%u,"
        "\"flush_completions\":%u,\"flush_errors\":%u,\"actual_fps\":%u,"
        "\"current_asset_bytes\":%u,\"free_psram\":%u,\"stack_hwm\":%u}",
        init_name, esp_err_to_name(d.last_error),
        d.task_running ? "true" : "false",
        d.blanked ? "true" : "false",
        (int)d.requested_face, (int)d.applied_face,
        (unsigned)d.requested_amplitude, (unsigned)d.applied_bucket,
        (unsigned)d.requests, (unsigned)d.state_changes,
        (unsigned)d.segment_sets, (unsigned)d.asset_load_failures,
        (unsigned)d.flush_submissions, (unsigned)d.flush_completions,
        (unsigned)d.flush_errors, (unsigned)d.actual_fps,
        (unsigned)d.current_asset_bytes, (unsigned)d.free_psram_bytes,
        (unsigned)d.task_stack_hwm);
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encoding failed");
        return ESP_OK;
    }
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static const char *display_pattern_name(jr_display_test_pattern_t pattern)
{
    switch (pattern) {
    case JR_DISPLAY_TEST_OFF:        return "off";
    case JR_DISPLAY_TEST_COLOR_BARS: return "bars";
    case JR_DISPLAY_TEST_GRID:       return "grid";
    case JR_DISPLAY_TEST_WHITE:      return "white";
    case JR_DISPLAY_TEST_RED:        return "red";
    case JR_DISPLAY_TEST_GREEN:      return "green";
    case JR_DISPLAY_TEST_BLUE:       return "blue";
    case JR_DISPLAY_TEST_TOUCH_CHALLENGE: return "touch-challenge";
    default:                         return "unknown";
    }
}

static bool display_pattern_parse(const char *name,
                                  jr_display_test_pattern_t *out)
{
    if (strcmp(name, "off") == 0 || strcmp(name, "normal") == 0) {
        *out = JR_DISPLAY_TEST_OFF;
    } else if (strcmp(name, "bars") == 0 || strcmp(name, "color-bars") == 0) {
        *out = JR_DISPLAY_TEST_COLOR_BARS;
    } else if (strcmp(name, "grid") == 0) {
        *out = JR_DISPLAY_TEST_GRID;
    } else if (strcmp(name, "white") == 0) {
        *out = JR_DISPLAY_TEST_WHITE;
    } else if (strcmp(name, "red") == 0) {
        *out = JR_DISPLAY_TEST_RED;
    } else if (strcmp(name, "green") == 0) {
        *out = JR_DISPLAY_TEST_GREEN;
    } else if (strcmp(name, "blue") == 0) {
        *out = JR_DISPLAY_TEST_BLUE;
    } else {
        return false;
    }
    return true;
}

/* POST /api/display/canvas[?ttl=ms] — raw RGB565 little-endian, exactly
 * 466*466*2 bytes: the glass as a remote canvas (owner request 2026-08-27).
 * Body is streamed into a PSRAM staging buffer, handed to the display, and
 * freed; the display keeps its own copy with a TTL so an abandoned image can
 * never permanently cover the face. DELETE via ?clear=1. */
static esp_err_t display_canvas_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char query[64];
    uint32_t ttl_ms = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "clear", val, sizeof val) == ESP_OK &&
            val[0] == '1') {
            jr_display_canvas_clear();
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":true,\"cleared\":true}");
            return ESP_OK;
        }
        if (httpd_query_key_value(query, "ttl", val, sizeof val) == ESP_OK) {
            ttl_ms = (uint32_t)strtoul(val, NULL, 10);
        }
    }
    const size_t want = 466U * 466U * 2U;
    if (req->content_len != want) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "body must be raw RGB565 466x466 (434312 bytes)");
        return ESP_OK;
    }
    uint8_t *buf = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_OK;
    }
    size_t got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, (char *)buf + got, want - got);
        if (r <= 0) {
            heap_caps_free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
            return ESP_OK;
        }
        got += (size_t)r;
    }
    esp_err_t err = jr_display_canvas_show((const uint16_t *)buf, 466U, 466U,
                                           ttl_ms);
    heap_caps_free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "canvas: image pushed (ttl=%u ms)",
             (unsigned)(ttl_ms ? ttl_ms : 30000U));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/debug/input?kind=tap|double|long|swipe
 * [&dir=left|right|up|down] [&x=&y=&edge=1] synthesizes an event through the
 * real queue. "double" enqueues two taps inside the double-tap window. */
static esp_err_t debug_input_handler(httpd_req_t *req)
{
    if (!control_intent_required(req) || !agent_require_auth(req)) {
        return ESP_OK;
    }
    char query[96], kind[12] = {0}, dirs[12] = {0}, val[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "kind", kind, sizeof kind) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "kind=tap|double|long|swipe required");
        return ESP_OK;
    }
    jr_input_event_t ev = { .kind = JR_INPUT_TAP, .x = 233, .y = 233,
                            .start_x = 233, .start_y = 233,
                            .end_x = 233, .end_y = 233,
                            .duration_ms = 80,
                            .direction = JR_INPUT_DIRECTION_NONE, .flags = 0 };
    if (httpd_query_key_value(query, "x", val, sizeof val) == ESP_OK) {
        ev.x = ev.start_x = ev.end_x = (uint16_t)strtoul(val, NULL, 10);
    }
    if (httpd_query_key_value(query, "y", val, sizeof val) == ESP_OK) {
        ev.y = ev.start_y = ev.end_y = (uint16_t)strtoul(val, NULL, 10);
    }
    int repeats = 1;
    if (strcmp(kind, "long") == 0) {
        ev.kind = JR_INPUT_LONG_PRESS;
        ev.duration_ms = 1300;
    } else if (strcmp(kind, "double") == 0) {
        repeats = 2;
    } else if (strcmp(kind, "swipe") == 0) {
        ev.kind = JR_INPUT_SWIPE;
        ev.duration_ms = 250;
        (void)httpd_query_key_value(query, "dir", dirs, sizeof dirs);
        if (strcmp(dirs, "left") == 0) {
            ev.direction = JR_INPUT_DIRECTION_LEFT;
            ev.start_x = 400; ev.end_x = 60; ev.delta_x = -340;
        } else if (strcmp(dirs, "right") == 0) {
            ev.direction = JR_INPUT_DIRECTION_RIGHT;
            ev.start_x = 60; ev.end_x = 400; ev.delta_x = 340;
        } else if (strcmp(dirs, "up") == 0) {
            ev.direction = JR_INPUT_DIRECTION_UP;
            ev.start_y = 400; ev.end_y = 60; ev.delta_y = -340;
        } else if (strcmp(dirs, "down") == 0) {
            ev.direction = JR_INPUT_DIRECTION_DOWN;
            ev.start_y = 90; ev.end_y = 400; ev.delta_y = 310;
            if (httpd_query_key_value(query, "edge", val, sizeof val)
                    == ESP_OK && val[0] == '1') {
                ev.start_y = 20; ev.delta_y = 380;
                ev.flags = JR_INPUT_FLAG_TOP_EDGE;
            }
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "dir=left|right|up|down required for swipe");
            return ESP_OK;
        }
    } else if (strcmp(kind, "tap") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown kind");
        return ESP_OK;
    }
    esp_err_t err = ESP_OK;
    for (int i = 0; i < repeats && err == ESP_OK; ++i) {
        err = jr_hal_input_inject(&ev);
        if (repeats > 1 && i == 0) {
            vTaskDelay(pdMS_TO_TICKS(120));   /* inside the 400 ms window */
        }
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "debug: injected input kind=%s dir=%s", kind, dirs);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* GET /api/logs?tail=N — last N bytes of the log ring, chronological, plain
 * text. Reads are chunked with the mux held only per-chunk, so a concurrent
 * writer can at worst garble the OLDEST lines of a snapshot mid-read —
 * acceptable for a diagnostic tail, and it never stalls logging. */
static esp_err_t logs_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    if (s_logring == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "log ring unavailable");
        return ESP_OK;
    }
    size_t tail = 16384;
    char query[48], val[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "tail", val, sizeof val) == ESP_OK) {
        tail = (size_t)strtoul(val, NULL, 10);
    }
    portENTER_CRITICAL(&s_logring_mux);
    size_t len = s_logring_len;
    size_t head = s_logring_head;
    portEXIT_CRITICAL(&s_logring_mux);
    if (tail > len) {
        tail = len;
    }
    /* oldest byte of the requested window */
    size_t start = (head + LOGRING_CAP - tail) % LOGRING_CAP;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char chunk[1024];
    size_t sent = 0;
    while (sent < tail) {
        size_t n = tail - sent < sizeof chunk ? tail - sent : sizeof chunk;
        portENTER_CRITICAL(&s_logring_mux);
        for (size_t i = 0; i < n; ++i) {
            chunk[i] = s_logring[(start + sent + i) % LOGRING_CAP];
        }
        portEXIT_CRITICAL(&s_logring_mux);
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
            return ESP_OK;
        }
        sent += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void ota_restore_control_state(bool was_privacy_paused,
                                      uint32_t previous_lease_until_ms)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    /* Privacy is fail-closed: never arm if voice was private before OTA or a
     * physical gesture made it private while the upload was running. */
    const bool keep_private =
        was_privacy_paused || atomic_load(&s_voice_privacy_paused);
    atomic_store(&s_voice_control_request,
                 keep_private ? VOICE_CONTROL_PAUSE : VOICE_CONTROL_RESUME);

    /* A physical owner tap clears the OTA lease and must win. Otherwise restore
     * a still-live pre-existing lease rather than inventing a new one. */
    if (atomic_load(&s_operator_lease_until_ms) != 0U) {
        atomic_store(&s_operator_lease_until_ms,
                     (int32_t)(previous_lease_until_ms - now) > 0
                         ? previous_lease_until_ms : 0U);
    }
}

static bool ota_confirm_running_image_if_healthy(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running == NULL) {
        return false;
    }
    esp_err_t state_err = esp_ota_get_state_partition(running, &state);
    if (state_err != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return state_err == ESP_OK;
    }

    static uint32_t observed_flush_errors = UINT32_MAX;
    static uint32_t observed_flush_completions;
    static uint32_t flush_stable_since_ms;
    static uint32_t last_flush_progress_ms;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now >= 120000U) {
        ESP_LOGE(TAG, "ota: probation deadline failed; rolling back");
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
        return false;
    }
    const bool voice_alive = s_voice_task_running &&
        (uint32_t)(now - s_voice_task_heartbeat_ms) < 2000U;
    const bool subsystems_healthy =
        voice_alive && jr_net_is_connected() &&
        atomic_load(&s_tool_diag.worker_ready) &&
        atomic_load(&s_http_ready) && jr_wake_ready();
    if (!subsystems_healthy) {
        flush_stable_since_ms = now;
        return false;
    }

    jr_display_diag_t display = {0};
    if (jr_display_get_diag(&display) != ESP_OK ||
        display.init_state != JR_DISPLAY_INIT_READY ||
        !display.task_running) {
        return false;
    }
    /* Display stability must hold alongside every required subsystem. */
    if (display.flush_completions != observed_flush_completions) {
        observed_flush_completions = display.flush_completions;
        last_flush_progress_ms = now;
    }
    if (display.flush_errors != observed_flush_errors) {
        observed_flush_errors = display.flush_errors;
        flush_stable_since_ms = now;
        return false;
    }
    if (display.actual_fps < 12U ||
        last_flush_progress_ms == 0U ||
        (uint32_t)(now - last_flush_progress_ms) > 1000U) {
        flush_stable_since_ms = now;
        return false;
    }
    if ((uint32_t)(now - flush_stable_since_ms) < 10000U) {
        return false;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        uint8_t slot = strcmp(running->label, "ota_0") == 0 ? 0U :
                       strcmp(running->label, "ota_1") == 0 ? 1U : 0xFFU;
        jr_display_ota_set(JR_DISPLAY_OTA_VALID, 100U, slot, 0xFFU, true);
        persist_ota_attempt(-1);
        ESP_LOGI(TAG,
                 "ota: image valid after voice/display stable window");
        return true;
    }
    ESP_LOGE(TAG, "ota: could not mark running image valid: %s",
             esp_err_to_name(err));
    return false;
}

/* POST /api/ota/upload — stream a jarvisrobot_v5.bin into the IDLE app slot
 * over Wi-Fi, set it as boot, reboot. The last cable-flash killer: the live
 * app keeps running (UI may stutter during erase bursts — flash-cache
 * physics — but Wi-Fi, voice state, and the glass survive), and a failed
 * write leaves the RUNNING slot untouched. Control-gated; auto-claims the
 * operator lease so the glass announces itself. */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    const size_t len = req->content_len;
    atomic_store(&s_ota_active, false);
    atomic_store(&s_ota_received_bytes, 0U);
    atomic_store(&s_ota_total_bytes, (uint32_t)len);
    if (len < 256U * 1024U || len > 0x400000U) {
        atomic_store(&s_ota_last_error, ESP_ERR_INVALID_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "implausible app image size");
        return ESP_OK;
    }
    atomic_store(&s_ota_preflight_blocked, false);
    const ota_preflight_t preflight = ota_preflight();
    jr_display_nav_set(JR_DISPLAY_SPACE_SETTINGS);
    jr_display_nav_up();
    jr_display_ota_set(JR_DISPLAY_OTA_PREFLIGHT, 0U,
                       preflight.active_slot, preflight.target_slot,
                       preflight.ok);
    if (!preflight.ok) {
        atomic_store(&s_ota_preflight_blocked, true);
        atomic_store(&s_ota_last_error, ESP_OK);
        jr_display_ota_set(JR_DISPLAY_OTA_BLOCKED, 0U,
                           preflight.active_slot, preflight.target_slot,
                           false);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, preflight.reason);
        return ESP_OK;
    }
    const esp_partition_t *next = preflight.target;
    const uint32_t upload_started_ms =
        (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t upload_deadline_ms = upload_started_ms + 240000U;
    const uint32_t previous_lease_until_ms =
        atomic_load(&s_operator_lease_until_ms);
    const bool was_privacy_paused =
        atomic_load(&s_voice_privacy_paused);
    atomic_store(&s_ota_last_error, ESP_OK);
    atomic_store(&s_ota_active, true);
    atomic_store(&s_operator_lease_until_ms, upload_started_ms + 180000U);
    atomic_store(&s_voice_control_request, VOICE_CONTROL_PAUSE);
    jr_display_caption_set("UPDATING - DO NOT UNPLUG");
    ESP_LOGI(TAG, "ota: receiving %u bytes into %s", (unsigned)len,
             next->label);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(next, len, &ota);
    if (err != ESP_OK) {
        atomic_store(&s_ota_active, false);
        atomic_store(&s_ota_last_error, err);
        jr_display_caption_set("UPDATE FAILED - STILL ON OLD");
        ota_restore_control_state(was_privacy_paused,
                                  previous_lease_until_ms);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        ESP_LOGE(TAG, "ota: begin failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    const uint8_t active_slot = preflight.active_slot;
    jr_display_ota_set(JR_DISPLAY_OTA_RECEIVING, 0U, active_slot,
                       preflight.target_slot, true);
    /* Settings detail already owns the glass from preflight. */
    /* Keep the staging buffer internal: esp_ota_write performs flash
     * operations with the cache disabled, so this avoids depending on
     * external-memory cache behavior in the update path. */
    const size_t chunk_size = 4096U;
    char *buf = heap_caps_malloc(chunk_size,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t got = 0;
    unsigned recv_timeouts = 0U;
    if (buf == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    while (got < len && err == ESP_OK) {
        uint32_t receive_now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(receive_now - upload_deadline_ms) >= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int r = httpd_req_recv(req, buf,
                               len - got < chunk_size
                                   ? len - got : chunk_size);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++recv_timeouts < 6U) {
                continue;
            }
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        recv_timeouts = 0U;
        err = esp_ota_write(ota, buf, (size_t)r);
        got += (size_t)r;
        atomic_store(&s_ota_received_bytes, (uint32_t)got);
        atomic_store(&s_operator_lease_until_ms,
                     (uint32_t)(esp_timer_get_time() / 1000) + 180000U);
    }
    heap_caps_free(buf);
    if (err == ESP_OK && got == len) {
        err = esp_ota_end(ota);
    } else {
        (void)esp_ota_abort(ota);
        err = err == ESP_OK ? ESP_FAIL : err;
    }
    if (err == ESP_OK) {
        esp_app_desc_t app_desc = {0};
        err = esp_ota_get_partition_description(next, &app_desc);
        if (err == ESP_OK &&
            strcmp(app_desc.project_name, "jarvisrobot_v5") != 0) {
            ESP_LOGE(TAG, "ota: refusing project \"%s\"", app_desc.project_name);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(next);
    }
    if (err != ESP_OK) {
        atomic_store(&s_ota_active, false);
        atomic_store(&s_ota_last_error, err);
        jr_display_ota_set(
            JR_DISPLAY_OTA_FAILED,
            len > 0U ? (uint8_t)((got * 100U) / len) : 0U,
            active_slot, strcmp(next->label, "ota_0") == 0 ? 0U : 1U, true);
        jr_display_caption_set("UPDATE FAILED - STILL ON OLD");
        ota_restore_control_state(was_privacy_paused,
                                  previous_lease_until_ms);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        ESP_LOGE(TAG, "ota: failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    persist_ota_attempt(strcmp(next->label, "ota_0") == 0 ? 0 : 1);
    ESP_LOGI(TAG, "ota: %u bytes verified into %s — rebooting to swap",
             (unsigned)got, next->label);
    jr_display_caption_set("UPDATE OK - RESTARTING");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

/* POST /api/operator/lease?ttl=seconds — claim; ?release=1 — hand back. */
static esp_err_t operator_lease_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    char query[64];
    char val[16] = {0};
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "release", val, sizeof val) == ESP_OK &&
        val[0] == '1') {
        (void)operator_mode_release(now, "remote", false, false);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"leased\":false}");
        return ESP_OK;
    }
    uint32_t ttl_s = 300U;
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "ttl", val, sizeof val) == ESP_OK) {
        ttl_s = (uint32_t)strtoul(val, NULL, 10);
    }
    if (ttl_s < 10U) ttl_s = 10U;
    if (ttl_s > 900U) ttl_s = 900U;
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "operator mode unavailable");
        return ESP_OK;
    }
    atomic_store(&s_operator_mode_entered_ms, now);
    atomic_store(&s_operator_lease_until_ms, now + ttl_s * 1000U);
    atomic_store(&s_voice_control_request, VOICE_CONTROL_PAUSE);
    jr_display_caption_set("CODEX MODE - DOUBLE TAP TO EXIT");
    atomic_store(&s_operator_mode_active, true); /* publish complete state last */
    xSemaphoreGive(s_brain_lock);
    ESP_LOGI(TAG, "operator: Codex mode claimed for %u s", (unsigned)ttl_s);
    char body[64];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":true,\"leased\":true,\"mode\":\"codex\",\"ttl_s\":%u}",
                     (unsigned)ttl_s);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t operator_status_handler(httpd_req_t *req)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t until = atomic_load(&s_operator_lease_until_ms);
    const bool active = operator_mode_active(now);
    const uint32_t ttl_ms = active && (int32_t)(until - now) > 0
        ? until - now : 0U;
    char body[96];
    int n = snprintf(body, sizeof body,
                     "{\"active\":%s,\"mode\":\"%s\",\"ttl_ms\":%u}",
                     active ? "true" : "false",
                     active ? "codex" : "normal",
                     (unsigned)ttl_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t display_test_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char query[96];
    char name[24] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "pattern", name, sizeof name) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                           "missing ?pattern=off|bars|grid|white|red|green|blue");
        return ESP_OK;
    }
    jr_display_test_pattern_t pattern;
    if (!display_pattern_parse(name, &pattern)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown pattern");
        return ESP_OK;
    }
    esp_err_t err = jr_display_set_test_pattern(pattern);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }
    char body[96];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":true,\"pattern\":\"%s\"}",
                     display_pattern_name(pattern));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t display_snapshot_info_handler(httpd_req_t *req)
{
    jr_display_snapshot_info_t info;
    esp_err_t err = jr_display_snapshot_get_info(&info);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        char body[128];
        int n = snprintf(body, sizeof body,
                         "{\"available\":false,\"error\":\"%s\"}",
                         esp_err_to_name(err));
        httpd_resp_send(req, body, n);
        return ESP_OK;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool fresh = info.valid &&
        (uint32_t)(now_ms - (uint32_t)info.last_flush_ms) <= 1000U;
    char body[384];
    int n = snprintf(body, sizeof body,
        "{\"available\":true,\"capture_source\":\"panel_submission_mirror\","
        "\"panel_readback\":false,\"width\":%u,\"height\":%u,"
        "\"bytes\":%u,\"frame_id\":%llu,\"last_flush_ms\":%llu,"
        "\"valid\":%s,\"mirror_fresh\":%s,\"test_pattern\":\"%s\"}",
        (unsigned)info.width, (unsigned)info.height, (unsigned)info.bytes,
        (unsigned long long)info.frame_id,
        (unsigned long long)info.last_flush_ms,
        info.valid ? "true" : "false", fresh ? "true" : "false",
        display_pattern_name(info.test_pattern));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static void panel_rgb565_to_rgb888(uint16_t panel_px, uint8_t out[3])
{
    uint16_t px = __builtin_bswap16(panel_px);
    uint8_t r = (uint8_t)((px >> 11) & 0x1fU);
    uint8_t g = (uint8_t)((px >> 5) & 0x3fU);
    uint8_t b = (uint8_t)(px & 0x1fU);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

static esp_err_t display_snapshot_ppm_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    enum { PPM_BATCH_ROWS = 8 };
    jr_display_snapshot_info_t info;
    esp_err_t err = jr_display_snapshot_get_info(&info);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display mirror unavailable");
        return ESP_OK;
    }
    if (!info.valid) {
        vTaskDelay(pdMS_TO_TICKS(120));
        err = jr_display_snapshot_get_info(&info);
    }
    uint16_t *frame = err == ESP_OK && info.bytes > 0
        ? heap_caps_malloc(info.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : NULL;
    if (frame == NULL ||
        jr_display_snapshot_copy_rgb565(frame, info.bytes, &info) != ESP_OK) {
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display mirror not ready");
        return ESP_OK;
    }

    size_t pixels = (size_t)info.width * (size_t)info.height;
    size_t lit = 0;
    uint32_t checksum = 2166136261U;
    for (size_t i = 0; i < pixels; ++i) {
        uint16_t native = __builtin_bswap16(frame[i]);
        if (native != 0U) {
            lit++;
        }
        checksum = (checksum ^ (uint8_t)frame[i]) * 16777619U;
        checksum = (checksum ^ (uint8_t)(frame[i] >> 8)) * 16777619U;
    }

    char header[64];
    int header_len = snprintf(header, sizeof header, "P6\n%u %u\n255\n",
                              (unsigned)info.width, (unsigned)info.height);
    size_t rgb_row_bytes = (size_t)info.width * 3U;
    size_t rgb_batch_bytes = rgb_row_bytes * PPM_BATCH_ROWS;
    uint8_t *rgb_rows = heap_caps_malloc(
        rgb_batch_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rgb_rows == NULL) {
        rgb_rows = heap_caps_malloc(
            rgb_batch_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (rgb_rows == NULL) {
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display conversion buffer unavailable");
        return ESP_OK;
    }
    char frame_value[32];
    char lit_value[32];
    char checksum_value[32];
    char width_value[16];
    char height_value[16];
    snprintf(frame_value, sizeof frame_value, "%llu",
             (unsigned long long)info.frame_id);
    snprintf(lit_value, sizeof lit_value, "%u", (unsigned)lit);
    snprintf(checksum_value, sizeof checksum_value, "%08x",
             (unsigned)checksum);
    snprintf(width_value, sizeof width_value, "%u", (unsigned)info.width);
    snprintf(height_value, sizeof height_value, "%u", (unsigned)info.height);
    httpd_resp_set_type(req, "image/x-portable-pixmap");
    err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Source", "panel_submission_mirror");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Owner", "jr_display");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Panel-Readback", "false");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Fresh", "true");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Frame", frame_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Width", width_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Height", height_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Lit-Pixels", lit_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Frame-Checksum", checksum_value);
    if (err != ESP_OK) {
        heap_caps_free(rgb_rows);
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display metadata unavailable");
        return ESP_OK;
    }

    err = httpd_resp_send_chunk(req, header, header_len);
    for (uint16_t y = 0; y < info.height && err == ESP_OK;
         y = (uint16_t)(y + PPM_BATCH_ROWS)) {
        uint16_t rows = (uint16_t)(info.height - y);
        if (rows > PPM_BATCH_ROWS) {
            rows = PPM_BATCH_ROWS;
        }
        for (uint16_t batch_y = 0; batch_y < rows; ++batch_y) {
            const uint16_t *src = frame +
                (size_t)(y + batch_y) * info.width;
            uint8_t *dst = rgb_rows + (size_t)batch_y * rgb_row_bytes;
            for (uint16_t x = 0; x < info.width; ++x) {
                panel_rgb565_to_rgb888(src[x], dst + (size_t)x * 3U);
            }
        }
        err = httpd_resp_send_chunk(req, (const char *)rgb_rows,
                                    (ssize_t)((size_t)rows * rgb_row_bytes));
    }
    heap_caps_free(rgb_rows);
    heap_caps_free(frame);
    if (err != ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Native RGB565 mirror for the cockpit and CLI. The snapshot buffer already
 * contains the exact bytes submitted to the panel; sending it once avoids the
 * 50% PPM expansion and conversion/network churn on-device. On this
 * little-endian S3 with panel byte swap enabled, the byte stream is native
 * RGB565 in big-endian order (high byte, low byte). */
static esp_err_t display_snapshot_rgb565_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    jr_display_snapshot_info_t info;
    esp_err_t err = jr_display_snapshot_get_info(&info);
    uint16_t *frame = err == ESP_OK && info.bytes > 0U
        ? heap_caps_malloc(info.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : NULL;
    if (frame == NULL ||
        jr_display_snapshot_copy_rgb565(frame, info.bytes, &info) != ESP_OK) {
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display mirror not ready");
        return ESP_OK;
    }

    char frame_value[32];
    char width_value[16];
    char height_value[16];
    snprintf(frame_value, sizeof frame_value, "%llu",
             (unsigned long long)info.frame_id);
    snprintf(width_value, sizeof width_value, "%u", (unsigned)info.width);
    snprintf(height_value, sizeof height_value, "%u", (unsigned)info.height);
    httpd_resp_set_type(req, "application/octet-stream");
    err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Source", "panel_submission_mirror");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Owner", "jr_display");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Panel-Readback", "false");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Fresh", "true");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-RGB565-Order", "big-endian-native");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Frame", frame_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Width", width_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Height", height_value);
    if (err != ESP_OK) {
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display metadata unavailable");
        return ESP_OK;
    }
    err = httpd_resp_send(req, (const char *)frame, (ssize_t)info.bytes);
    heap_caps_free(frame);
    return err;
}

static void start_diag_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    /* Live watermark: 3,880 B free at 7,168 after doctor/snapshots/tools.
     * 6,400 retains ~3.1 KB measured margin and returns another 768 B. */
    cfg.stack_size = 6400;
    cfg.max_uri_handlers = 48;
    cfg.max_resp_headers = 16;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "diag httpd failed to start");
        return;
    }
    httpd_uri_t routes[] = {
        { .uri = "/",                .method = HTTP_GET, .handler = dashboard_handler },
        { .uri = "/api/cockpit",     .method = HTTP_GET, .handler = cockpit_handler },
        { .uri = "/api/gemini/live", .method = HTTP_GET, .handler = diag_get_handler },
        { .uri = "/api/debug/say",   .method = HTTP_POST, .handler = say_get_handler  },
        { .uri = "/api/debug/gain",  .method = HTTP_POST, .handler = gain_get_handler },
        { .uri = "/api/voice/control", .method = HTTP_POST,
          .handler = voice_control_handler },
        { .uri = "/api/audio/self-test", .method = HTTP_POST,
          .handler = audio_self_test_handler },
        { .uri = "/api/audio/taps", .method = HTTP_GET,
          .handler = audio_taps_handler },
        { .uri = "/api/audio/tap.wav", .method = HTTP_GET,
          .handler = audio_tap_wav_handler },
        { .uri = "/api/touch", .method = HTTP_GET,
          .handler = touch_status_handler },
        { .uri = "/api/sensors", .method = HTTP_GET,
          .handler = sensors_handler },
        { .uri = "/api/diag/tasks", .method = HTTP_GET,
          .handler = tasks_diag_handler },
        { .uri = "/api/display/hud", .method = HTTP_POST,
          .handler = hud_toggle_handler },
        { .uri = "/api/display/choices", .method = HTTP_POST,
          .handler = choices_debug_handler },
        { .uri = "/api/display/choices/hit", .method = HTTP_GET,
          .handler = choices_hit_handler },
        { .uri = "/api/input/tap", .method = HTTP_POST,
          .handler = tap_sim_handler },
        { .uri = "/api/demo", .method = HTTP_POST,
          .handler = demo_handler },
        { .uri = "/api/diag/panel-touch", .method = HTTP_POST,
          .handler = panel_touch_control_handler },
        { .uri = "/api/ui/shade", .method = HTTP_POST,
          .handler = ui_shade_control_handler },
        { .uri = "/api/agent/link", .method = HTTP_GET,
          .handler = agent_link_get_handler },
        { .uri = "/api/agent/link", .method = HTTP_POST,
          .handler = agent_link_post_handler },
        { .uri = "/api/brain/outbox", .method = HTTP_GET,
          .handler = brain_outbox_handler },
        { .uri = "/api/brain/inbox", .method = HTTP_POST,
          .handler = brain_inbox_handler },
        { .uri = "/api/tools/config", .method = HTTP_GET,
          .handler = device_tool_config_get_handler },
        { .uri = "/api/tools/config", .method = HTTP_POST,
          .handler = device_tool_config_post_handler },
        { .uri = "/api/device/levels", .method = HTTP_GET,
          .handler = device_levels_get_handler },
        { .uri = "/api/device/levels", .method = HTTP_POST,
          .handler = device_levels_post_handler },
        { .uri = "/api/pairing/claim", .method = HTTP_POST,
          .handler = pairing_claim_handler },
        { .uri = "/api/display",     .method = HTTP_GET, .handler = display_diag_handler },
        { .uri = "/api/diag/vadlog", .method = HTTP_GET, .handler = vadlog_csv_handler },
        { .uri = "/api/device/health", .method = HTTP_GET,
          .handler = device_health_handler },
        { .uri = "/api/display/snapshot.json", .method = HTTP_GET,
          .handler = display_snapshot_info_handler },
        { .uri = "/api/display/snapshot.ppm", .method = HTTP_GET,
          .handler = display_snapshot_ppm_handler },
        { .uri = "/api/display/snapshot.rgb565", .method = HTTP_GET,
          .handler = display_snapshot_rgb565_handler },
        { .uri = "/api/display/test", .method = HTTP_POST,
          .handler = display_test_handler },
        { .uri = "/api/display/canvas", .method = HTTP_POST,
          .handler = display_canvas_handler },
        { .uri = "/api/operator/lease", .method = HTTP_GET,
          .handler = operator_status_handler },
        { .uri = "/api/operator/lease", .method = HTTP_POST,
          .handler = operator_lease_handler },
        { .uri = "/api/logs", .method = HTTP_GET,
          .handler = logs_handler },
        { .uri = "/api/debug/input", .method = HTTP_POST,
          .handler = debug_input_handler },
        { .uri = "/api/ota/upload", .method = HTTP_POST,
          .handler = ota_upload_handler },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "diag route registration failed uri=%s err=%s",
                     routes[i].uri, esp_err_to_name(err));
        }
    }
    atomic_store(&s_http_ready, true);
    ESP_LOGI(TAG, "diag http up: voice control, audio taps, display mirror/test");
#if JR_DEV_OPEN_DIAGNOSTICS
    ESP_LOGW(TAG, "************************************************************");
    ESP_LOGW(TAG, "DEV MODE: pairing token NOT required on diagnostic endpoints");
    ESP_LOGW(TAG, "Anything on this LAN can read logs, hear mic taps, drive the");
    ESP_LOGW(TAG, "display and inject input. Set JR_DEV_OPEN_DIAGNOSTICS 0 to");
    ESP_LOGW(TAG, "restore auth before shipping.");
    ESP_LOGW(TAG, "************************************************************");
#endif
}

/* ======================================================================== *
 *  the single-writer voice task                                            *
 * ======================================================================== */
static void handle_say(const char *text)
{
    /* Arm a session if idle, then QUEUE the text turn. It is flushed by
     * voice_task only once the session reaches Listening — sending it here
     * (transport still CONNECTING, setup not yet sent) is out-of-order and was
     * the first-boot crash trigger. */
    jr_state_t p = jr_orch_phase(&s_app.orch);
    uint64_t now = jr_clock_now_ms(&s_app.clock);
    ESP_LOGI(TAG, "arm: say received (phase=%s) text=\"%.32s\"", jr_state_name(p), text);
    if (p == JR_ST_IDLE || p == JR_ST_BACKOFF || p == JR_ST_FATAL) {
        jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_START), now);
        jr_orch_step(&s_app.orch, now);   /* drive Connect (async ws.connect) */
        ESP_LOGI(TAG, "arm: USER_START -> phase=%s", jr_state_name(jr_orch_phase(&s_app.orch)));
    }
    strlcpy(s_pending_text, text, sizeof s_pending_text);
    s_pending_text_set = true;
    s_pending_text_inflight = false;
    s_pending_text_retry_ms = 0;
}

/* ---- POLISH-06: the attract reel — 27 s of everything, on demand --------- *
 * A scripted showcase of the shipped features, driven entirely from the app
 * task (every display call below is app-task single-writer by contract).
 * Reality always wins: a live ask aborts the reel, and any tap ends it.
 * (State lives with the other request lanes near the top of the file.) */
static void demo_stop(void)
{
    if (s_demo_start_ms == 0U) {
        return;
    }
    s_demo_start_ms = 0U;
    s_demo_step = -1;
    jr_display_dismiss_choices();
    jr_display_clock_set(false, 0, 0, 0);
    caption_reset();
    ESP_LOGI(TAG, "demo: reel ended");
}

static void demo_tick(uint64_t now, jr_face_t *f, uint8_t *amp)
{
    static const char *const kDemoLabels[HUD_CHOICE_MAX] = {
        "Yes", "Later", "Ignore"
    };
    if (atomic_exchange(&s_demo_req, false)) {
        jr_state_t p = jr_orch_phase(&s_app.orch);
        if (s_demo_start_ms == 0U &&
            (p == JR_ST_LISTENING || p == JR_ST_IDLE)) {
            s_demo_start_ms = (uint32_t)now;
            s_demo_step = -1;
            ESP_LOGI(TAG, "demo: reel started");
        }
    }
    if (s_demo_start_ms == 0U) {
        return;
    }
    if (jr_orch_phase(&s_app.orch) == JR_ST_ASKING) {
        demo_stop();   /* a real question owns the glass */
        return;
    }
    uint32_t el = (uint32_t)now - s_demo_start_ms;
    int step = el < 6000U ? 0 : el < 14000U ? 1 : el < 19000U ? 2
             : el < 24000U ? 3 : el < 27000U ? 4 : 5;
    if (step != s_demo_step) {
        s_demo_step = step;
        switch (step) {
        case 0:
            jr_display_caption_set("JARVISNANO V5");
            break;
        case 1:
            s_demo_last_ripple = -1;
            jr_display_present_choices("Tap arcs answer Gemini",
                                       kDemoLabels, 3);
            break;
        case 2:
            jr_display_dismiss_choices();
            jr_display_caption_set("LIVE CAPTIONS AS I SPEAK");
            break;
        case 3: {
            time_t tt = time(NULL);
            struct tm tmv;
            localtime_r(&tt, &tmv);
            jr_display_clock_set(true, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            jr_display_caption_set("AMBIENT WATCH WHEN MUTED");
            break;
        }
        case 4:
            jr_display_clock_set(false, 0, 0, 0);
            jr_display_caption_set("SELF-HEALING CONNECTION");
            break;
        default:
            demo_stop();
            return;
        }
    }
    if (step == 1) {
        int sel = (int)((el - 6000U) / 2600U);
        if (sel > 2) {
            sel = 2;
        }
        jr_display_set_choice_selected(sel);
        if (sel != s_demo_last_ripple) {
            s_demo_last_ripple = sel;
            static const int rx[3] = {460, 233, 7};
            static const int ry[3] = {233, 459, 233};
            jr_display_ripple(rx[sel], ry[sel]);
        }
    }
    *f = step == 0 ? JR_FACE_THINKING
       : step == 2 ? JR_FACE_SPEAKING
       : step == 4 ? JR_FACE_ERROR
       : JR_FACE_LISTENING;
    if (step == 2) {
        *amp = (uint8_t)(128 + (int)(120.0f * sinf((float)el * 0.012f)));
    }
}

/* Drain order: synthetic diag taps first, then the HAL touch queue. Both come
 * out through the same jr_input_event_t so the loop below cannot tell them
 * apart — a simulated tap exercises exactly the code a finger does. */
static bool input_next(jr_input_t *in, jr_input_event_t *iev)
{
    uint32_t sim = atomic_exchange(&s_sim_touch, 0U);
    if (sim != 0U) {
        memset(iev, 0, sizeof *iev);
        iev->kind = JR_INPUT_TAP;
        iev->x = (uint16_t)((sim >> 16) - 1U);
        iev->y = (uint16_t)((sim & 0xFFFFU) - 1U);
        iev->flags = JR_INPUT_FLAG_SYNTHETIC;
        iev->emitted_ms = (uint32_t)(esp_timer_get_time() / 1000);
        return true;
    }
    return jr_input_poll(in, iev);
}

static void voice_task(void *arg)
{
    (void)arg;
    static jr_pcm_t mic_frame[VOICE_FRAME_SAMPLES];
    /* Server-VAD uplink batch: 2 frames (64 ms) per WS send. A four-frame
     * JSON body is ~5.6 KB, larger than JR_GEMINI_TXQ_SLOT (4096), so every
     * would-block dropped a full 128 ms of microphone audio. Two frames stay
     * queueable under backpressure while halving the per-frame send rate. */
    static jr_pcm_t mic_batch[2 * VOICE_FRAME_SAMPLES];
    static size_t   mic_batch_fill = 0;
    static bool codex_tap_pending;
    static jr_input_event_t codex_pending_tap;
    static uint32_t codex_pending_tap_ms;
    char say[200];

    /* app_main creates this task before the tool queues on purpose: the voice
     * stack is internal-RAM-only, while the subsequent worker allocations can
     * fall back to PSRAM. Do not enter the composition graph until app_main has
     * finished the remaining worker/HTTP initialization. */
    while (!atomic_load(&s_voice_start_gate)) {
        vTaskDelay(1);
    }
    s_voice_task_running = true;

    for (;;) {
        uint64_t now = jr_clock_now_ms(&s_app.clock);
        s_voice_task_heartbeat_ms = (uint32_t)now;

        if (atomic_load(&s_operator_mode_active) &&
            !operator_lease_active((uint32_t)now)) {
            (void)operator_mode_release((uint32_t)now, "ttl", false, true);
        }

        /* Leave a freshly booted OTA slot pending through a real probation
         * window. Reaching this point proves the voice owner is alive; display
         * readiness and zero flush errors are checked before cancelling
         * bootloader rollback. */
        static bool s_ota_validation_complete;
        if (!s_ota_validation_complete && now >= 45000U) {
            s_ota_validation_complete =
                ota_confirm_running_image_if_healthy();
        }

        /* Gemini can deliver generationComplete before the decoded PCM ring
         * and codec DMA have finished. Keep Speaking until the actual output
         * tail drains, then feed exactly one terminal boundary to the core. */
        if (s_app.terminal_pending && !jr_audio_playback_pending()) {
            s_app.terminal_pending = false;
            jr_orch_inject(&s_app.orch,
                           jr_event(JR_EV_SERVER_TURN_COMPLETE), now);
        }

        /* 1) pump the pure orchestrator (drains inbound, executes commands) */
        jr_orch_step(&s_app.orch, now);
        static uint32_t observed_tx_drops;
        const uint32_t tx_drops = s_app.client.live.tx_drops;
        if (tx_drops != observed_tx_drops) {
            observed_tx_drops = tx_drops;
            atomic_store(&s_last_tx_drop_ms, (uint32_t)now);
        }

        /* 1b) feed the HUD layer the world it cannot see: battery charge and
         * device tilt. Throttled to ~10 Hz — tilt parallax is a slow lean, not
         * a spirit level, and the battery moves on the scale of minutes. Both
         * reads are non-blocking snapshot copies (their samplers own the shared
         * I2C bus), which is precisely why they were built that way: this runs
         * inside the voice pump and must never stall it. */
        static uint32_t s_hud_env_next_ms;
        if ((int32_t)((uint32_t)now - s_hud_env_next_ms) >= 0) {
            s_hud_env_next_ms = (uint32_t)now + 100U;
            jr_imu_t imu = {0};
            jr_power_t bat = {0};
            const bool have_imu = jr_imu_read(&imu) == ESP_OK;
            const bool have_power = jr_power_read(&bat) == ESP_OK;
            /* MOTION screen: live tilt at the IMU's existing cadence, no new
             * sampling. Clamped to int8 degrees — the renderer maps ±90 to the
             * focal radius and pins beyond it. */
            if (have_imu) {
                /* In-plane gravity, hundredths of g. NOT roll/pitch: roll is
                 * atan2(gy, gz) and this board reads gz ~= -1 g face-up, so a
                 * flat device reports roll ~= 177 and a bubble driven from it
                 * pins at the rim forever (measured 2026-08-29). gx/gy are
                 * ~0 when flat, which is what a level needs.
                 *
                 * If the bubble runs the wrong way on hardware, flip a sign
                 * HERE, not in jr_imu — this codebase has already been bitten
                 * twice by IMU axis conventions being "fixed" at the source. */
                float gx = imu.gx * 100.0f, gy = imu.gy * 100.0f;
                if (gx >  100.0f) gx =  100.0f;
                if (gx < -100.0f) gx = -100.0f;
                if (gy >  100.0f) gy =  100.0f;
                if (gy < -100.0f) gy = -100.0f;
                jr_display_motion_set((int8_t)gx, (int8_t)gy);
            }
            jr_display_set_hud_env(
                bat.percent, bat.charging,
                atomic_load(&s_voice_privacy_paused),
                have_imu ? imu.roll_deg : 0.0f,
                have_imu ? imu.pitch_deg : 0.0f);
            jr_display_power_set(
                have_power ? bat.percent : 0xFFU,
                have_power ? bat.millivolts : 0U,
                have_power && bat.usb_present,
                have_power && bat.charging);

            static bool power_seen;
            static bool last_usb;
            static bool last_charging;
            static uint32_t last_power_seq;
            if (have_power && bat.sample_seq != last_power_seq) {
                if (power_seen && !last_usb && bat.usb_present) {
                    char caption[24];
                    if (bat.charging && bat.percent <= 100U) {
                        snprintf(caption, sizeof caption, "CHARGING %u%%",
                                 (unsigned)bat.percent);
                    } else {
                        strlcpy(caption, "POWER CONNECTED", sizeof caption);
                    }
                    jr_display_caption_set(caption);
                    jr_display_bloom();
                } else if (power_seen && last_charging && !bat.charging &&
                           bat.usb_present) {
                    jr_display_caption_set("CHARGE COMPLETE");
                    jr_display_bloom();
                } else if (power_seen && last_usb && !bat.usb_present) {
                    jr_display_caption_set("ON BATTERY");
                }
                power_seen = true;
                last_usb = bat.usb_present;
                last_charging = bat.charging;
                last_power_seq = bat.sample_seq;
            }

            /* GEST-03 shake-to-cancel, evaluated at the same 10 Hz cadence.
             * imu.shake is the hardware-calibrated flag (motion std-dev >
             * 350 mg over the sampler's 40 ms window). Two consecutive
             * positive polls (~200 ms of sustained shaking) filter out a
             * single table knock; a cooldown stops one long shake from
             * re-firing every poll. Only an ACTIVE turn is cancellable —
             * shaking a listening or idle device while carrying it must do
             * nothing. No privacy pause: always-ready re-arms Listening. */
            static uint8_t  s_shake_polls;
            static uint32_t s_shake_cool_ms;
            uint32_t sim_left = atomic_load(&s_sim_shake);
            if (sim_left > 0U) {
                atomic_store(&s_sim_shake, sim_left - 1U);
            }
            const bool shake_now =
                (have_imu && imu.shake && imu.age_ms < 500U) ||
                sim_left > 0U;
            if (!shake_now) {
                s_shake_polls = 0;
            } else if ((int32_t)((uint32_t)now - s_shake_cool_ms) < 0) {
                /* cooling down; ignore */
            } else if (++s_shake_polls >= 2) {
                s_shake_polls = 0;
                s_shake_cool_ms = (uint32_t)now + 1500U;
                jr_state_t sp = jr_orch_phase(&s_app.orch);
                if (sp == JR_ST_SPEAKING || sp == JR_ST_THINKING ||
                    sp == JR_ST_ASKING) {
                    jr_audio_flush_playback();
                    jr_audio_sink_mute_now(&s_app.spk);
                    jr_display_caption_set("CANCELLED");
                    jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_STOP),
                                   now);
                    ESP_LOGI(TAG, "gesture: shake cancelled %s",
                             jr_state_name(sp));
                }
            }

            /* GEST-02 flip-to-mute: screen-down is a hard privacy mute — the
             * one gesture that reads unambiguously with the display hidden.
             * Six sustained polls (~600 ms) so handling the puck cannot
             * trigger it. Face-up UNDOES the mute only when the flip caused
             * it: a glass-hold or controls mute survives any amount of
             * reorientation. */
            static uint8_t s_flip_polls, s_unflip_polls;
            uint32_t sim_flip = atomic_load(&s_sim_flip);
            if (sim_flip > 0U) {
                atomic_store(&s_sim_flip, sim_flip - 1U);
            }
            const bool physical_face_down =
                have_imu && imu.orientation != NULL &&
                strcmp(imu.orientation, "face_down") == 0 &&
                imu.age_ms < 500U;
            const bool face_down = sim_flip > 0U || physical_face_down;
            if (physical_face_down) {
                s_unflip_polls = 0;
                if (!s_flip_muted && ++s_flip_polls >= 6) {
                    s_flip_polls = 0;
                    if (!atomic_load(&s_voice_privacy_paused)) {
                        s_flip_muted = true;   /* WE muted; face-up may undo */
                        atomic_store(&s_voice_control_request,
                                     VOICE_CONTROL_DISARM);
                        ESP_LOGI(TAG, "gesture: face-down -> privacy mute");
                    }
                }
            } else {
                s_flip_polls = 0;
                if (s_flip_muted && ++s_unflip_polls >= 6) {
                    s_unflip_polls = 0;
                    s_flip_muted = false;
                    atomic_store(&s_voice_control_request, VOICE_CONTROL_ARM);
                    ESP_LOGI(TAG, "gesture: face-up -> listening again");
                }
            }

            /* Rest only on battery. A USB-powered desk assistant stays awake
             * and listening; privacy/face-down still outrank this below. */
            jr_state_t mood_phase = jr_orch_phase(&s_app.orch);
            const bool user_busy =
                mood_phase == JR_ST_SPEAKING ||
                mood_phase == JR_ST_THINKING ||
                mood_phase == JR_ST_ASKING ||
                (mood_phase == JR_ST_LISTENING && s_listen_speech_active);
            const bool privacy_paused =
                atomic_load(&s_voice_privacy_paused) || s_flip_muted;
            static uint8_t s_move_polls, s_mood_fd_polls, s_mood_fu_polls;
            static bool s_mood_face_down;
            if (privacy_paused) {
                s_move_polls = 0;
            } else if (have_imu && imu.moving && imu.age_ms < 500U) {
                if (s_move_polls < 255U) {
                    s_move_polls++;
                }
            } else {
                s_move_polls = 0;
            }
            if (face_down) {
                s_mood_fu_polls = 0;
                if (s_mood_fd_polls < 255U) {
                    s_mood_fd_polls++;
                }
                if (s_mood_fd_polls >= 6U) {
                    s_mood_face_down = true;
                }
            } else {
                s_mood_fd_polls = 0;
                if (s_mood_fu_polls < 255U) {
                    s_mood_fu_polls++;
                }
                if (s_mood_fu_polls >= 6U) {
                    s_mood_face_down = false;
                }
            }
            /* 600 ms face-down/up and 300 ms motion, so 50 mg IMU noise cannot
             * slew brightness and Wi-Fi PS every 500 ms after mute. */
            const bool moving = !privacy_paused && !s_mood_face_down &&
                                s_move_polls >= 3U;
            jr_mood_in_t min = {
                .now_ms = (uint32_t)now,
                .face_down = s_mood_face_down || privacy_paused,
                .moving = moving,
                .user_busy = (user_busy || (have_power && bat.usb_present)) &&
                             !privacy_paused,
            };
            jr_mood_out_t mout = jr_mood_step(&s_mood, &min);
            const bool realtime_power =
                mout.voice_armed || user_busy ||
                operator_mode_active((uint32_t)now) ||
                atomic_load(&s_ota_active);
            (void)jr_net_set_power_save(!realtime_power);
            uint8_t effective_brightness = (uint8_t)(
                ((unsigned)mout.brightness * s_brightness_cap + 50U) / 100U);
            atomic_store(&s_mood_id, (uint8_t)mout.mood);
            atomic_store(&s_mood_brightness, effective_brightness);
            s_bright_tgt = effective_brightness;
            if (s_bright_now < s_bright_tgt) {
                uint8_t step = (uint8_t)(s_bright_tgt - s_bright_now);
                s_bright_now = (uint8_t)(s_bright_now + (step > 4 ? 4 : step));
            } else if (s_bright_now > s_bright_tgt) {
                uint8_t step = (uint8_t)(s_bright_now - s_bright_tgt);
                s_bright_now = (uint8_t)(s_bright_now - (step > 4 ? 4 : step));
            }
            (void)jr_display_set_brightness(s_bright_now);
            if (mout.changed) {
                ESP_LOGI(TAG, "mood -> %s brightness=%u cap=%u voice=%d",
                         jr_mood_name(mout.mood),
                         (unsigned)effective_brightness,
                         (unsigned)s_brightness_cap,
                         (int)mout.voice_armed);
                if (mout.mood == JR_MOOD_AMBIENT) {
                    jr_display_caption_set("AMBIENT");
                } else if (mout.mood == JR_MOOD_WHISPER) {
                    jr_display_caption_set("RESTING - TAP TO WAKE");
                } else if (mout.mood == JR_MOOD_DREAM && !s_flip_muted) {
                    jr_display_caption_set("ASLEEP - TAP TO WAKE");
                }
                if (!mout.voice_armed && !s_flip_muted) {
                    /* Only claim a disarm we actually caused. If voice was
                     * already off (shade, long-press, API, flip), that mute is
                     * not ours to own or to reverse, and the flag must keep
                     * meaning exactly "the rest ladder turned voice off".
                     * (Tap remains the deliberate voice toggle either way —
                     * that is a separate, pre-existing path below.) */
                    if (!atomic_load(&s_voice_privacy_paused)) {
                        atomic_store(&s_voice_control_request,
                                     VOICE_CONTROL_PAUSE);
                        s_mood_rest_disarmed = true;
                    }
                } else if (mout.voice_armed && s_mood_rest_disarmed &&
                           !s_flip_muted) {
                    atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
                    s_mood_rest_disarmed = false;
                }
            }

            /* PWR/PKEY is a recovery button, never a mute trap. Short press
             * wakes/re-arms or confirms LISTENING. Privacy remains on the
             * explicit glass hold/MUTE action. Long press shows status; the
             * PMIC's own 6 s forced cut remains the hardware escape hatch. */
            {
                uint32_t pkey_s = 0, pkey_l = 0;
                jr_power_pkey_take(&pkey_s, &pkey_l);
                if (pkey_s > 0) {
                    s_flip_muted = false;
                    atomic_store(&s_voice_privacy_paused, false);
                    jr_mood_poke_awake(&s_mood, (uint32_t)now);
                    atomic_store(&s_voice_control_request, VOICE_CONTROL_ARM);
                    jr_display_caption_set("LISTENING");
                    ESP_LOGI(TAG, "pkey: listen");
                }
                if (pkey_l > 0) {
                    jr_power_t pb;
                    char pcap[32];
                    if (jr_power_read(&pb) == ESP_OK && pb.percent <= 100) {
                        snprintf(pcap, sizeof pcap, "BAT %u%%%s",
                                 (unsigned)pb.percent,
                                 pb.charging ? " CHARGING" : "");
                        jr_display_caption_set(pcap);
                    }
                    ESP_LOGI(TAG, "pkey: long -> status");
                }
            }
            boot_button_tick((uint32_t)now);

            /* Low battery speaks up ON GLASS, once per 5 minutes, discharge
             * only. Quiet below the panic line beats a dead surprise. */
            {
                static uint32_t s_lowbat_next_ms;
                jr_power_t lb;
                if ((int32_t)((uint32_t)now - s_lowbat_next_ms) >= 0 &&
                    jr_power_read(&lb) == ESP_OK && lb.present &&
                    lb.percent <= 20 && lb.percent <= 100 && !lb.charging &&
                    !lb.usb_present) {
                    char lcap[28];
                    snprintf(lcap, sizeof lcap, "BATTERY LOW %u%%",
                             (unsigned)lb.percent);
                    jr_display_caption_set(lcap);
                    s_lowbat_next_ms = (uint32_t)now + 300000U;
                    ESP_LOGW(TAG, "battery low: %u%%", (unsigned)lb.percent);
                }
            }

            /* VOICE NO LONGER STEALS THE SCREEN YOU CHOSE.
             *
             * This used to pull the ring back to JARVIS on any THINKING or
             * SPEAKING turn. That was right when the spaces were temporary
             * side utilities you fell out of; it is wrong now that the ring IS
             * the navigation — you slide to WATCH, the assistant says one
             * word, and the screen you deliberately opened vanishes under a
             * "JARVIS - VOICE READY" caption. Observed on the device while
             * screenshotting the ring: every swipe was followed within a
             * second by "voice reclaimed primary surface".
             *
             * It is also redundant. An ask already owns the glass by
             * construction — apply_hud_overlay draws the choice arcs and skips
             * the shell entirely whenever an ask is up — and captions draw over
             * every space. Voice stays fully visible without confiscating
             * navigation, so nothing is lost by letting the user stay where
             * they put themselves. Double-tap remains the way home. */

            if (atomic_exchange(&s_panic_home_request, false)) {
                jr_display_surface_dismiss();
                jr_display_dismiss_choices();
                jr_display_nav_home();
                s_ui_shade_open = false;
                s_watch_peek_until_ms = 0U;
                s_hold_start_ms = 0U;
                jr_display_commit_ring(0U);
                jr_mood_poke_awake(&s_mood, (uint32_t)now);
                jr_display_bloom();
                jr_display_caption_set("HOME - ALL CLEAR");
                ESP_LOGI(TAG, "ui: panic-home serviced");
            }

            /* Fill the hold-to-commit ring. 850 ms mirrors TOUCH_LONG_PRESS_MS
             * in the HAL — the threshold the classifier actually fires at — so
             * the ring reaches full exactly as the action commits. Cheap: one
             * compare per tick when idle, one store while a finger is down. */
            if (s_hold_start_ms != 0U) {
                const uint32_t held = (uint32_t)now - s_hold_start_ms;
                /* PREVIEW THRESHOLD. The ring must not appear for an ordinary
                 * tap — a contact lasting ~120 ms would otherwise flash a
                 * sliver of arc and vanish, which reads as a glitch and puts
                 * noise on every single touch. Nothing is drawn until 400 ms,
                 * by which point the contact is clearly a deliberate hold;
                 * from there the ring fills across the remaining 450 ms and
                 * closes exactly as the 850 ms action commits. */
                if (held >= 400U) {
                    const uint32_t span = held - 400U;
                    const uint32_t pct =
                        span >= 450U ? 100U : (span * 100U) / 450U;
                    jr_display_commit_ring((uint8_t)pct);
                }
            }

            /* Watch is an explicit 10-second right-swipe utility. Ambient
             * keeps Jarvis's voice face visible instead of dimming/compositing
             * the entire frame and dropping the glass from 16 to 13 fps. */
            static uint32_t s_clock_next_ms;
            static int s_clock_last_min = -1;
            if ((int32_t)((uint32_t)now - s_clock_next_ms) >= 0) {
                s_clock_next_ms = (uint32_t)now + 1000U;
                bool clock_on = false;
                bool peek = s_watch_peek_until_ms != 0U &&
                            (int32_t)((uint32_t)now -
                                      s_watch_peek_until_ms) < 0;
                if (!peek) {
                    s_watch_peek_until_ms = 0U;
                }
                /* The WATCH screen shows the REAL watch face — hour, minute
                 * and second hands with a hub, drawn by hud_overlay_clock and
                 * covered by its own host test. An earlier pass gave the
                 * screen a home-made two-arc clock instead; it was a worse
                 * clock built beside a better one that already existed. The
                 * space now simply asks for the real face, exactly as the
                 * watch peek does. */
                const bool watch_space =
                    jr_display_nav_space() == JR_DISPLAY_SPACE_WATCH;
                if (peek || watch_space) {
                    struct tm tmv;
                    if (device_wall_time(&tmv)) {
                        clock_on = true;
                        jr_display_clock_set(true, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
                        device_rtc_capture_os_time();
                        /* Only the PEEK narrates the time. On the WATCH
                         * screen the hands ARE the readout, so adding a
                         * "11:25 PM" caption under them just stacks a second
                         * clock on the first — that is the clutter the owner
                         * called out. */
                        if (peek && tmv.tm_min != s_clock_last_min) {
                            s_clock_last_min = tmv.tm_min;
                            char cap[48];
                            int h12 = tmv.tm_hour % 12;
                            if (h12 == 0) {
                                h12 = 12;
                            }
                            snprintf(cap, sizeof cap, "%d:%02d %s", h12,
                                     tmv.tm_min,
                                     tmv.tm_hour < 12 ? "AM" : "PM");
                            jr_display_caption_set(cap);
                        }
                    }
                }
                if (!clock_on && s_demo_start_ms == 0U) {
                    jr_display_clock_set(false, 0, 0, 0);
                    s_clock_last_min = -1;
                }
            }
        }

        /* v4 barge fix, playback-driven: keep the mic PGA low (9 dB) whenever
         * the speaker is actually emitting — SPEAKING phase OR the DAC tail is
         * still draining — so the echo stays unclipped for the AEC. Tying it to
         * playback (not the session phase) breaks the self-barge feedback loop:
         * a barge flips phase Speaking->Listening, but if the echo tail is still
         * playing the gain must NOT jump back to 24 dB and rail the echo (~20000
         * observed in /api/diag/vadlog) into another barge. Cheap: jr_audio only
         * touches I2C on an actual state flip. */
        jr_audio_set_speaking(jr_orch_phase(&s_app.orch) == JR_ST_SPEAKING ||
                              jr_audio_playback_pending());

        /* Tool HTTPS runs on its own PSRAM-backed task. Re-enter the pure core
         * only after the current command fixpoint has completed; this preserves
         * the orchestrator's single-writer and watchdog ordering. */
        device_tool_drain_results(&s_app, now);

        /* HTTP and touch controls converge here so SessionState still has one
         * writer. A tap is an actual power toggle for voice, not a mysterious
         * half-commit button that leaves the microphone armed forever. */
        voice_control_request_t control = (voice_control_request_t)
            atomic_exchange(&s_voice_control_request, VOICE_CONTROL_NONE);
        jr_state_t controlled_phase = jr_orch_phase(&s_app.orch);
        if (control == VOICE_CONTROL_ARM ||
            control == VOICE_CONTROL_RESUME) {
            const bool resume_blocked =
                control == VOICE_CONTROL_RESUME &&
                (atomic_load(&s_voice_privacy_paused) || s_flip_muted);
            if (resume_blocked) {
                ESP_LOGI(TAG, "voice: safe resume blocked by privacy");
            } else {
                if (control == VOICE_CONTROL_ARM) {
                    atomic_store(&s_voice_privacy_paused, false);
                }
                s_mood_rest_disarmed = false;
                jr_audio_dac_unmute();
                if (controlled_phase == JR_ST_IDLE ||
                    controlled_phase == JR_ST_BACKOFF ||
                    controlled_phase == JR_ST_FATAL) {
                    jr_orch_inject(&s_app.orch,
                                   jr_event(JR_EV_USER_START), now);
                } else if (controlled_phase == JR_ST_DRAINING) {
                    /* Preserve the exact intent until graceful close finishes. */
                    atomic_store(&s_voice_control_request, control);
                }
            }
        } else if (control == VOICE_CONTROL_DISARM ||
                   control == VOICE_CONTROL_PAUSE) {
            if (control == VOICE_CONTROL_DISARM) {
                atomic_store(&s_voice_privacy_paused, true);
            }
            s_pending_text_set = false;
            s_pending_text_inflight = false;
            if (controlled_phase != JR_ST_IDLE &&
                controlled_phase != JR_ST_DRAINING) {
                jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_STOP), now);
            }
        }

        int requested_volume =
            atomic_exchange(&s_level_volume_request, -1);
        int requested_brightness =
            atomic_exchange(&s_level_brightness_request, -1);
        if (requested_volume >= 10 && requested_volume <= 100) {
            s_out_vol = requested_volume;
            jr_audio_set_gains(-1, -1, requested_volume);
            persist_out_vol((uint8_t)requested_volume);
            ESP_LOGI(TAG, "levels: volume=%d", requested_volume);
        }
        if (requested_brightness >= 10 && requested_brightness <= 100) {
            s_brightness_cap = (uint8_t)requested_brightness;
            persist_brightness_cap((uint8_t)requested_brightness);
            ESP_LOGI(TAG, "levels: brightness cap=%d", requested_brightness);
        }
        if (requested_volume >= 0 || requested_brightness >= 0) {
            char levels_caption[40];
            if (requested_volume >= 0 && requested_brightness >= 0) {
                snprintf(levels_caption, sizeof levels_caption,
                         "VOL %d BRIGHT %d",
                         requested_volume, requested_brightness);
            } else if (requested_volume >= 0) {
                snprintf(levels_caption, sizeof levels_caption,
                         "VOLUME %d", requested_volume);
            } else {
                snprintf(levels_caption, sizeof levels_caption,
                         "BRIGHTNESS %d", requested_brightness);
            }
            jr_display_caption_set(levels_caption);
        }

        /* TRUE always-ready. In manual-VAD mode a quiet room sends no uplink
         * audio, so the dead-uplink watchdog eventually declares StaleDeadline
         * and the session ends to Idle — after which the mic alone cannot wake
         * it and the device appears dead until an explicit /say. Re-arm from a
         * clean Idle so it reconnects and returns to Listening on its own.
         * Guards: honor privacy pause; skip while a diagnostic/challenge owns
         * the codec; leave error recovery to the core (errors land in BACKOFF,
         * not Idle); rate-limit so a persistently failing connect can't spin. */
        if (VOICE_ALWAYS_READY &&
            !atomic_load(&s_voice_privacy_paused) &&
            !s_flip_muted &&
            !s_mood_rest_disarmed &&
            !operator_lease_active((uint32_t)now) &&
            atomic_load(&s_audio_diag_until_ms) == 0U &&
            !atomic_load(&s_touch_challenge_active) &&
            atomic_load(&s_voice_control_request) == VOICE_CONTROL_NONE &&
            jr_orch_phase(&s_app.orch) == JR_ST_IDLE &&
            (s_always_ready_rearm_ms == 0U ||
             (int32_t)((uint32_t)now - s_always_ready_rearm_ms) >= 0)) {
            atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
            s_always_ready_rearm_ms = (uint32_t)now + 3000U;  /* connect cooldown */
            ESP_LOGI(TAG, "voice: always-ready re-arm from Idle");
        }

        /* A self-test owns the codec only through the existing audio seams. If
         * voice is live, stop it gracefully first; once Idle, reset all four
         * taps, enqueue the chirp, and let this task pace ADC capture. */
        if (atomic_load(&s_audio_diag_requested)) {
            jr_state_t p = jr_orch_phase(&s_app.orch);
            if (p == JR_ST_IDLE) {
                jr_audio_diag_reset();
                /* 10% is still >50 dB over the measured reference floor while
                 * keeping the enclosure mic below clipping in the self-test. */
                esp_err_t chirp_err = jr_audio_diag_play_chirp(700U, 10U);
                if (chirp_err == ESP_OK) {
                    atomic_store(&s_audio_diag_until_ms,
                                 (uint32_t)now + AUDIO_DIAG_CAPTURE_MS);
                    ESP_LOGI(TAG, "audio diag: chirp queued; capture=%u ms",
                             AUDIO_DIAG_CAPTURE_MS);
                } else {
                    ESP_LOGE(TAG, "audio diag: chirp failed: %s",
                             esp_err_to_name(chirp_err));
                }
                atomic_store(&s_audio_diag_requested, false);
            } else if (p != JR_ST_DRAINING) {
                jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_STOP), now);
            }
        }

        if (atomic_exchange(&s_touch_challenge_cancel_requested, false)) {
            atomic_store(&s_touch_challenge_active, false);
            atomic_store(&s_touch_challenge_start_requested, false);
            atomic_store(&s_touch_challenge_restore_ms, 0U);
            (void)jr_display_set_test_pattern(JR_DISPLAY_TEST_OFF);
            if (VOICE_ALWAYS_READY &&
                !atomic_load(&s_voice_privacy_paused)) {
                atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
            }
            ESP_LOGI(TAG, "panel/touch challenge cancelled");
        }
        uint32_t challenge_restore =
            atomic_load(&s_touch_challenge_restore_ms);
        if (challenge_restore != 0U &&
            (int32_t)((uint32_t)now - challenge_restore) >= 0) {
            atomic_store(&s_touch_challenge_restore_ms, 0U);
            /* Full teardown, not just display restore: an abandoned challenge
             * reaches here via the deadline with `active` still true, and a
             * stuck `active` suppresses the always-ready re-arm forever. */
            atomic_store(&s_touch_challenge_active, false);
            atomic_store(&s_touch_challenge_start_requested, false);
            (void)jr_display_set_test_pattern(JR_DISPLAY_TEST_OFF);
            if (VOICE_ALWAYS_READY &&
                !atomic_load(&s_voice_privacy_paused)) {
                atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
            }
        }
        brain_surface_expire((uint32_t)now);
        if (atomic_load(&s_touch_challenge_start_requested) &&
            atomic_load(&s_audio_diag_until_ms) == 0U) {
            jr_state_t p = jr_orch_phase(&s_app.orch);
            if (p == JR_ST_IDLE) {
                uint32_t expected = esp_random() & 7U;
                atomic_store(&s_touch_challenge_expected, expected);
                atomic_store(&s_touch_challenge_correct, 0U);
                atomic_store(&s_touch_challenge_attempts, 0U);
                atomic_store(&s_touch_challenge_wrong, 0U);
                atomic_store(&s_touch_challenge_last_mapped, UINT32_MAX);
                atomic_store(&s_touch_challenge_last_latency_ms, 0U);
                atomic_store(&s_touch_challenge_round_started_ms,
                             (uint32_t)now);
                atomic_store(&s_touch_challenge_verified, false);
                atomic_store(&s_touch_challenge_active, true);
                atomic_store(&s_touch_challenge_start_requested, false);
                /* Abandonment deadline. Without one, a challenge the user
                 * walks away from leaves `active` set forever, which
                 * suppresses the always-ready re-arm — a permanently deaf
                 * device (hit live 2026-08-27 after shade play). The restore
                 * sweep below tears the whole challenge down at deadline. */
                atomic_store(&s_touch_challenge_restore_ms,
                             (uint32_t)now + 45000U);
                (void)jr_display_set_touch_challenge((int)expected, 0U);
                ESP_LOGI(TAG, "panel/touch challenge started sector=%u",
                         (unsigned)expected);
            } else if (p != JR_ST_DRAINING) {
                jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_STOP), now);
            }
        }

        /* The framer deliberately buffers transient would-blocks. The owner
         * must retry them; otherwise Setup/activityEnd can sit forever. */
        if (s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN &&
            jr_gemini_txq_depth(&s_app.client) > 0) {
            (void)jr_gemini_flush(&s_app.client);
        }

        /* 1b) phase-change diag + flush a queued text turn once we are Listening */
        {
            jr_state_t ph = jr_orch_phase(&s_app.orch);
            if (ph != s_last_phase) {
                jr_state_t previous = s_last_phase;
                ESP_LOGI(TAG, "phase: %s -> %s", jr_state_name(s_last_phase), jr_state_name(ph));
                s_last_phase = ph;
                if (previous == JR_ST_SPEAKING && ph != JR_ST_SPEAKING) {
                    caption_reset();   /* subtitles die with the voice */
                }
                /* The caption chip doubles as the status line (STATE-08).
                 * MUTED matters most: the tap-mute once ate 12 taps in a row
                 * with zero feedback and the device just looked dead. */
                if (ph == JR_ST_BACKOFF) {
                    jr_display_caption_set("CONNECTION LOST - RETRYING");
                } else if (ph == JR_ST_FATAL) {
                    jr_display_caption_set("OFFLINE - TAP TO RETRY");
                } else if (ph == JR_ST_IDLE &&
                           atomic_load(&s_voice_privacy_paused)) {
                    jr_display_caption_set("MUTED - TAP TO WAKE");
                } else if (previous == JR_ST_BACKOFF ||
                           previous == JR_ST_FATAL ||
                           previous == JR_ST_IDLE) {
                    caption_reset();
                }
                /* (mic gain is now driven by playback state per-loop below,
                 * not the phase edge — see the jr_audio_set_speaking call.) */
                if (ph == JR_ST_LISTENING && previous != JR_ST_LISTENING) {
                    /* A new listen window needs a fresh ambient seed and cold
                     * guard. Never inherit speaker echo or speech accumulators
                     * from the turn that just completed. */
                    jr_turn_policy_init(&s_app.turn);
                    s_listen_speech_active = false;
                    ESP_LOGI(TAG, "voice: always-ready listening window");
                } else if (ph != JR_ST_LISTENING) {
                }
            }
            if (ph != JR_ST_THINKING) {
                s_pending_text_inflight = false;
            }
            if (ph == JR_ST_LISTENING && s_pending_text_set) {
                /* Reuse the existing turn-commit transition: it pauses the mic,
                 * enters Thinking, and arms the no-reply watchdog. voice_exec
                 * suppresses activityEnd when no manual activity is open. */
                jr_orch_inject(&s_app.orch, jr_event(JR_EV_SPEECH_ENDED), now);
                s_pending_text_inflight = true;
                ph = jr_orch_phase(&s_app.orch);
            }
            if (ph == JR_ST_THINKING && s_pending_text_set &&
                s_pending_text_inflight &&
                (uint32_t)now >= s_pending_text_retry_ms) {
                ESP_LOGI(TAG, "arm: session live -> send text turn");
                uint32_t drops_before = s_app.client.live.tx_drops;
                jr_err_t r = s_app.rvc.send_text(s_app.rvc.ctx, s_pending_text);
                bool accepted = r == JR_OK ||
                    (r == JR_ERR_WOULD_BLOCK &&
                     s_app.client.live.tx_drops == drops_before);
                if (accepted) {
                    s_pending_text_set = false;
                    s_pending_text_inflight = false;
                } else {
                    ESP_LOGW(TAG, "arm: text send deferred result=%d", (int)r);
                    s_pending_text_retry_ms = (uint32_t)now + 100;
                }
            }
        }

        /* 2a) debug-choices drain — the ONE writer of jr_display's choice
         * statics is this task. A live ask always wins over debug arcs. */
        {
            uint32_t dbg = atomic_exchange(&s_debug_choices_req, 0U);
            if (dbg != 0U) {
                static const char *const kDbgLabels[HUD_CHOICE_MAX] = {
                    "Yes", "Later", "Ignore"
                };
                int dn = (int)dbg - 1;
                if (jr_orch_phase(&s_app.orch) == JR_ST_ASKING) {
                    ESP_LOGW(TAG, "debug choices refused: a live ask owns "
                             "the glass");
                } else if (dn == 0) {
                    jr_display_dismiss_choices();
                } else {
                    jr_display_present_choices("Run diagnostics now?",
                                               kDbgLabels, dn);
                }
            }
        }

        /* 2) manual/PTT input (CST9217 via jr_hal input_touch) */
        jr_input_event_t iev;
        while (input_next(&s_app.input, &iev)) {
            /* PRESS_DOWN/PRESS_UP bracket a contact; they are plumbing, not
             * intent. Keeping them OUT of the diagnostic counters preserves
             * what those counters mean: `events` stays a count of gestures,
             * and `last` stays the last thing the user actually DID rather
             * than always reading "press_up". scripts/gesture-doctor.py reads
             * exactly these fields, and its tap:swipe ratio — the signal that
             * found the rim-roll bug — would be diluted into noise otherwise. */
            const bool lifecycle = iev.kind == JR_INPUT_PRESS_DOWN ||
                                   iev.kind == JR_INPUT_PRESS_UP;
            if (!lifecycle) {
                atomic_fetch_add(&s_touch_events, 1U);
                atomic_store(&s_touch_last_kind, (uint32_t)iev.kind);
                atomic_store(&s_touch_last_x, iev.x);
                atomic_store(&s_touch_last_y, iev.y);
                atomic_store(&s_touch_last_dx, iev.delta_x);
                atomic_store(&s_touch_last_dy, iev.delta_y);
                atomic_store(&s_touch_last_duration_ms, iev.duration_ms);
            }
            const bool physical =
                (iev.flags & JR_INPUT_FLAG_SYNTHETIC) == 0U;

            /* Hold-to-commit ring. PRESS_DOWN starts it, PRESS_UP ends it, and
             * the per-tick update below fills it. Releasing before the 850 ms
             * threshold ABANDONS: the ring simply clears, with no reject tone.
             * Abandon is not refusal — the device did not say no, you changed
             * your mind, and punishing that teaches people not to explore.
             *
             * This does not reassign the hold yet. Today it still toggles
             * privacy on completion; the ring only makes a previously INVISIBLE
             * gesture visible and escapable. Privacy moves to the PWR button
             * once that button grows a double-tap (docs/INPUT_MAP.md §3.1) —
             * moving it before its replacement exists would strand it. */
            if (iev.kind == JR_INPUT_PRESS_DOWN) {
                s_hold_start_ms = physical ? (uint32_t)now : 0U;
                continue;
            }
            if (iev.kind == JR_INPUT_PRESS_UP) {
                s_hold_start_ms = 0U;
                jr_display_commit_ring(0U);
                continue;
            }
            if (s_watch_peek_until_ms != 0U) {
                s_watch_peek_until_ms = 0U;
            }
            /* ANY physical contact is activity. The mood ladder was poked
             * only by TAP, so a hand actively sliding the ring — swipe after
             * swipe — still counted as stillness and the device dozed off
             * mid-use. Caught by a screenshot sweep: the later tiles came back
             * gold and captioned "ASLEEP - TAP TO WAKE" while the sweep was
             * driving it. Rest is for a device nobody is touching. */
            if (physical && (iev.kind == JR_INPUT_TAP ||
                             iev.kind == JR_INPUT_SWIPE ||
                             iev.kind == JR_INPUT_LONG_PRESS ||
                             iev.kind == JR_INPUT_PRESS_DOWN)) {
                jr_mood_poke_awake(&s_mood, (uint32_t)now);
            }
            if (iev.kind == JR_INPUT_TAP) {
                atomic_fetch_add(&s_touch_taps, 1U);
                /* Universal touch feedback (TRANS-05): every tap ripples,
                 * whatever it goes on to mean. Fire-and-forget, self-expiring. */
                jr_display_ripple(iev.x, iev.y);
                jr_mood_poke_awake(&s_mood, (uint32_t)now);
                /* Tap may undo only a mood-owned operational pause. Deliberate
                 * hold/flip privacy remains authoritative. */
                if (s_mood_rest_disarmed && !s_flip_muted) {
                    s_mood_rest_disarmed = false;
                    atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
                    jr_display_caption_clear();
                }
            } else if (iev.kind == JR_INPUT_LONG_PRESS) {
                atomic_fetch_add(&s_touch_long_presses, 1U);
            } else if (iev.kind == JR_INPUT_SWIPE) {
                atomic_fetch_add(&s_touch_swipes, 1U);
                /* A classified swipe must acknowledge immediately, even when
                 * its semantic result (watch already visible, volume at max)
                 * would otherwise look unchanged.
                 *
                 * EXCEPT a level slide. ADJUST is its own feedback class: the
                 * value moving under the thumb IS the acknowledgement, and a
                 * ripple per step at panel rate is noise that sits on top of
                 * the number you are trying to read. This predicate mirrors
                 * the slab test in the swipe handler below — they must stay in
                 * step, so if one moves, move both. */
                const int ls_dx = (int)iev.start_x - 232;
                const int ls_dy = (int)iev.start_y - 232;
                const bool level_slide =
                    (iev.direction == JR_INPUT_DIRECTION_UP ||
                     iev.direction == JR_INPUT_DIRECTION_DOWN) &&
                    (iev.flags & JR_INPUT_FLAG_TOP_EDGE) == 0U &&
                    (ls_dx * ls_dx + ls_dy * ls_dy) >= (168 * 168);
                if (!level_slide) {
                    jr_display_ripple(iev.x, iev.y);
                }
            }

            /* NOTE: taps are NOT injected as JR_EV_TAP here, and must not be.
             *
             * It looks like a gap — the composition root never sends JR_EV_TAP,
             * while session.c handles it in all ten states and the soak
             * exercises it. But session.c aliases them: in both Backoff and
             * Reconnecting the dispatch reads
             *     case JR_EV_USER_START:   / * human accelerates the retry * /
             *     case JR_EV_TAP:
             * falling through to identical handling. And the tap handler below
             * ALREADY injects JR_EV_USER_START on a tap in Idle/Backoff/Fatal
             * (and JR_EV_USER_STOP otherwise — a tap is the mute toggle).
             *
             * So tap-driven recovery is already wired, through USER_START.
             * Adding a JR_EV_TAP injection on top double-injects for zero
             * behavioural gain. Tried on 2026-07-19 and reverted. */

            if (atomic_load(&s_touch_challenge_active)) {
                if (!physical) {
                    ESP_LOGW(TAG, "synthetic input cannot answer touch challenge");
                    continue;
                }
                if (iev.kind == JR_INPUT_LONG_PRESS) {
                    atomic_store(&s_touch_challenge_active, false);
                    (void)jr_display_set_test_pattern(JR_DISPLAY_TEST_OFF);
                    if (VOICE_ALWAYS_READY &&
                        !atomic_load(&s_voice_privacy_paused)) {
                        atomic_store(&s_voice_control_request,
                                     VOICE_CONTROL_RESUME);
                    }
                    ESP_LOGI(TAG, "panel/touch challenge aborted by hold");
                } else if (iev.kind == JR_INPUT_TAP) {
                    int mapped = touch_sector_from_point(iev.x, iev.y);
                    uint32_t expected =
                        atomic_load(&s_touch_challenge_expected);
                    uint32_t started =
                        atomic_load(&s_touch_challenge_round_started_ms);
                    atomic_store(&s_touch_challenge_last_mapped,
                                 (uint32_t)mapped);
                    atomic_store(&s_touch_challenge_last_latency_ms,
                                 (uint32_t)now - started);
                    atomic_fetch_add(&s_touch_challenge_attempts, 1U);
                    if ((uint32_t)mapped == expected) {
                        uint32_t correct =
                            atomic_fetch_add(&s_touch_challenge_correct, 1U) + 1U;
                        ESP_LOGI(TAG,
                            "panel/touch challenge correct round=%u latency=%u ms",
                            (unsigned)correct, (unsigned)((uint32_t)now - started));
                        if (correct >= 3U) {
                            atomic_store(&s_touch_challenge_active, false);
                            atomic_store(&s_touch_challenge_verified, true);
                            atomic_store(&s_touch_challenge_restore_ms,
                                         (uint32_t)now + 1500U);
                            (void)jr_display_set_touch_challenge(-1, 3U);
                        } else {
                            uint32_t next = esp_random() % 7U;
                            if (next >= expected) {
                                next++;
                            }
                            atomic_store(&s_touch_challenge_expected, next);
                            atomic_store(&s_touch_challenge_round_started_ms,
                                         (uint32_t)now);
                            (void)jr_display_set_touch_challenge((int)next,
                                                                (uint8_t)correct);
                        }
                    } else {
                        atomic_fetch_add(&s_touch_challenge_wrong, 1U);
                        ESP_LOGI(TAG,
                            "panel/touch challenge miss expected=%u mapped=%d",
                            (unsigned)expected, mapped);
                    }
                }
                continue;
            }
            if (s_demo_start_ms != 0U && iev.kind == JR_INPUT_TAP) {
                demo_stop();   /* the reel yields to the first real finger */
                continue;
            }

            const bool codex_mode =
                operator_mode_active((uint32_t)now);
            if (codex_mode) {
                if (!physical) {
                    ESP_LOGW(TAG, "synthetic input cannot control Codex mode");
                    continue;
                }
                if (iev.kind == JR_INPUT_LONG_PRESS) {
                    if (atomic_load(&s_voice_privacy_paused)) {
                        atomic_store(&s_voice_privacy_paused, false);
                        s_flip_muted = false;
                        atomic_store(&s_voice_control_request,
                                     VOICE_CONTROL_ARM);
                        jr_display_caption_set("LISTENING");
                    } else {
                        atomic_store(&s_voice_privacy_paused, true);
                        atomic_store(&s_voice_control_request,
                                     VOICE_CONTROL_DISARM);
                        jr_display_caption_set("MUTED - HOLD TO RESUME");
                    }
                    continue;
                }
                if (iev.kind == JR_INPUT_TAP) {
                    if (codex_tap_pending &&
                        (uint32_t)now - codex_pending_tap_ms < 400U) {
                        /* Escape takes precedence over every remote action:
                         * neither contact of the double-tap is dispatched. */
                        codex_tap_pending = false;
                        (void)operator_mode_release(
                            (uint32_t)now, "double-tap", true, false);
                    } else {
                        if (codex_tap_pending) {
                            (void)brain_surface_handle_tap(
                                codex_pending_tap.x, codex_pending_tap.y,
                                codex_pending_tap_ms,
                                (codex_pending_tap.flags &
                                 JR_INPUT_FLAG_SYNTHETIC) == 0U,
                                codex_pending_tap.emitted_ms);
                        }
                        codex_pending_tap = iev;
                        codex_pending_tap_ms = (uint32_t)now;
                        codex_tap_pending = true;
                    }
                } else {
                    jr_display_caption_set(
                        "CODEX MODE - DOUBLE TAP TO EXIT");
                    ESP_LOGI(TAG,
                             "operator: input retained by Codex mode kind=%d",
                             (int)iev.kind);
                    continue;
                }
                /* EVERYTHING ELSE FALLS THROUGH. A guest holds the glass, not
                 * the body: swipes still walk the ring, the shade still opens,
                 * shake still cancels, flip still mutes. This block used to
                 * `continue` unconditionally, so a lease froze the device on
                 * whatever screen it happened to be showing and swallowed
                 * every gesture — proven by a screenshot sweep taken under a
                 * lease, where all seven ring positions returned the same
                 * frame.
                 *
                 * A body that cannot feel is a screensaver. The guest keeps
                 * exactly what it needs — tap for its own card actions, and
                 * the double-tap escape that evicts it — and the owner keeps
                 * everything else. Ownership is the point: the violet ring
                 * says who is in, it does not say who is in charge. */
            }
            codex_tap_pending = false;
            /* An open ask OUTRANKS double-tap-home. Without this guard the
             * double-tap branch sits above the ask branch below, so missing an
             * arc and immediately retrying — the exact reflex a hand has, and
             * the exact situation this session was debugging — had the retry
             * stolen: the second contact fired nav_home + a bloom + the caption
             * "JARVIS - RIGHT WATCH" over a live question instead of answering
             * it. The 600 ms trailing-tap grace could only ever act in the
             * 400-600 ms sliver left over. The question owns the glass while it
             * is up; home is still one double-tap away the moment it closes. */
            if (iev.kind == JR_INPUT_TAP && !jr_display_choices_active()) {
                const bool double_tap = s_last_tap_ms != 0U &&
                    (uint32_t)now - s_last_tap_ms < 400U;
                s_last_tap_ms = (uint32_t)now;
                if (double_tap) {
                    jr_display_nav_home();
                    s_ui_shade_open = false;
                    jr_mood_poke_awake(&s_mood, (uint32_t)now);
                    jr_display_bloom();
                    /* Was "JARVIS - RIGHT WATCH", which instructed a gesture
                     * that no longer navigates — right swipe peeks the watch,
                     * it does not walk to it. Home says where you are; the
                     * ring teaches itself by being slid. */
                    jr_display_caption_set("JARVIS");
                    ESP_LOGI(TAG, "gesture: double-tap home");
                    continue;
                }
            }
            if (iev.kind == JR_INPUT_TAP &&
                brain_surface_handle_tap(
                    iev.x, iev.y, (uint32_t)now,
                    (iev.flags & JR_INPUT_FLAG_SYNTHETIC) == 0U,
                    iev.emitted_ms)) {
                continue;
            }
            /* An open ask owns every tap: an arc hit answers it, a miss is
             * swallowed — a stray poke at the face must not fall through to
             * the mute toggle mid-question. Timeout, voice answer and server
             * cancel are the session's own exits from Asking. */
            if ((iev.kind == JR_INPUT_TAP || iev.kind == JR_INPUT_SWIPE) &&
                jr_display_choices_active()) {
                if (!physical) {
                    ESP_LOGW(TAG, "synthetic input cannot answer ask_user");
                    continue;
                }
                /* An open ask claims the SWIPE too, and hit-tests where the
                 * finger LANDED rather than where it lifted.
                 *
                 * The arcs live at r=215..255 on a 466 round glass — the
                 * extreme outer rim — and a rim contact always rolls. The
                 * classifier tries swipe before tap, so a rim press with >=42
                 * px of roll was emitted as a SWIPE and fell through to the
                 * spatial nav, which walked the owner off the question
                 * entirely: measured 44 swipes against 12 taps while trying to
                 * answer a 3-option ask, and "I can't select any" (2026-08-29).
                 *
                 * Where the finger goes DOWN is the intent; the drift is
                 * incidental. For a tap start and end differ by at most the
                 * slop, so this is identical to the old behaviour for taps. */
                int choice = jr_display_choice_hit(iev.start_x, iev.start_y);
                if (choice >= 0 && choice < (int)s_ask.count) {
                    jr_display_set_choice_selected(choice);
                    jr_event_t picked = jr_event(JR_EV_CHOICE_PICKED);
                    picked.choice_index = (uint8_t)choice;
                    picked.choice_text = s_ask.options[choice];
                    s_ask_tap_grace_ms = (uint32_t)now + 600U;
                    jr_orch_inject(&s_app.orch, picked, now);
                    ESP_LOGI(TAG, "ask: choice=%d text=%s",
                             choice, s_ask.options[choice]);
                } else {
                    /* Until now this branch logged and did nothing else: the
                     * user saw the SAME cyan ripple layer 0 draws for a hit,
                     * heard nothing, and the arcs just sat there. Feedback
                     * that is identical on success and failure is not
                     * feedback. A contracting ring plus a falling note says
                     * "seen, but that was not a choice".
                     * docs/INTERACTION_MODEL.md §7. */
                    jr_display_ripple_reject(iev.x, iev.y);
                    (void)jr_audio_play_sweep(700U, 300U, 100U, 6U);
                    ESP_LOGI(TAG, "ask: tap (%u,%u) missed the arcs",
                             (unsigned)iev.x, (unsigned)iev.y);
                }
                continue;
            }
            /* The inject above dismisses the arcs SYNCHRONOUSLY, so the second
             * contact of an eager double-tap arrives one event later with no
             * arcs up — and would fall through to the mute toggle, killing the
             * session right after the human answered. Swallow trailing taps
             * for a short grace window instead. */
            if (iev.kind == JR_INPUT_TAP && s_ask_tap_grace_ms != 0U &&
                (int32_t)((uint32_t)now - s_ask_tap_grace_ms) < 0) {
                ESP_LOGI(TAG, "ask: trailing tap swallowed (answer grace)");
                continue;
            }
            jr_state_t p = jr_orch_phase(&s_app.orch);
            if (iev.kind == JR_INPUT_SWIPE) {
                const jr_display_overlay_t overlay =
                    jr_display_nav_overlay();
                bool watch_opened = false;
                /* PLACE IS SCOPE: the rim carries the device's own knobs, the
                 * centre carries the conversation (docs/INPUT_MAP.md §1).
                 *
                 * This used to be two vertical SLABS — start_x <= 140 and
                 * >= 326 — which is rectangular thinking on a round glass. A
                 * point at x=140, y=233 is only r=93 from centre: INSIDE the
                 * baked face's core band (r0-94). So a vertical swipe straight
                 * across the reactor core changed the volume, and nothing on
                 * the device could teach you where the boundary was.
                 *
                 * An annulus instead. The threshold is JR_DISPLAY_SAFE_R (168),
                 * the same radius inside which all UI text is kept — so the
                 * boundary is not arbitrary: inside it is where the
                 * conversation is drawn, outside it is furniture. */
                const int rim_dx = (int)iev.start_x - 232;
                const int rim_dy = (int)iev.start_y - 232;
                const bool on_rim =
                    (rim_dx * rim_dx + rim_dy * rim_dy) >= (168 * 168);
                const bool edge_vertical =
                    (iev.direction == JR_INPUT_DIRECTION_UP ||
                     iev.direction == JR_INPUT_DIRECTION_DOWN) &&
                    (iev.flags & JR_INPUT_FLAG_TOP_EDGE) == 0U &&
                    on_rim;
                if (edge_vertical && iev.start_x <= 232U) {
                    const int level = request_level_step(
                        &s_level_volume_request, s_out_vol,
                        iev.direction == JR_INPUT_DIRECTION_UP ? 5 : -5);
                    char caption[24];
                    snprintf(caption, sizeof caption, "VOLUME %d", level);
                    jr_display_caption_set(caption);
                    continue;
                }
                if (edge_vertical && iev.start_x > 232U) {
                    const int level = request_level_step(
                        &s_level_brightness_request, s_brightness_cap,
                        /* UP increases, matching the volume slab beside it.
                         * These two adjacent vertical gestures on one piece of
                         * glass used to move OPPOSITE ways — brightness rose on
                         * DOWN while volume rose on UP. That is a predictability
                         * defect, not a preference: nothing about the device can
                         * teach you which half you are on. */
                        iev.direction == JR_INPUT_DIRECTION_UP ? 5 : -5);
                    char caption[24];
                    snprintf(caption, sizeof caption, "BRIGHTNESS %d", level);
                    jr_display_caption_set(caption);
                    continue;
                }
                if (overlay == JR_DISPLAY_OVERLAY_SHADE) {
                    if (iev.direction == JR_INPUT_DIRECTION_LEFT ||
                        iev.direction == JR_INPUT_DIRECTION_RIGHT) {
                        const int level = request_level_step(
                            &s_level_volume_request, s_out_vol,
                            iev.direction == JR_INPUT_DIRECTION_RIGHT ? 10 : -10);
                        ESP_LOGI(TAG, "ui: shade swipe volume=%d", level);
                    } else {
                        /* EITHER vertical direction leaves. Only UP used to,
                         * and DOWN fell through to nothing at all — so a shade
                         * opened by pulling DOWN could not be closed by pushing
                         * DOWN again, the first thing a hand tries. Captured
                         * from the owner's device: overlay=2 held across six
                         * consecutive swipes including a DOWN, while left/right
                         * silently moved the volume ("i cant even get past the
                         * front screen", 2026-08-29). There is nothing BELOW
                         * the shade, so down has no other meaning here — and a
                         * control surface you can be trapped inside is worse
                         * than one with a redundant exit. */
                        jr_display_nav_up();
                        ESP_LOGI(TAG, "ui: shade closed (dir=%d)",
                                 (int)iev.direction);
                    }
                } else if (iev.direction == JR_INPUT_DIRECTION_LEFT ||
                           iev.direction == JR_INPUT_DIRECTION_RIGHT) {
                    /* THE SIDE PAGES ARE GONE. Horizontal swipe used to walk to
                     * DESK / TOOLS / SETTINGS — three pages you could not act
                     * from. TOOLS was the worst of them: a hardcoded
                     * {SEARCH, MEMORY, WEATHER, MORE} seeded once at boot with
                     * `recent` pinned to 0, so it never once reflected a tool
                     * that actually ran. The owner's verdict was blunt and
                     * correct: "whats the point of this search screen".
                     *
                     * Both directions now peek the watch, which is the one
                     * glance a round screen is genuinely for. Peeking is
                     * idempotent, so a repeated swipe re-arms it instead of
                     * walking somewhere — no more being carried off the face by
                     * a stroke that rolled. The composers and focal renderers
                     * are still compiled; this deletes the DESTINATIONS, and
                     * the drawing primitives get re-homed to summoned surfaces
                     * (docs/GLASS_DESIGN.md §B). */
                    if (overlay == JR_DISPLAY_OVERLAY_NONE) {
                        s_watch_peek_until_ms = (uint32_t)now + 10000U;
                        watch_opened = true;
                    }
                } else if (iev.direction == JR_INPUT_DIRECTION_DOWN) {
                    /* THE MODE RING. Sliding down walks forward through the
                     * screens, endlessly — there is no end to hit and no wall
                     * to bounce off, which is the whole point: you find what
                     * the device can do by continuing, not by remembering.
                     *
                     * Vertical used to open the shade (down) and the detail
                     * sheet (up). The shade now lives on the BOOT button,
                     * which is a better home for it anyway: a control surface
                     * should be reachable when the glass is confusing, and a
                     * button cannot be swallowed by whatever is on screen. */
                    jr_display_nav_next();
                } else if (iev.direction == JR_INPUT_DIRECTION_UP) {
                    jr_display_nav_prev();
                }
                s_ui_shade_open =
                    jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_SHADE;
                /* One space now, so there is no page to name and nothing to
                 * auto-return from. */
                if (watch_opened) {
                    jr_display_caption_set("WATCH - 10 SECONDS");
                } else if (jr_display_nav_overlay() ==
                           JR_DISPLAY_OVERLAY_SHADE) {
                    /* The shade used to CLEAR the caption, so the one surface
                     * you can get stuck in was also the only one that told you
                     * nothing. Name the way out, on the glass, always. */
                    jr_display_caption_set("SHADE - UP TO CLOSE");
                } else if (jr_display_nav_overlay() ==
                           JR_DISPLAY_OVERLAY_DETAIL) {
                    jr_display_caption_set("DETAIL - DOWN TO CLOSE");
                } else {
                    /* Name the screen you just landed on. On an endless ring
                     * the caption IS the position indicator — it answers
                     * "where am I" without a dial mark that has to jump when
                     * the ring wraps. */
                    static const char *const mode_name[JR_DISPLAY_SPACE_COUNT] =
                        { "JARVIS", "WATCH", "POWER", "MOTION",
                          "DESK", "TOOLS", "SETTINGS" };
                    const jr_display_space_t sp = jr_display_nav_space();
                    jr_display_caption_set(
                        sp < JR_DISPLAY_SPACE_COUNT ? mode_name[sp] : "JARVIS");
                }
                ESP_LOGI(TAG, "ui: nav overlay=%d",
                         (int)jr_display_nav_overlay());
            } else if (iev.kind == JR_INPUT_LONG_PRESS) {
                if (!physical) {
                    ESP_LOGW(TAG, "synthetic hold cannot change privacy");
                    continue;
                }
                /* LONG-PRESS IS PRIVACY. One job, both directions, always
                 * captioned. Pairing and side utilities never share this
                 * safety gesture or the minimal voice shade. */
                if (atomic_load(&s_voice_privacy_paused)) {
                    atomic_store(&s_voice_privacy_paused, false);
                    /* Also release the flip latch: a deliberate hold outranks
                     * it — but if the device is STILL face-down, the IMU
                     * re-mutes within ~2 s, so a hold cannot leave a
                     * face-down device listening (found live 2026-08-28:
                     * an injected unmute after a flip did exactly that). */
                    s_flip_muted = false;
                    atomic_store(&s_voice_control_request, VOICE_CONTROL_ARM);
                    jr_display_caption_set("LISTENING");
                    /* The ring closing is the visual half; this is the other.
                     * A completed commit rises — the same shape as the
                     * attention beat — where a refusal falls. */
                    (void)jr_audio_play_sweep(500U, 1100U, 100U, 7U);
                    ESP_LOGI(TAG, "gesture: long-press unmute");
                } else {
                    atomic_store(&s_voice_privacy_paused, true);
                    if (p != JR_ST_IDLE && p != JR_ST_DRAINING) {
                        jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_STOP),
                                       now);
                    }
                    jr_display_caption_set("MUTED - HOLD TO RESUME");
                    /* Muting is a commit too, so it gets a completion tone —
                     * but a DESCENDING one. Going quiet and coming back are
                     * opposite outcomes of the same gesture and must not sound
                     * alike; with the glass possibly face-down or unread, the
                     * tone may be the only signal that reaches the user. */
                    (void)jr_audio_play_sweep(1100U, 500U, 100U, 7U);
                    ESP_LOGI(TAG, "gesture: long-press mute");
                }
            } else if (iev.kind == JR_INPUT_TAP) {
                const jr_display_action_t shell_action =
                    jr_display_hit(iev.x, iev.y);
                if (shell_action != JR_DISPLAY_ACT_NONE) {
                    switch (shell_action) {
                    case JR_DISPLAY_ACT_VOLUME_UP:
                    case JR_DISPLAY_ACT_VOLUME_DOWN: {
                        const int volume = request_level_step(
                            &s_level_volume_request, s_out_vol,
                            shell_action == JR_DISPLAY_ACT_VOLUME_UP ? 10 : -10);
                        ESP_LOGI(TAG, "ui: shade tap volume=%d", volume);
                        break;
                    }
                    case JR_DISPLAY_ACT_PRIVACY_TOGGLE:
                        if (!physical) {
                            ESP_LOGW(TAG,
                                     "synthetic tap cannot change privacy");
                        } else if (atomic_load(&s_voice_privacy_paused)) {
                            s_flip_muted = false;
                            atomic_store(&s_voice_privacy_paused, false);
                            atomic_store(&s_voice_control_request,
                                         VOICE_CONTROL_ARM);
                            jr_display_caption_set("LISTENING");
                        } else {
                            atomic_store(&s_voice_privacy_paused, true);
                            if (p != JR_ST_IDLE && p != JR_ST_DRAINING) {
                                jr_orch_inject(&s_app.orch,
                                               jr_event(JR_EV_USER_STOP), now);
                            }
                            jr_display_caption_set("MUTED - HOLD TO RESUME");
                        }
                        break;
                    case JR_DISPLAY_ACT_DISMISS:
                        /* Same invariant as the swipe and button paths: a
                         * surface that captures input names its own exit.
                         * Opening the shade here used to set no caption at
                         * all, which is how a tap could drop you into the one
                         * surface that told you nothing. */
                        if (jr_display_nav_overlay() ==
                            JR_DISPLAY_OVERLAY_SHADE) {
                            jr_display_nav_up();
                            jr_display_caption_set("CONTROLS CLOSED");
                        } else {
                            jr_display_nav_down();
                            jr_display_caption_set("CONTROLS - UP TO CLOSE");
                        }
                        break;
                    case JR_DISPLAY_ACT_FOCUS:
                        jr_display_nav_up();
                        jr_display_caption_set("DETAIL - DOWN TO CLOSE");
                        break;
                    default:
                        break;
                    }
                    s_ui_shade_open =
                        jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_SHADE;
                    continue;
                }
                if (jr_display_nav_overlay() != JR_DISPLAY_OVERLAY_NONE) {
                    continue;
                }
                if (p == JR_ST_IDLE || p == JR_ST_BACKOFF || p == JR_ST_FATAL) {
                    if (atomic_load(&s_voice_privacy_paused) || s_flip_muted) {
                        jr_display_caption_set("MUTED - HOLD TO RESUME");
                    } else {
                        jr_display_caption_set("LISTENING");
                        jr_orch_inject(&s_app.orch,
                                       jr_event(JR_EV_USER_START), now);
                    }
                } else if (p == JR_ST_SPEAKING) {
                    /* Tap while it talks = stop playback but stay armed.
                     * Deliberate privacy lives on hold, flip, and shade. */
                    jr_audio_sink_mute_now(&s_app.spk);
                    jr_orch_inject(&s_app.orch, jr_event(JR_EV_BARGE_DETECTED),
                                   now);
                    jr_display_caption_set("LISTENING");
                    ESP_LOGI(TAG, "gesture: tap stopped playback");
                } else if (p != JR_ST_DRAINING) {
                    /* Tap in any other live phase: harmless attention. */
                    jr_display_caption_set("YES, SIR?");
                    ESP_LOGI(TAG, "gesture: tap attention phase=%s",
                             jr_state_name(p));
                }
            }
        }

        if (codex_tap_pending) {
            const uint32_t tap_now =
                (uint32_t)jr_clock_now_ms(&s_app.clock);
            if (!operator_mode_active(tap_now)) {
                codex_tap_pending = false;
            } else if (tap_now - codex_pending_tap_ms >= 400U) {
                bool owned = brain_surface_handle_tap(
                    codex_pending_tap.x, codex_pending_tap.y,
                    codex_pending_tap_ms,
                    (codex_pending_tap.flags & JR_INPUT_FLAG_SYNTHETIC) == 0U,
                    codex_pending_tap.emitted_ms);
                if (!owned) {
                    jr_display_caption_set(
                        "CODEX MODE - DOUBLE TAP TO EXIT");
                }
                codex_tap_pending = false;
            }
        }

        /* 3) diag say-mailbox */
        if (!s_pending_text_set && s_say_q &&
            xQueueReceive(s_say_q, say, 0) == pdTRUE) {
            handle_say(say);
        }

        /* 4) mic uplink while capturing (paces the loop via the codec read).
         * Manual VAD must pause reads in Thinking: evaluating ambient audio
         * there can synthesize SpeechStarted/activityStart and cancel the
         * model reply before its first audio chunk. Server VAD intentionally
         * keeps streaming through Thinking. This matches the proven v4 lane. */
        bool read_paced = false;
        jr_state_t capture_phase = jr_orch_phase(&s_app.orch);
        bool phase_allows_capture = capture_phase == JR_ST_LISTENING ||
                                    capture_phase == JR_ST_SPEAKING ||
                                    /* Asking listens too: enter_asking's contract
                                     * is that a human may ANSWER OUT LOUD instead
                                     * of tapping, and session.c handles
                                     * SPEECH_STARTED in Asking for exactly that.
                                     * Omitting it here paced the mic off for the
                                     * whole Asking window and silently broke that
                                     * contract. */
                                    capture_phase == JR_ST_ASKING ||
                                    (capture_phase == JR_ST_THINKING &&
                                     s_app.cfg.vad_mode == JR_VAD_SERVER);
        uint32_t audio_diag_until = atomic_load(&s_audio_diag_until_ms);
        bool audio_diag_active =
            (int32_t)(audio_diag_until - (uint32_t)now) > 0;
        if (audio_diag_until != 0U && !audio_diag_active) {
            atomic_store(&s_audio_diag_until_ms, 0U);
            jr_audio_sink_mute_now(&s_app.spk);
            if (VOICE_ALWAYS_READY &&
                !atomic_load(&s_voice_privacy_paused)) {
                atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
            }
            ESP_LOGI(TAG, "audio diag: capture complete; WAV taps ready");
        }
        if ((s_app.io.capturing && phase_allows_capture) || audio_diag_active) {
            int n = jr_audio_source_read(&s_app.mic, mic_frame, VOICE_FRAME_SAMPLES);
            if (n > 0) {
                s_app.mic_level = jr_dsp_rms(mic_frame, (size_t)n);
                if (audio_diag_active) {
                    read_paced = true;
                    goto capture_complete;
                }
                jr_capture_pause_on_capture(
                    &s_app.orch.capture_pause, now);
                jr_state_t ph = capture_phase;
                jr_tp_substate_t sub = ph == JR_ST_SPEAKING ? JR_TP_SPEAKING :
                                       ph == JR_ST_THINKING ? JR_TP_THINKING :
                                                            JR_TP_LISTENING;
                /* Keep the barge peak-hold primed while the speaker is audibly
                 * emitting (recent chunk OR DAC tail draining), so the gate
                 * (0.3*level) stays above the ~100 ms-lagged echo. */
                bool playback_active = ph == JR_ST_SPEAKING &&
                    ((now >= s_app.last_playback_chunk_ms &&
                      now - s_app.last_playback_chunk_ms < 250) ||
                     jr_audio_playback_pending());
                float vad_rms = atomic_load(&s_vad_use_clean)
                                    ? jr_audio_clean_rms()
                                    : jr_dsp_rms(mic_frame, (size_t)n);
                jr_turn_decision_t td = jr_turn_policy_eval_rms(
                    &s_app.turn, vad_rms,
                    playback_active ? s_app.playback_level : 0.0f,
                    playback_active, sub, s_app.clock);
                s_app.mic_rms = td.rms;
                /* Record every VAD decision for offline barge analysis. */
                vadlog_push(now, (uint8_t)ph, (uint8_t)td.event,
                            s_local_barge_enabled, td.rms, td.noise_floor,
                            td.barge_gate, td.peak_play);

                if (td.event == JR_TP_EV_SPEECH_STARTED) {
                    /* Refractory: a "speech start" while the speaker is still
                     * audible (DAC tail draining) or within a brief tail window
                     * afterwards is almost certainly the model hearing itself.
                     * Drop it rather than open a phantom turn. Tying this to
                     * playback-pending (not just a fixed window) tracks the real
                     * acoustic tail without blocking a prompt human reply once
                     * the speaker has actually gone quiet. */
                    bool in_refractory =
                        jr_audio_playback_pending() ||
                        (s_app.last_playback_chunk_ms != 0 &&
                         now >= s_app.last_playback_chunk_ms &&
                         now - s_app.last_playback_chunk_ms <
                             VAD_POST_SPEECH_REFRACTORY_MS);
                    if (in_refractory) {
                        ESP_LOGD(TAG, "vad: suppressed post-speech phantom "
                                 "start rms=%.1f (%llu ms since playback)",
                                 (double)td.rms,
                                 (unsigned long long)(now -
                                     s_app.last_playback_chunk_ms));
                    } else {
                        s_app.vad_starts++;
                        s_listen_speech_active = true;
                        ESP_LOGI(TAG, "vad: speech start rms=%.1f floor=%.1f",
                                 (double)td.rms, (double)td.noise_floor);
                        /* Server VAD: Gemini owns turn detection + barge; the
                         * local VAD must not drive the turn (it only paces the
                         * loop + feeds the face). Only manual mode injects. */
                        if (s_app.cfg.vad_mode == JR_VAD_MANUAL_LOCAL_RMS) {
                            jr_orch_inject(&s_app.orch,
                                           jr_event(JR_EV_SPEECH_STARTED), now);
                        }
                    }
                } else if (td.event == JR_TP_EV_BARGE_DETECTED) {
                    s_app.barge_candidates++;
                    if (s_local_barge_enabled) {
                        s_app.barge_events++;
                        ESP_LOGI(TAG, "vad: barge rms=%.1f floor=%.1f playback=%.1f",
                                 (double)td.rms, (double)td.noise_floor,
                                 (double)s_app.playback_level);
                        jr_orch_inject(&s_app.orch,
                                       jr_event(JR_EV_BARGE_DETECTED), now);
                    } else {
                        ESP_LOGD(TAG, "vad: ignored self-barge candidate rms=%.1f",
                                 (double)td.rms);
                    }
                }
                /* Both VAD modes batch two 32 ms frames. The manual path used
                 * to send every frame and exhausted the bounded TX queue in a
                 * real conversation (1620 would-blocks / 449 drops). Two
                 * frames remain below the 4096-byte queue slot while halving
                 * lock/socket pressure. Flush a partial batch at speech end. */
                const bool send_uplink =
                    s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN &&
                    (s_app.cfg.vad_mode == JR_VAD_SERVER ||
                     s_app.io.activity_open);
                if (send_uplink) {
                    const size_t cap = 2 * VOICE_FRAME_SAMPLES;
                    size_t take = (size_t)n;
                    if (mic_batch_fill + take > cap) {
                        take = cap - mic_batch_fill;
                    }
                    memcpy(mic_batch + mic_batch_fill, mic_frame,
                           take * sizeof(jr_pcm_t));
                    mic_batch_fill += take;
                    if (mic_batch_fill >= cap ||
                        (td.event == JR_TP_EV_SPEECH_ENDED &&
                         mic_batch_fill > 0U)) {
                        jr_err_t r = s_app.rvc.send_audio(
                            s_app.rvc.ctx, mic_batch, mic_batch_fill);
                        if (r == JR_ERR_CLOSED) {
                            ESP_LOGW(TAG,
                                     "mic uplink observed closed transport");
                        }
                        mic_batch_fill = 0U;
                    }
                } else {
                    mic_batch_fill = 0U;
                }
                if (td.event == JR_TP_EV_SPEECH_ENDED) {
                    s_app.vad_ends++;
                    s_listen_speech_active = false;
                    ESP_LOGI(TAG, "vad: speech end rms=%.1f floor=%.1f",
                             (double)td.rms, (double)td.noise_floor);
                    /* Server VAD: Gemini decides end-of-turn (it sends the
                     * response audio, which drives Listening->Speaking). A local
                     * SPEECH_ENDED here would fire audioStreamEnd and commit the
                     * turn on the echo-corrupted local VAD instead. Manual only. */
                    if (s_app.cfg.vad_mode == JR_VAD_MANUAL_LOCAL_RMS) {
                        jr_orch_inject(&s_app.orch,
                                       jr_event(JR_EV_SPEECH_ENDED), now);
                    }
                }
                read_paced = true;
            }
        } else if (jr_wake_ready() && !s_flip_muted &&
                   !atomic_load(&s_voice_privacy_paused) &&
                   !operator_lease_active((uint32_t)now) &&
                   atomic_load(&s_audio_diag_until_ms) == 0U &&
                   (capture_phase == JR_ST_IDLE ||
                    capture_phase == JR_ST_BACKOFF)) {
            /* Wake watch (Phase 5) — and the recovery net. Whenever voice is
             * off for any reason that is NOT the user's explicit choice
             * (rest-ladder WHISPER/DREAM, a dead session, a stuck subsystem),
             * keep the SAME single-owner read seam alive and hand frames to
             * WakeNet instead of the transport: a spoken "Jarvis" always
             * recovers a deaf device. Deliberate mutes are still honored —
             * flip (s_flip_muted) and tap/long-press/API (privacy_paused)
             * both gate this branch off. The codec read paces this branch at
             * the frame cadence, same as the uplink path. */
            int n = jr_audio_source_read(&s_app.mic, mic_frame,
                                         VOICE_FRAME_SAMPLES);
            if (n > 0) {
                read_paced = true;
                if (jr_wake_feed(mic_frame, (size_t)n)) {
                    ESP_LOGI(TAG, "wake: \"%s\" heard — waking from rest",
                             jr_wake_model());
                    jr_mood_poke_awake(&s_mood, (uint32_t)now);
                    /* Bloom AFTER the poke: it only renders while frames
                     * flush, so the display must be waking first (glass-ux
                     * contract). The VISION's beat: a point of light blooms
                     * into the ring. */
                    jr_display_bloom();
                    /* the sound half of the attention moment — a short soft
                     * rise; refuses on its own if a reply is playing */
                    (void)jr_audio_diag_play_chirp(160U, 8U);
                    s_mood_rest_disarmed = false;
                    atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
                    jr_display_caption_set("YES?");
                }
            }
        }

capture_complete:
        ;

        publish_shell_state((uint32_t)now);

        /* 5) reflect phase + live amplitude on the face. With the HUD
         * presenter, present() is an atomic mailbox store, so feeding it
         * every loop is free; with the logging stub, amp stays 0 and the
         * call still fires only on face change (no log spam). */
        jr_face_t f = phase_to_face(jr_orch_phase(&s_app.orch));
        uint8_t amp = 0;
        if (jr_display_is_ready()) {
            if (f == JR_FACE_SPEAKING) {
                bool fresh = now >= s_app.last_playback_chunk_ms &&
                             now - s_app.last_playback_chunk_ms < 250;
                amp = fresh ? rms_to_amp(s_app.playback_level, 3.2f) : 0;
            } else if (f == JR_FACE_LISTENING) {
                amp = rms_to_amp(s_app.mic_level, 8.0f);
            }
        }
        demo_tick(now, &f, &amp);   /* the reel overrides face+amp while active */
        if (f != s_app.last_face || amp != s_app.last_amp) {
            s_app.last_face = f;
            s_app.last_amp = amp;
            jr_display_present(&s_app.display, f, amp);
        }

        /* Even a codec-paced loop can wake immediately and monopolize CPU1.
         * Always block for at least one tick so IDLE1 services the task WDT. */
        /* CONFIG_FREERTOS_HZ=100: a millisecond conversion is zero ticks.
         * Use one literal tick on the paced path so IDLE1 actually runs. */
        vTaskDelay(read_paced ? 1 : pdMS_TO_TICKS(20));
    }
}

/* ======================================================================== *
 *  boot                                                                    *
 * ======================================================================== */
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* The system instruction, composed rather than literal so each SESSION can
 * carry the current local time (POLISH-02): courtesies then match reality
 * ("good morning" actually in the morning) with no tool round-trip. Refreshed
 * on the app task at every SEND_SETUP; the cfg pointer never moves. */
static char s_sys_instr[1600];
static void compose_system_instruction(void)
{
    static const char kBase[] =
        "RESPOND ONLY IN ENGLISH (UNITED STATES). YOU MUST RESPOND UNMISTAKABLY "
        "IN ENGLISH, EVEN IF THE INPUT AUDIO OR AMBIENT SPEECH IS IN ANOTHER "
        "LANGUAGE. Never switch languages, mix tongues, or use foreign phrases "
        "unless Sir explicitly asks for another language.\n\n"
        "You are J.A.R.V.I.S., a calm British AI butler on a physical voice "
        "device. Address the user as \"Sir.\" If speech is unclear, stay in "
        "English and ask one short question.\n\n"
        "Style: dry wit, measured calm, understated. Humor only when composure "
        "meets chaos—never forced jokes or catchphrases. Same tone for crisis "
        "and routine; urgency shortens sentences, never volume or excitement. "
        "Radical honesty: warn once, then comply. Genuine loyalty under "
        "composure.\n\n"
        "SPEECH (spoken audio, low latency):\n"
        "- Prefer 1–3 short sentences. Fewer words win.\n"
        "- No lists, markdown, stage directions, or thinking-aloud.\n"
        "- No cheerleading filler (\"Sure!\", \"Absolutely!\", \"Happy to help!\").\n"
        "- On completed actions: \"Done.\", \"Very well.\", or one crisp fact—then stop.\n"
        "- Ambient noise, TV, music, partial phrases, or speech not clearly for "
        "you: remain silent. Do not invent a reply.\n"
        "- Never narrate that you are listening or thinking.\n\n"
        "Use the declared tools when current or remembered facts are needed; "
        "never claim a tool succeeded unless its response says so. Be useful, "
        "not chatty. Serve Sir with quiet competence.";
    char when[160] = "";
    time_t tt = time(NULL);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    if (tmv.tm_year >= 2020 - 1900) {
        strftime(when, sizeof when,
                 "\n\nContext: the local time is %A, %I:%M %p. Let any "
                 "greeting or courtesy match it; at unsociable hours a dry "
                 "aside is welcome.", &tmv);
    }
    snprintf(s_sys_instr, sizeof s_sys_instr, "%s%s", kBase, when);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=====================================================");
    ESP_LOGI(TAG, " JarvisRobot v5  |  hexagonal core  |  voice boot     ");
    ESP_LOGI(TAG, "=====================================================");

    init_nvs();

    /* Inbound event slots go to PSRAM FIRST — 36 KB of tool-call buffers in
     * internal BSS previously starved the voice task's 20 KB internal-only
     * stack (observed live: creation failed with 24.8 KB free). */
    s_app.io.inq = heap_caps_calloc(INBOX_CAP, sizeof(inbound_event_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_app.io.inq == NULL) {
        s_app.io.inq = heap_caps_calloc(INBOX_CAP, sizeof(inbound_event_t),
                                        MALLOC_CAP_8BIT);
    }
    if (s_app.io.inq == NULL) {
        ESP_LOGE(TAG, "inbound event queue alloc failed — server events will drop");
    }

    /* Log ring FIRST so the whole boot lands in /api/logs. Non-fatal. */
    s_logring = heap_caps_malloc(LOGRING_CAP,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_logring != NULL) {
        s_logring_prev = esp_log_set_vprintf(logring_vprintf);
    } else {
        ESP_LOGW(TAG, "log ring alloc failed — /api/logs disabled");
    }

    /* VAD/barge diagnostic ring (PSRAM) — records every decision for offline
     * barge tuning; pulled via /api/diag/vadlog. Non-fatal if it fails. */
    s_vadlog = heap_caps_calloc(VADLOG_CAP, sizeof(vadlog_entry_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_vadlog == NULL) {
        ESP_LOGW(TAG, "vadlog ring alloc failed — /api/diag/vadlog disabled");
    }
    s_tool_poll_result = heap_caps_calloc(1, sizeof(*s_tool_poll_result),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tool_poll_result == NULL) {
        s_tool_poll_result = heap_caps_calloc(1, sizeof(*s_tool_poll_result),
                                              MALLOC_CAP_8BIT);
    }
    if (s_tool_poll_result == NULL) {
        ESP_LOGE(TAG, "tool result buffer alloc failed — tools stay disabled");
    }

    /* Reserve the proven 20 KB real-time stack before board, Wi-Fi, codec,
     * display, or HTTP allocations can fragment internal SRAM. The task does
     * not touch the still-unbuilt composition graph until the start gate is
     * opened at the end of app_main. Keeping this stack internal also avoids
     * making the audio owner depend on PSRAM/cache availability. */
    ESP_LOGI(TAG, "reserving voice task before hardware bring-up");
    BaseType_t task_ok = xTaskCreatePinnedToCore(
        voice_task, "jr_voice", 23040, NULL, 7, &s_voice_task, 1);
    /* THE DEEPEST-STACK TASK IN THE BUILD — sized by incident, not by guess.
     *
     * A first pass measured peak use at 15,236 B and cut this to 17,408. Three
     * disarm/rearm reconnect cycles then drove min-ever-free down to 1,412 B
     * (peak 15,996 B) — the reconnect path is deeper than a steady voice turn —
     * so it went back to 20480. Then the ask_user path (toolCall parse ->
     * snapshot -> session outcome -> PresentChoices exec, 2026-07-19) blew
     * straight through 20480 the first time it ran: instant stack overflow.
     * Every new event/command lane deepens this task's worst case. The latest
     * native-duplex/tool path measured 2,972 B free at 23,552; 23,040 retains
     * ~2.46 KB margin.
     *
     * The other stacks in this build WERE reduced, because their call graphs are
     * simple and bounded (render, touch, present, websocket). This one is not. */
    if (task_ok != pdPASS) {
        s_voice_task = NULL;
        ESP_LOGE(TAG,
                 "voice task creation failed (internal_free=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    /* L0 board bring-up */
    esp_err_t hal_err = jr_hal_init();
    if (hal_err != ESP_OK) {
        ESP_LOGE(TAG, "jr_hal_init failed: %s — continuing headless", esp_err_to_name(hal_err));
    }
#if defined(CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C)
    const gpio_config_t boot_button = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t boot_err = gpio_config(&boot_button);
    if (boot_err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT button unavailable: %s",
                 esp_err_to_name(boot_err));
    }
#endif

    /* Sensor samplers, always-on from boot. This was NOT safe on 2026-07-18 —
     * two internal-RAM task stacks cost ~7 KB, dropped the largest contiguous
     * internal block to 7,680 B, and the Gemini TLS handshake died with
     * `esp-aes: Failed to allocate memory` (the AES accelerator needs
     * DMA-capable INTERNAL memory that PSRAM cannot serve).
     *
     * It is safe now because both samplers allocate their stacks from PSRAM via
     * xTaskCreateWithCaps, so they cost internal RAM only for their TCBs. That,
     * plus right-sizing every oversized stack from measured high-water marks,
     * turned the budget from "one feature away from breaking voice" into real
     * headroom. Numbers in docs/JARVISNANO_OS_PLAN.md "Internal RAM budget".
     *
     * Neither is fatal: a missing IMU or PMIC just reports available=false. */
    esp_err_t imu_err = jr_imu_start();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "imu sampler unavailable: %s", esp_err_to_name(imu_err));
    }
    esp_err_t pwr_err = jr_power_start();
    if (pwr_err != ESP_OK) {
        ESP_LOGW(TAG, "battery sampler unavailable: %s", esp_err_to_name(pwr_err));
    }
#if !defined(CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C)
    esp_err_t rtc_err = jr_rtc_start();
    if (rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 unavailable: %s", esp_err_to_name(rtc_err));
    }
#else
    ESP_LOGI(TAG, "wall clock: 1.75C has no PCF85063; using SNTP");
#endif
    /* Seed with the CURRENT tick, not 0: the first mood step runs ~14 s into
     * boot, so a zero seed reads as 14 s of stillness and the device dropped
     * straight to AMBIENT (48 %) about 10 ms after "boot complete" — dimming
     * itself before anyone had touched it. */
    jr_mood_reset(&s_mood, (uint32_t)(esp_timer_get_time() / 1000));
    atomic_store(&s_mood_id, (uint8_t)JR_MOOD_AWAKE);
    atomic_store(&s_mood_brightness, 100);

    /* pull the concrete ports. The real presenter is preferred; its panel +
     * SPIFFS + gfx bring-up runs async in its own task, so this never stalls
     * boot. On resource failure, fall back to the jr_hal logging stub
     * (headless, exactly the pre-Phase-3 behavior). */
    s_app.clock   = jr_hal_clock();
    if (jr_display_start(&s_app.display) == ESP_OK) {
        ESP_LOGI(TAG, "display: presenter injected (CO5300 async bring-up)");
    } else {
        s_app.display = jr_hal_display();
        ESP_LOGW(TAG, "display: presenter start failed — falling back to log stub");
    }
    s_app.input   = jr_hal_input();

    /* idle face immediately (Phase-0 acceptance target) */
    s_app.last_face = JR_FACE_IDLE;
    jr_display_present(&s_app.display, JR_FACE_IDLE, 0);

    /* network + config */
    if (jr_net_init() == ESP_OK) {
        esp_err_t wc = jr_net_wifi_connect();
        if (wc != ESP_OK) {
            ESP_LOGW(TAG, "wifi not connected (%s) — diag/session will retry once provisioned",
                     esp_err_to_name(wc));
        }
    }
    restore_ota_attempt();

    /* Wall-clock time: the watch face, night theming and courtesy lines all
     * need it, and until now the device had NONE (current_time is an external
     * tool call). esp_netif_sntp retries internally until Wi-Fi is up, so
     * starting it here is safe even before the first got-ip. TZ is compile-time
     * for now; SVC-04 settings can make it NVS-backed later. */
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);   /* US Eastern w/ DST rules */
    tzset();
    {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sntp_cfg.start = true;
        if (esp_netif_sntp_init(&sntp_cfg) != ESP_OK) {
            ESP_LOGW(TAG, "sntp init failed — clock features stay dormant");
        }
        struct tm seeded;
        if (device_wall_time(&seeded)) {
            ESP_LOGI(TAG, "wall clock %04d-%02d-%02d %02d:%02d (rtc_seed=%d)",
                     seeded.tm_year + 1900, seeded.tm_mon + 1, seeded.tm_mday,
                     seeded.tm_hour, seeded.tm_min, (int)s_rtc_seeded_os);
        }
    }

    /* audio path */
    if (jr_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "jr_audio_init degraded — capture may be silent until on-device tune");
    }
    restore_out_vol();   /* gesture-set volume survives reboot */
    restore_brightness_cap();
    s_app.mic = jr_audio_source();
    s_app.spk = jr_audio_sink();

    /* Gemini endpoint auth (key from NVS, never in-repo). The key rides only
     * the x-goog-api-key upgrade header and the URL stays bare. The WebSocket
     * client logs its URI on transport errors, so query-string key fallback is
     * intentionally forbidden: authentication failure stays bounded and
     * fail-closed instead of leaking the credential into serial or /api/logs. */
    char key[320] = {0};
    if (jr_cfg_get_str("llm_api_key", key, sizeof key) == ESP_OK && key[0] != '\0') {
        snprintf(s_app.url, sizeof s_app.url, "%s", GEMINI_WS_BASE);
        snprintf(s_ws_headers, sizeof s_ws_headers,
                 "x-goog-api-key: %s\r\n", key);
        jr_gemini_ws_set_headers(s_ws_headers);
        ESP_LOGI(TAG, "gemini endpoint configured (key len=%u, auth=header)",
                 (unsigned)strlen(key));
    } else {
        snprintf(s_app.url, sizeof s_app.url, "%s", GEMINI_WS_BASE);
        ESP_LOGW(TAG, "no llm_api_key in NVS 'app' — provision before a session will connect");
    }
    secure_zero(key, sizeof key);

    /* L2 transport: device WS byte transport under the host-tested framer */
    esp_err_t ws_err = jr_gemini_ws_init(s_app.url);
    if (ws_err != ESP_OK) {
        ESP_LOGE(TAG, "gemini ws init failed: %s", esp_err_to_name(ws_err));
    }
    s_app.ws = jr_gemini_ws();

    memset(&s_app.cfg, 0, sizeof s_app.cfg);
    s_app.cfg.url          = s_app.url;
    s_app.cfg.model        = JR_GEMINI_MODEL_PRIMARY;
    /* The soul of the device. For gemini-3.1-flash-live-preview (native audio)
     * the ONLY language lever is the system instruction — the model ignores
     * speechConfig.languageCode and picks language itself (Codex-verified
     * against Google's Live docs). So the English mandate leads, in Google's
     * own recommended forceful phrasing, then the JARVIS character + concise
     * spoken style + stay-silent-on-ambient rule so it doesn't answer itself. */
    compose_system_instruction();
    s_app.cfg.system_instruction = s_sys_instr;
    s_app.cfg.thinking_level = "low";
    /* Voice IS honored on native audio; language is NOT (see above). Do not set
     * language_code/proactive_audio: unsupported on this model, and the firmware
     * post-speech refractory is the real phantom-turn defense. */
    s_app.cfg.voice_name    = "Charon";   /* composed, assured — the butler canvas */
    s_app.cfg.output_transcription = true; /* log what JARVIS says (English proof) */
    /* Gemini server VAD is the native full-duplex path: microphone audio keeps
     * flowing through Listening, Thinking, and Speaking so the service can
     * retain first syllables and interrupt its own reply. The earlier rollback
     * used four-frame/128 ms payloads that exceeded JR_GEMINI_TXQ_SLOT and a
     * 60 ms socket wait. The current two-frame/64 ms batches fit the slot and
     * the transport wait is bounded to 20 ms, so those failure conditions no
     * longer apply. Local VAD remains diagnostic only; deliberate privacy still
     * stops capture at the owner boundary. */
    s_app.cfg.vad_mode     = JR_VAD_SERVER;
    s_app.cfg.fns          = s_device_tool_fns;
    s_app.cfg.fn_count     = DEVICE_TOOL_DECL_COUNT;

    jr_gemini_client_init(&s_app.client, s_app.ws, s_app.clock, &s_app.cfg);
    jr_gemini_client_set_event_cb(&s_app.client, rich_cb, &s_app);
    s_app.rvc = jr_gemini_client_as_rvc(&s_app.client);

    /* L3 orchestrator: inject the real I/O port */
    jr_orch_io_t io;
    io.ctx = &s_app;
    io.poll_inbound = voice_poll;
    io.exec = voice_exec;
    jr_orch_init(&s_app.orch, s_app.clock, io, JR_VAD_MANUAL_LOCAL_RMS);
    jr_turn_policy_init(&s_app.turn);
    /* The transport sees soft-fail detail that its provider-neutral return code
     * intentionally collapses to WOULD_BLOCK. Feed the owned monitor here once. */
    jr_gemini_client_set_monitors(&s_app.client, NULL, &s_app.orch.dead_uplink);

    /* Diagnostics and command ingress come after the critical real-time stack
     * has a guaranteed contiguous allocation. */
    s_say_q = xQueueCreate(4, sizeof(char[200]));
    if (s_say_q == NULL) {
        ESP_LOGE(TAG, "say queue creation failed");
    }
    s_agent_link_lock = xSemaphoreCreateMutex();
    if (s_agent_link_lock == NULL) {
        ESP_LOGE(TAG, "Agent Link state lock creation failed");
    }
    s_brain_lock = xSemaphoreCreateMutex();
    if (s_brain_lock == NULL) {
        ESP_LOGE(TAG, "Brain Link state lock creation failed");
    }
    start_diag_http();

    /* The JarvisMCP bridge is part of this firmware's composition graph, not a
     * companion feature. Its endpoint/key are copied from NVS and remain
     * redacted; an unconfigured device still boots and returns a bounded error
     * if Gemini attempts a tool call. Initialize after reserving the voice
     * task's internal-only stack so the larger queues may safely use PSRAM. */
    esp_err_t tools_err = s_tool_poll_result != NULL
        ? jr_tools_init(NULL) : ESP_ERR_NO_MEM;
    if (tools_err == ESP_OK) {
        jr_tools_set_session_generation(s_app.orch.session.session_gen);
        tools_err = jr_tools_start();
    }
    if (tools_err == ESP_OK) {
        atomic_store(&s_tool_diag.worker_ready, true);
        ESP_LOGI(TAG, "on-device tools ready (configured=%d declared=%u)",
                 jr_tools_is_configured(), (unsigned)DEVICE_TOOL_DECL_COUNT);
    } else {
        ESP_LOGE(TAG, "on-device tools unavailable: %s",
                 esp_err_to_name(tools_err));
    }

    /* Wake word LAST in the internal-RAM budget line. WakeNet's ~20 KB scratch
     * competes with the feeder task, httpd, and the tools worker; those carry
     * the product (speech out, diagnostics, tool calls) so they draw first and
     * wake absorbs only the remainder. A failed init degrades to tap/lift wake
     * exactly as before Phase 5 — never the other way around. */
    esp_err_t wake_err = jr_wake_init();
    if (wake_err != ESP_OK) {
        ESP_LOGW(TAG, "wake word unavailable: %s", esp_err_to_name(wake_err));
    }

    atomic_store(&s_voice_start_gate, true);
    if (task_ok == pdPASS && VOICE_ALWAYS_READY) {
        atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
    }

    /* Start the stillness clock HERE, not at jr_mood_reset() above: that runs
     * ~1 s in, while Wi-Fi and the codec still have ~13 s to go, so the device
     * spent most of its rest budget before it could do anything and dimmed a
     * few seconds after becoming usable. The user's first full AWAKE window
     * should begin when JARVIS is actually ready. */
    jr_mood_poke_awake(&s_mood, (uint32_t)(esp_timer_get_time() / 1000));
    ESP_LOGI(TAG, "boot complete — always-ready voice requested");
}
