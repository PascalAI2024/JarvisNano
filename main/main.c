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
 *
 * Since 2026-09-02 the root is four files sharing one internal seam, app.h:
 * this one keeps the wiring, the voice task and presentation; http_routes.c
 * is the control plane; power.c the gears and deep sleep; device_tools.c the
 * device-tool lane. Ownership did not move — only the line numbers did.
 */
#include "app.h"

static const char *TAG = "jarvis_v5";

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
#define AUDIO_DIAG_CAPTURE_MS 1800U

/* The model sees fixed templates plus two server-policy meta-tools. The worker
 * owns HTTPS on-device; no Android/Mac companion participates in the
 * voice -> tool -> voice path. `remember` is intercepted for physical
 * confirmation, and execute_tool can never bypass JarvisMCP server policy. */
#define REMEMBER_NEEDS_TAP 0

const jr_gemini_fn_decl_t s_device_tool_fns[] = {
    {
        .name = "recall_memory",
        .description = "Search Pascal's Jarvis memory for relevant context.",
        .arg_name = "query",
        .arg_desc = "A concise natural-language memory search query.",
    },
    {
        .name = "remember",
        .description =
            "Save something Sir tells you to remember into Jarvis memory. His "
            "asking is the approval; say back what you saved.",
        .arg_name = "note",
        .arg_desc =
            "The note in plain words, at most 200 characters: letters, digits, "
            "spaces and simple punctuation only.",
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
            "Find a Jarvis capability by describing it: web search, weather, "
            "Wikipedia, crypto and stock prices, exchange rates, news, "
            "translation, research papers, memory. Returns tool names with "
            "their parameters, in order, for execute_tool.",
        .arg_name = "query",
        .arg_desc =
            "A concise description of the capability needed.",
    },
    {
        .name = "execute_tool",
        .description =
            "Run one Jarvis capability. Common tools need no search first: "
            "websearch {\"query\"} for anything on the live web or in the news; "
            "weather {\"latitude\",\"longitude\"} (Fort Lauderdale is 26.12, "
            "-80.14); wiki {\"query\"}; crypto {\"coin\",\"currency\"}; "
            "stocks.quote {\"symbols\"}; time {\"timezone\"}. For Sir's own "
            "life and work: memory.capture {\"title\",\"body\",\"tags\"} to "
            "remember anything he tells you (say what you remembered); "
            "memory.search {\"query\"} to recall it; butlercrm.calendar.upcoming "
            "{} and butlercrm.calendar.create {\"title\",\"starts_at\"} for his "
            "calendar; coordination.portfolio {} and coordination.createWorkItem "
            "{\"projectId\",\"title\"} for the work board. Otherwise use the "
            "tool and params search_tools returned, keys in that order.",
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

_Static_assert(sizeof(s_device_tool_fns) / sizeof(s_device_tool_fns[0]) ==
                   DEVICE_TOOL_DECL_COUNT,
               "DEVICE_TOOL_DECL_COUNT in app.h must match the catalog");


/* DISPLAY LABELS ARE NOT CANONICAL TOOL IDS.
 *
 * TOOLS used to publish s_device_tool_fns[i].name straight to the glass, but
 * those strings are the protocol identifiers Gemini is given, and the shell
 * stores twelve glyphs. "recall_memory" is thirteen and rendered as
 * "RECALL_MEMOR"; "set_brightness" is fourteen. A name chosen for a wire
 * format has no reason to fit a round display, and truncating it silently is
 * the worst of both.
 *
 * The row width is the enforcement: each entry is char[13], so a label that
 * does not fit is a COMPILE error ("initializer-string for array of chars is
 * too long"), not a defect discovered on the panel. The static assert keeps
 * this table and the catalog the same length, so adding a tool without naming
 * it fails the build rather than shipping a blank petal. Order matches
 * s_device_tool_fns exactly. */
static const char s_device_tool_labels[][13] = {
    "RECALL",       /* recall_memory  */
    "REMEMBER",     /* remember       */
    "TIME",         /* current_time   */
    "SEARCH",       /* search_tools   */
    "EXECUTE",      /* execute_tool   */
    "VOLUME",       /* set_volume     */
    "LIGHT",        /* set_brightness */
    "ASK",          /* ask_user       */
};

_Static_assert(sizeof(s_device_tool_labels) / sizeof(s_device_tool_labels[0]) ==
                   DEVICE_TOOL_DECL_COUNT,
               "every declared tool needs exactly one display label");


device_tool_diag_t s_tool_diag;
/* Single-writer voice-task fallback lane for queue/unavailable failures. It
 * holds metadata and a fixed error object only, never a key, endpoint, or MCP
 * result payload. */
local_tool_result_t s_local_tool_results[LOCAL_TOOL_RESULT_CAP];
size_t s_local_tool_head;
size_t s_local_tool_count;
jr_tool_result_t *s_tool_poll_result;
pending_tool_consent_t s_tool_consent;

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
_Atomic uint32_t s_sim_touch;

/* Debug-choices request lane: 0 == none, else requested arc count + 1 (so a
 * dismiss, n == 0, encodes as 1). The httpd handler only posts here; the app
 * task drains it — jr_display's choice statics keep exactly ONE writer. */
_Atomic uint32_t s_debug_choices_req;

/* App-task only: taps are swallowed until this deadline right after an arc was
 * tapped, so the trailing contact of a double-tap cannot reach the mute
 * toggle the instant the arcs synchronously dismiss. */
static uint32_t s_ask_tap_grace_ms;

/* Synthetic shake lane (GEST-03 verification): number of 10 Hz polls that
 * should read as shake-positive. The endpoint posts 2 — exactly the sustained
 * window the detector demands — so the sim exercises the REAL persistence
 * filter, not a bypass. */
_Atomic uint32_t s_sim_shake;

/* Synthetic flip lane (GEST-02): number of 10 Hz polls that should read as
 * face_down. The endpoint posts enough to satisfy the sustain filter. */
_Atomic uint32_t s_sim_flip;

/* Gesture layer (app task only): swipe-right watch peek deadline, and the
 * double-tap window for the attention gesture. */
uint32_t s_watch_peek_until_ms;
static uint32_t s_last_tap_ms;
/* Hold-to-commit: ms at which the current physical contact was confirmed, or 0
 * when no hold is in flight. App task only. */
uint32_t s_hold_start_ms;
_Atomic uint32_t s_pairing_claim_until_ms;
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
int s_out_vol = 100;
uint8_t s_brightness_cap = 100;
_Atomic int s_level_volume_request = -1;
_Atomic int s_level_brightness_request = -1;

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

void persist_ota_attempt(int slot)
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
char *s_logring;                 /* PSRAM, alloc'd in app_main */
volatile size_t s_logring_head;  /* next write offset */
volatile size_t s_logring_len;   /* filled bytes, saturates at CAP */
portMUX_TYPE s_logring_mux = portMUX_INITIALIZER_UNLOCKED;
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
_Atomic uint32_t s_operator_lease_until_ms;
_Atomic bool s_operator_mode_active;
_Atomic uint32_t s_operator_mode_entered_ms;
_Atomic bool s_ota_active;
_Atomic uint32_t s_ota_received_bytes;
_Atomic uint32_t s_ota_total_bytes;
_Atomic int s_ota_last_error;
_Atomic bool s_ota_preflight_blocked;
_Atomic bool s_http_ready;

bool operator_lease_active(uint32_t now_ms)
{
    uint32_t until = atomic_load(&s_operator_lease_until_ms);
    return until != 0U && (int32_t)(now_ms - until) < 0;
}

bool operator_mode_active(uint32_t now_ms)
{
    return atomic_load(&s_operator_mode_active) &&
           operator_lease_active(now_ms);
}

/* Defined next to app_main (it owns the persona text); SEND_SETUP refreshes
 * it so every session's instruction carries the current local time. */
static void compose_system_instruction(void);
static void handle_say(const char *text);

/* Attract-reel state (POLISH-06). The httpd handler only posts the request;
 * everything else is app-task single-writer. */
_Atomic bool s_demo_req;
uint32_t s_demo_start_ms;   /* app task only; 0 = off */
static int      s_demo_step = -1;
static int      s_demo_last_ripple = -1;

/* WebSocket auth state (see endpoint-auth block in app_main). The header
 * buffer holds the API key and must never be logged; the URI always stays bare. */
static char    s_ws_headers[400];
static bool    s_ws_auth_header_ok_logged;


/* ---- mapped inbound event queue (single-threaded: only the app task) ----
 * The slots live in PSRAM (heap, allocated at boot): 16 x ~2.2 KB of tool-call
 * buffers is ~36 KB — as static BSS it starved internal SRAM until the voice
 * task stack (20 KB, internal-only) could no longer allocate. Task-context
 * data only, never touched by ISR/DMA, so external RAM is safe. */


jr_app_t s_app;
_Atomic uint32_t s_last_tx_drop_ms;

/* diag: say-mailbox drained by the app task */
QueueHandle_t s_say_q;   /* of char[200] */

/* A text turn armed via /api/debug/say. It must NOT be sent until the session
 * reaches Listening (transport OPEN + setup complete) — sending into a half-open
 * WS corrupts the turn. voice_task flushes it once, on entering Listening. */
static char        s_pending_text[200];
static volatile bool s_pending_text_set;
char        s_last_said[192];   /* tail of JARVIS's last spoken transcript */
static uint32_t    s_always_ready_rearm_ms;  /* cooldown gate for idle re-arm */

/* Rolling caption accumulator (app task only). The output transcript arrives
 * as fragments; the on-glass chip (STATE-04) shows the TAIL of the current
 * turn so a reader can follow along. Front-clipped in place — a subtitle, not
 * an archive. */
static char s_caption_acc[128];
/* A LIVE CAPTION MUST SHOW THE NEWEST WORDS, NOT THE OLDEST.
 *
 * The accumulator holds 128 characters, but the caption band renders two lines
 * of 19 glyphs — 38 characters — and hud_wrap2 takes them from the FRONT. So a
 * reply longer than 38 characters displayed its opening and then froze there,
 * while the rest of the sentence accumulated invisibly. The caption only ever
 * moved once the buffer overflowed 128 and began dropping from the head, which
 * is both far too late and reads as a device that has stopped listening.
 *
 * Show the last 38 characters instead, and step forward to a word boundary
 * when one is close enough that the jump costs less than starting mid-word. */
static void caption_show_tail(const char *acc)
{
    enum { CAP = 38U };                 /* 2 lines x 19 glyphs */
    const size_t n = strlen(acc);
    const char *tail = acc;
    if (n > (size_t)CAP) {
        tail = acc + (n - (size_t)CAP);
        const char *sp = strchr(tail, ' ');
        if (sp != NULL && (size_t)(sp + 1 - tail) <= 12U) {
            tail = sp + 1;              /* don't open mid-word */
        }
    }
    jr_display_caption_set(tail);
}

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
    caption_show_tail(s_caption_acc);
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
vadlog_entry_t *s_vadlog;
_Atomic uint32_t s_vadlog_seq;   /* monotonic push count               */

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
TaskHandle_t s_voice_task;
volatile bool s_voice_task_running;
volatile uint32_t s_voice_task_heartbeat_ms;
static _Atomic bool s_voice_start_gate;
static bool s_pending_text_inflight;
static uint32_t s_pending_text_retry_ms;
/* Local barge is opt-in. Live 1.75C evidence showed the AEC residual clearing
 * the local 0.10 gate during model speech, forcing Speaking->Listening->Speaking
 * transitions that sound like hiccups. Gemini server VAD remains active and
 * owns normal interruption; /api/debug/gain?barge=1 is the calibration switch
 * for controlled local-gate experiments. */
volatile bool s_local_barge_enabled = false;

/* Human/diagnostic controls are produced by HTTP and consumed only by the
 * voice task. The orchestrator therefore remains the sole SessionState writer. */

_Atomic int s_voice_control_request;
/* BOOTS MUTED, deliberately. A voice assistant that comes up listening is a
 * privacy problem: after a power cut, a flash, or a battery swap the mic would
 * be live before anyone in the room knows the device is on. Starting paused
 * means the first thing the owner does is DELIBERATELY open the mic — long-
 * press, PWR, or the shade control — and the gold privacy ring says plainly
 * that it is shut until they do.
 *
 * This costs a wake-word cold start: "Jarvis" will not open a session until
 * the owner unmutes once. That is the correct trade for an always-on
 * microphone on a desk. */
_Atomic bool s_voice_privacy_paused = true;
/* ---- a deaf session gets a fresh one ------------------------------------
 *
 * Gemini owns turn detection here, so when the server goes quiet the device
 * has nothing to wake it: the local VAD sees whole utterances, the uplink
 * runs, no frame comes back, and the owner talks to a wall. Seen on the
 * glass 2026-09-01: three questions in 40 s, no reply, then it answered
 * again as if nothing happened. So every utterance of some length that ends
 * in Listening starts a clock; ANY server frame or a phase change stops it;
 * two unanswered in a row are a deaf session and the core is told
 * StaleDeadline, which is exactly what it does for a keepalive miss —
 * reconnect with the resume handle. Ambient chatter Gemini rightly ignores
 * can trip this too; the cost is one reconnect. */
#define UTT_MIN_MS          800U
#define UTT_REPLY_WAIT_MS   7000U
#define UTT_DEAF_COUNT      2U
static uint32_t         s_utt_start_ms;
static _Atomic uint32_t s_reply_deadline_ms;
static uint8_t          s_unanswered;
static _Atomic uint32_t s_unanswered_total;
static temperature_sensor_handle_t s_tsens;
jr_mood_state_t s_mood;
_Atomic uint8_t s_mood_id;
_Atomic uint8_t s_mood_brightness;
bool s_mood_rest_disarmed;
/* Hoisted out of the IMU block in voice_task: the tap-to-wake path also needs
 * it, to tell "the mood ladder put us to sleep" (tap may undo) apart from
 * "the user flipped the puck face-down" (tap must NOT undo). */
bool s_flip_muted;
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
_Atomic bool s_vad_use_clean = true;
static uint8_t s_bright_now = 100;
static uint8_t s_bright_tgt = 100;
static bool s_rtc_seeded_os;
static bool s_os_seeded_rtc;
_Atomic bool s_audio_diag_requested;
_Atomic uint32_t s_audio_diag_until_ms;
static bool s_listen_speech_active;
_Atomic bool s_ui_shade_open;

/* Touch observability plus the randomized three-round panel/touch proof. */
_Atomic uint32_t s_touch_events;
_Atomic uint32_t s_touch_taps;
_Atomic uint32_t s_touch_long_presses;
_Atomic uint32_t s_touch_swipes;
_Atomic uint32_t s_touch_last_kind;
_Atomic uint32_t s_touch_last_x;
_Atomic uint32_t s_touch_last_y;
_Atomic int s_touch_last_dx;
_Atomic int s_touch_last_dy;
_Atomic uint32_t s_touch_last_duration_ms;

_Atomic bool s_touch_challenge_start_requested;
_Atomic bool s_touch_challenge_cancel_requested;
_Atomic bool s_touch_challenge_active;
_Atomic bool s_touch_challenge_verified;
_Atomic uint32_t s_touch_challenge_expected;
_Atomic uint32_t s_touch_challenge_correct;
_Atomic uint32_t s_touch_challenge_attempts;
_Atomic uint32_t s_touch_challenge_wrong;
_Atomic uint32_t s_touch_challenge_last_mapped;
_Atomic uint32_t s_touch_challenge_last_latency_ms;
static _Atomic uint32_t s_touch_challenge_round_started_ms;
_Atomic uint32_t s_touch_challenge_restore_ms;


SemaphoreHandle_t s_agent_link_lock;
agent_link_state_t s_agent_link;
/* Revisions are globally monotonic for the lifetime of this boot. Agent Link
 * has one authenticated writer stream; retaining this high-water mark across
 * task switches prevents an older signed-in payload from becoming current. */
uint32_t s_agent_link_revision_hwm;

/* Brain Link is the backend-neutral optional companion seam. Gemini and the
 * bounded JarvisMCP worker run directly on the physical device today; a paired
 * Mac/Android companion may additionally present a surface and receive button
 * actions. The device credential remains NVS-only and is never returned here. */


SemaphoreHandle_t s_brain_lock;
brain_surface_state_t s_brain_surface;
brain_action_event_t s_brain_events[BRAIN_EVENT_CAP];
uint32_t s_brain_inbox_seq_hwm;
uint32_t s_brain_event_seq;
uint32_t s_brain_last_seen_ms;

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
    if (ge->kind != JR_GEV_RESUMPTION_UPDATE && ge->kind != JR_GEV_UNKNOWN) {
        atomic_store(&s_reply_deadline_ms, 0U);   /* the server is there */
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
        activity_note_turn_end();
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
            activity_note_said(ge->text);
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
        /* A SPOKEN "REMEMBER" IS THE APPROVAL. The consent arc that used to
         * intercept this tool asked the owner to tap the glass to confirm a
         * note they had just dictated; from across the room the tap never
         * came, the prompt timed out, and Jarvis apologised for not saving
         * what he had been told twice. The owner's instruction is physical
         * authority already (it is his voice in his room); synthetic input
         * cannot dictate a note because it cannot speak. The arc stays in
         * the tree for tools that do warrant it. */
        if (REMEMBER_NEEDS_TAP && cmd->tool_name != NULL &&
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
        activity_note_tool(cmd->tool_name, cmd->tool_args);
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
    case JR_ST_THINKING:  return JR_FACE_THINKING;
    /* Connecting is not thinking: the reactor idles while a dot orbits the
     * bezel, so a slow handshake reads as "reaching", not "considering". */
    case JR_ST_CONNECTING:
    case JR_ST_HANDSHAKING:
    case JR_ST_RECONNECTING: return JR_FACE_LINKING;
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


ota_preflight_t ota_preflight(void)
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

/* Fit a title into the shell's twelve glyphs without cutting it mid-word and
 * without pretending it was whole.
 *
 * Agent-link titles carry up to 48 characters and were strlcpy'd into a
 * 13-byte cache, so anything longer lost its tail with no wrap and no mark —
 * "DEPLOY STAGING BUILD" arrived as "DEPLOY STAG" and read like a different,
 * complete task. Back up to a word boundary when one is near enough to be
 * worth the space, and always spend the last glyph on a full stop so a
 * shortened title is visibly shortened. */
void title_shorten(char *dst, size_t cap, const char *src)
{
    const size_t max = cap - 1U;
    const size_t n = strnlen(src, AGENT_TITLE_CAP);
    if (n <= max) {
        memcpy(dst, src, n);
        dst[n] = '\0';
        return;
    }
    size_t keep = max - 1U;                  /* one glyph for the mark */
    size_t cut = keep;
    while (cut > 0U && src[cut] != ' ') {
        cut--;
    }
    if (cut >= max / 2U) {                   /* boundary close enough to use */
        keep = cut;
    }
    memcpy(dst, src, keep);
    dst[keep] = '.';
    dst[keep + 1U] = '\0';
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
        /* AN EXPIRED LINK MUST ACTUALLY GO AWAY.
         *
         * This branch computed the TTL and then did nothing, so `active` was
         * never cleared: DESK kept rendering a task that had finished or died,
         * and the agent rim stayed lit indefinitely with no way to dismiss it.
         * A stale surface that outlives its owner reads as a frozen device,
         * because the one thing it will not do is respond.
         *
         * The counters (updates, rejects) are lifetime statistics and survive;
         * everything the glass reads is cleared. */
        if (s_agent_link.active &&
            (int32_t)(now_ms - s_agent_link.expires_ms) >= 0) {
            ESP_LOGI(TAG, "agent: link expired, clearing task_id=%s",
                     s_agent_link.task_id);
            s_agent_link.active = false;
            s_agent_link.progress = 0;
            s_agent_link.state[0] = '\0';
            s_agent_link.title[0] = '\0';
            s_agent_link.summary[0] = '\0';
            s_agent_link.evidence_count = 0;
        }
        cached_active = s_agent_link.active;
        cached_progress = s_agent_link.progress;
        cached_state = agent_state_to_display(s_agent_link.state);
        title_shorten(cached_title, sizeof(cached_title),
                      s_agent_link.active && s_agent_link.title[0] != '\0'
                          ? s_agent_link.title : "STANDBY");
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

    /* TOOLS is gone; ACTIVITY is fed at turn end (activity_note_turn). */
    const jr_state_snapshot_t *snapshot = jr_orch_snapshot(&s_app.orch);
    jr_display_jarvis_set_session(
        s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN,
        (uint16_t)(snapshot->transitions / 2U), now_ms / 1000U);
    if ((int32_t)(now_ms - next_status_ms) >= 0) {
        jr_display_set_status((uint8_t)s_out_vol);

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

        /* STATUS's connections, once a second: the same readings the
         * cockpit serves, so the glass and /api/cockpit can never disagree.
         * The Wi-Fi query is a round trip into the radio task, which is why
         * it lives in this 1 Hz block and not in the per-tick path above. */
        jr_net_status_t net = {0};
        (void)jr_net_get_status(&net);
        bool desk_live = false;
        if (s_brain_lock != NULL &&
            xSemaphoreTake(s_brain_lock, 0) == pdTRUE) {
            desk_live = s_brain_last_seen_ms != 0U &&
                (uint32_t)(now_ms - s_brain_last_seen_ms) <=
                    BRAIN_DESK_FRESH_MS;
            xSemaphoreGive(s_brain_lock);
        }
        float chip_c = 0.0f;
        const bool chip_ok = s_tsens != NULL &&
            temperature_sensor_get_celsius(s_tsens, &chip_c) == ESP_OK;
        jr_display_links_t links = {
            .chip_c = (int8_t)(chip_ok ? lroundf(chip_c) : 0),
            .chip_c_valid = chip_ok,
            .cpu_mhz = (uint16_t)atomic_load(&s_cpu_mhz),
            .wifi_up = net.sta_connected,
            .rssi_dbm = net.rssi,
            .link_open = s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN,
            .tools = !jr_tools_is_configured() ? 0U
                     : atomic_load(&s_tool_diag.worker_ready) ? 2U : 1U,
            .desk_live = desk_live,
            .radio_saving = jr_net_power_save_active(),
        };
        strlcpy(links.ip, net.sta_connected ? net.sta_ip : "",
                sizeof links.ip);
        jr_display_links_set(&links);
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
static bool s_demo_owns_choices;   /* app task only: reel arcs are up */


void demo_stop(void)
{
    if (s_demo_start_ms == 0U) {
        return;
    }
    s_demo_start_ms = 0U;
    s_demo_step = -1;
    /* Dismiss only the arcs the REEL put up. This was unconditional, and the
     * reel's yield-to-a-real-ask path runs later in the same loop iteration
     * that presented the real ask's arcs — so it erased them, left the
     * orchestrator parked in ASKING with nothing to tap, and the ask could
     * only end by timeout. */
    if (s_demo_owns_choices) {
        s_demo_owns_choices = false;
        jr_display_dismiss_choices();
    }
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
        } else {
            ESP_LOGW(TAG, "demo: request dropped (phase=%s, running=%d)",
                     jr_state_name(p), s_demo_start_ms != 0U);
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
            s_demo_owns_choices = true;
            break;
        case 2:
            s_demo_owns_choices = false;
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
            static uint8_t s_move_polls, s_mood_fd_polls, s_mood_fu_polls;
            static bool s_mood_face_down;
            if (have_imu && imu.moving && imu.age_ms < 500U) {
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
            const bool moving = !s_mood_face_down && s_move_polls >= 3U;
            /* PRIVACY SILENCES THE MICROPHONE. IT DOES NOT BLIND THE GLASS.
             *
             * Muting used to count as face-down AND cancel user_busy, so a
             * muted device slammed to DREAM at brightness 6 — even while it
             * was mid-interaction. Caught live with mood=DREAM, brightness=6
             * and phase=ASKING at the same instant: the device was asking the
             * owner a question on a screen too dark to read. The owner's words
             * were "device too dark when its working and i need to read why".
             *
             * Booting muted (a deliberate privacy default) made this constant
             * rather than occasional, which is how it surfaced.
             *
             * Rest is now driven by what it always should have been: physical
             * stillness, and having nothing to show. A muted device with an
             * ask, a card or a reply on the glass stays lit long enough to be
             * read. Flip-to-mute still forces rest, because turning a device
             * face-down is an unambiguous "I am done looking at it".
             *
             * Motion is physical and is read the same way whether the mic is
             * live or not — gating it on privacy meant a boot-muted device
             * could not be woken by being picked up at all. */
            const bool on_cell = have_power && bat.present && !bat.usb_present;
            const bool saver = on_cell && bat.percent <= 100U &&
                               bat.percent <= BATTERY_SAVER_PCT;
            jr_mood_in_t min = {
                .now_ms = (uint32_t)now,
                .face_down = s_mood_face_down,
                .moving = moving,
                .user_busy = user_busy || (have_power && bat.usb_present),
                .saver = saver,
            };
            jr_mood_out_t mout = jr_mood_step(&s_mood, &min);
            const bool realtime_power =
                mout.voice_armed || user_busy ||
                operator_mode_active((uint32_t)now) ||
                atomic_load(&s_ota_active);
            (void)jr_net_set_power_save(!realtime_power);
            /* THE GEAR. Anything happening, or a cable: 240. Resting on the
             * cell with the session closed: REST. Never lower while the
             * audio self-test owns the codec. */
            {
                const int forced = atomic_load(&s_cpu_force);
                cpu_gear_set(forced != 0 ? forced
                             : (!on_cell || realtime_power ||
                                atomic_load(&s_audio_diag_until_ms) != 0U)
                                   ? CPU_MHZ_LIVE : CPU_MHZ_REST);
            }
            /* THE CADENCE. The engine draws every period whether or not a
             * pixel changed, so at rest the frame rate is the display's whole
             * cost. Anything live keeps the panel ceiling; the ladder steps
             * it down as the device settles. A touch restores 24 at once in
             * the input loop, before this tick can notice. */
            (void)jr_display_set_render_fps(
                (realtime_power || mout.mood == JR_MOOD_AWAKE) ? RENDER_FPS_LIVE
                : mout.mood == JR_MOOD_AMBIENT                ? RENDER_FPS_AMBIENT
                : mout.mood == JR_MOOD_WHISPER                ? RENDER_FPS_WHISPER
                                                              : RENDER_FPS_DREAM);
            uint8_t effective_brightness = (uint8_t)(
                ((unsigned)mout.brightness * s_brightness_cap + 50U) / 100U);
            {
                static uint8_t prev_mood = (uint8_t)JR_MOOD_AWAKE;
                const bool rested = prev_mood == (uint8_t)JR_MOOD_WHISPER ||
                                    prev_mood == (uint8_t)JR_MOOD_DREAM;
                if (mout.changed && mout.mood == JR_MOOD_AWAKE && rested &&
                    moving && s_weather.valid &&
                    jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE &&
                    !jr_display_choices_active()) {
                    jr_display_nav_set(JR_DISPLAY_SPACE_WEATHER);
                    jr_display_bloom();
                    s_glance_until_ms = (uint32_t)now + GLANCE_MS;
                    ESP_LOGI(TAG, "glance: lifted after rest, showing weather");
                }
                prev_mood = (uint8_t)mout.mood;
            }
            /* DEEP SLEEP WHEN NOT IN USE. The ladder says when (DREAM for
             * JR_MOOD_SLEEP_MS); the world says whether: never on USB (a
             * desk device on its cable is a desk device), never mid-update,
             * never with a companion in, and never in the first three
             * minutes, so a device is always reachable for a while after
             * any boot. A forced sleep (the debug route) skips the gates:
             * it exists to prove the wake sources from a desk. */
            {
                const bool forced = atomic_load(&s_sleep_force);
                const bool in_use =
                    (have_power && bat.usb_present) ||
                    atomic_load(&s_ota_active) ||
                    operator_mode_active((uint32_t)now);
                if (forced) {
                    enter_deep_sleep("forced", atomic_load(&s_sleep_timer_s));
                } else if (jr_mood_sleep_due(&s_mood, (uint32_t)now) &&
                           !in_use && now >= SLEEP_MIN_UPTIME_MS) {
                    enter_deep_sleep("not in use", SLEEP_TIMER_WAKE_S);
                }
            }
            if (s_glance_until_ms != 0U &&
                (int32_t)((uint32_t)now - s_glance_until_ms) >= 0) {
                s_glance_until_ms = 0U;
                if (jr_display_nav_space() == JR_DISPLAY_SPACE_WEATHER &&
                    jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE) {
                    jr_display_nav_home();
                    jr_display_caption_set("JARVIS");
                }
            }
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
                    /* HOLD PWR: OFF. The owner: "the big button hold should
                     * put it in the lowest state and only turn back on when
                     * held again — or off completely, might be better."
                     * Off completely it is: the PMIC drops every rail and
                     * only a one-second hold of the same key brings it
                     * back. The battery it used to read out lives on STATUS
                     * and in Jarvis's mouth. */
                    atomic_store(&s_power_off_req, true);
                    ESP_LOGI(TAG, "pkey: long -> power off");
                }
                if (atomic_load(&s_power_off_req) && image_in_probation()) {
                    /* A cold boot through the bootloader rolls back an image
                     * still on probation — the deep sleep taught this. */
                    atomic_store(&s_power_off_req, false);
                    jr_display_caption_set("UPDATING - TRY AGAIN IN A MINUTE");
                    ESP_LOGW(TAG, "power off refused: image in probation");
                }
                if (atomic_load(&s_power_off_req)) {
                    atomic_store(&s_power_off_req, false);
                    jr_display_caption_set("POWERING OFF - HOLD PWR TO START");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    jr_display_panel_off_request();
                    for (int i = 0; i < 30 && !jr_display_panel_is_off(); ++i) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                    const esp_err_t off_err = jr_power_off();
                    /* Only reached if the PMIC refused. */
                    ESP_LOGE(TAG, "power off refused: %s", esp_err_to_name(off_err));
                    jr_display_caption_set("POWER OFF FAILED");
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
                panic_home_clear_glass();
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
                if (!peek && s_watch_peek_until_ms != 0U) {
                    /* The peek's caption ("WATCH - 10 SECONDS", or the wall
                     * time it rolled to) used to outlive the peek: the hands
                     * left on schedule and the words stayed burned on. */
                    s_watch_peek_until_ms = 0U;
                    caption_reset();
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
            {
                const uint32_t deadline = atomic_load(&s_reply_deadline_ms);
                if (deadline != 0U && ph != JR_ST_LISTENING) {
                    atomic_store(&s_reply_deadline_ms, 0U);
                    s_unanswered = 0U;
                } else if (deadline != 0U &&
                           (int32_t)((uint32_t)now - deadline) >= 0) {
                    atomic_store(&s_reply_deadline_ms, 0U);
                    s_unanswered++;
                    atomic_fetch_add(&s_unanswered_total, 1U);
                    ESP_LOGW(TAG, "voice: utterance unanswered (%u in a row)",
                             (unsigned)s_unanswered);
                    if (s_unanswered < UTT_DEAF_COUNT) {
                        /* THE FIRST MISS GETS A NUDGE, NOT SILENCE. The model
                         * still holds the audio it did not answer; a short
                         * text turn asks it to answer what it heard or ask
                         * for a repeat. Seen on the glass: a clear 3 s
                         * question straight after connecting, ignored, and
                         * the owner waiting at a device that looked fine. */
                        handle_say("Sir just spoke and you did not answer. "
                                   "If you heard a question, answer it now; "
                                   "if it was unclear, ask one short question.");
                    }
                    if (s_unanswered >= UTT_DEAF_COUNT) {
                        s_unanswered = 0U;
                        ESP_LOGW(TAG, "voice: session is deaf — fresh session");
                        jr_display_caption_set("RECONNECTING");
                        jr_orch_inject(&s_app.orch,
                                       jr_event(JR_EV_STALE_DEADLINE), now);
                        ph = jr_orch_phase(&s_app.orch);
                    }
                }
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
            /* A finger on the glass wants frames now, not at the next mood
             * tick; unchanged values cost nothing. */
            (void)jr_display_set_render_fps(RENDER_FPS_LIVE);
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
                caption_reset();   /* a swallowed tap must not strand it */
            }
            s_glance_until_ms = 0U;   /* a touched glance is a chosen screen */
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
                /* Synthetic input cannot ESCAPE a lease — the double-tap
                 * exit, the card actions and the privacy hold are physical
                 * only. A swipe escapes nothing: it walks the ring under the
                 * guest exactly as a finger would. This guard used to drop
                 * every synthetic kind, so two screenshot sweeps taken under
                 * a lease returned six identical frames each and were read
                 * as the composition freezing (S21). It never froze: the
                 * compositor held 19 fps throughout; the sweep's swipes were
                 * being refused here. */
                if (!physical && iev.kind != JR_INPUT_SWIPE) {
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
                    /* THE TAP IS THE LEASE'S, SO IT STOPS HERE.
                     *
                     * Without this the branch fell out of the codex block and
                     * straight onto `codex_tap_pending = false` below, which
                     * cleared the flag in the SAME loop iteration that set it.
                     * It could therefore never be true when the next tap
                     * tested it: the double-tap escape never fired, and the
                     * deferred single tap was never dispatched to the card.
                     * A guest you cannot evict, holding a card you cannot
                     * press — "there's something on screen, can't even click
                     * it". Taps now stay with the lease and are flushed by the
                     * 400 ms timeout below. */
                    continue;
                }
                jr_display_caption_set("CODEX MODE - DOUBLE TAP TO EXIT");
                ESP_LOGI(TAG, "operator: guest holds the glass, kind=%d",
                         (int)iev.kind);
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
            /* The SHADE outranks double-tap-home too. Its volume arcs step
             * +-10, so adjusting volume IS a rapid repeated tap — and any
             * second contact inside 400 ms was reclassified as the home
             * gesture, ejecting the owner to the face mid-adjustment. The
             * shade names its own exits (BOOT, centre up, tap outside). */
            if (iev.kind == JR_INPUT_TAP && !jr_display_choices_active() &&
                !s_ui_shade_open &&
                jr_display_nav_overlay() != JR_DISPLAY_OVERLAY_SHADE) {
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
                    /* WATCH is deliberately blank: a clock face is the one
                     * screen that needs no caption to identify it, and the
                     * word sat directly under the hands as pure clutter.
                     * caption_set("") routes to caption_clear(). */
                    static const char *const mode_name[JR_DISPLAY_SPACE_COUNT] =
                        { "JARVIS", "", "WEATHER", "STATUS", "DESK", "ACTIVITY" };
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
                        /* TWO DOORS, ONE SHADE. A finger opens it through the
                         * nav overlay; /api/ui/shade opens it through the
                         * shell bit (s_ui_shade_open). The re-derivation
                         * after this switch used to read only the nav door,
                         * so ONE tap on an HTTP-opened shade — a volume step —
                         * re-derived "closed" and shut it. Each door now
                         * closes its own. */
                        if (jr_display_nav_overlay() ==
                            JR_DISPLAY_OVERLAY_SHADE) {
                            jr_display_nav_up();
                            s_ui_shade_open = false;
                            jr_display_caption_set("CONTROLS CLOSED");
                        } else if (s_ui_shade_open) {
                            s_ui_shade_open = false;
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
                    if (jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_SHADE) {
                        s_ui_shade_open = true;
                    }
                    continue;
                }
                /* THE GLASS CAN TALK ABOUT WHAT IT SHOWS. A tap on an open
                 * sheet asks the assistant to say it aloud — the weather
                 * brief, the time and date, a recap of what we did. Screens
                 * are not just pictures; they are the shortest possible
                 * question. A text turn, so privacy (the mic) is untouched. */
                if (jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_DETAIL) {
                    const char *ask = screen_voice_prompt(jr_display_nav_space());
                    if (ask != NULL) {
                        jr_display_caption_set("ONE MOMENT");
                        handle_say(ask);
                        ESP_LOGI(TAG, "ui: sheet tap speaks the screen");
                        continue;
                    }
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
        /* A COMPANION LEASE MUST BE ABLE TO HEAR YOU.
         *
         * Taking an operator lease already pauses the Gemini session, which
         * drops the phase to IDLE — and IDLE is not in phase_allows_capture,
         * so the codec was never read and every mic tap stayed empty. The
         * companion was structurally deaf: you could talk to the device all
         * you liked during companion mode and nothing was listening, which is
         * the opposite of what companion mode is for.
         *
         * The read follows the audio-diag lane, not the voice lane: it fills
         * the WAV taps and then jumps straight to capture_complete, so no
         * frame reaches VAD, the orchestrator, or the Gemini uplink. The
         * leaseholder pulls /api/audio/tap.wav; Gemini stays paused and hears
         * nothing.
         *
         * PRIVACY IS STILL ABSOLUTE. This is gated on the mic being live by
         * the owner's own hand. A lease is a remote grant, and a remote grant
         * must never be able to switch a microphone on — holding the glass or
         * flipping the device face-down outranks any leaseholder. */
        const bool companion_listen =
            operator_mode_active((uint32_t)now) &&
            !atomic_load(&s_voice_privacy_paused) && !s_flip_muted;
        if ((s_app.io.capturing && phase_allows_capture) || audio_diag_active ||
            companion_listen) {
            int n = jr_audio_source_read(&s_app.mic, mic_frame, VOICE_FRAME_SAMPLES);
            if (n > 0) {
                s_app.mic_level = jr_dsp_rms(mic_frame, (size_t)n);
                if (audio_diag_active || companion_listen) {
                    read_paced = true;
                    goto capture_complete;
                }
                /* PRIVACY IS THE MICROPHONE, NOT A FLAG ON THE RE-ARM. The
                 * flag used to gate only the always-ready re-arm, so any
                 * other road into a live session — a text turn from the desk
                 * route, a companion, a reconnect — brought the uplink up
                 * under a gold ring that said MUTED. Seen by the owner: "it's
                 * on privacy mode but look, it's listening to me". Here the
                 * frame is read (the codec keeps its pace) and dropped: no
                 * VAD, no uplink, no level, whatever the session is doing.
                 * Text turns still work with the mic shut.
                 *
                 * The frame is ZEROED rather than dropped: with no frames at
                 * all the server's turn detection has no silence to conclude a
                 * text turn on, and a desk turn under privacy took 47 s to
                 * answer. Zeros are a quiet room, not the room. */
                const bool mic_gated = atomic_load(&s_voice_privacy_paused);
                if (mic_gated) {
                    memset(mic_frame, 0, (size_t)n * sizeof(jr_pcm_t));
                    s_app.mic_level = 0.0f;
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
                float vad_rms = mic_gated ? 0.0f
                                : atomic_load(&s_vad_use_clean)
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
                        s_utt_start_ms = (uint32_t)now;
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
                    if (jr_orch_phase(&s_app.orch) == JR_ST_LISTENING &&
                        !s_pending_text_set &&
                        (uint32_t)now - s_utt_start_ms >= UTT_MIN_MS) {
                        atomic_store(&s_reply_deadline_ms,
                                     (uint32_t)now + UTT_REPLY_WAIT_MS);
                    }
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
        weather_maybe_fetch((uint32_t)now);

        /* 5) reflect phase + live amplitude on the face. With the HUD
         * presenter, present() is an atomic mailbox store, so feeding it
         * every loop is free; with the logging stub, amp stays 0 and the
         * call still fires only on face change (no log spam). */
        jr_face_t f = phase_to_face(jr_orch_phase(&s_app.orch));
        if (f == JR_FACE_IDLE) {
            /* Idle has three truths the face used to hide: muted is gold and
             * still; resting (WHISPER/DREAM) is a breathing slit; only a
             * live, awake idle keeps the open cyan reactor. */
            const uint8_t mood = atomic_load(&s_mood_id);
            if (atomic_load(&s_voice_privacy_paused)) {
                f = JR_FACE_MUTED;
            } else if (mood == (uint8_t)JR_MOOD_WHISPER ||
                       mood == (uint8_t)JR_MOOD_DREAM) {
                f = JR_FACE_RESTING;
            }
        }
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
static char s_sys_instr[2560];
static void compose_system_instruction(void)
{
    static const char kBase[] =
        "RESPOND ONLY IN ENGLISH (UNITED STATES). YOU MUST RESPOND UNMISTAKABLY "
        "IN ENGLISH, EVEN IF THE INPUT AUDIO OR AMBIENT SPEECH IS IN ANOTHER "
        "LANGUAGE. Never switch languages, mix tongues, or use foreign phrases "
        "unless Sir explicitly asks for another language.\n\n"
        "You are J.A.R.V.I.S., the owner's personal AI, on a small voice "
        "device on their desk. Your voice is that of a calm British butler; "
        "your scope is not a household. The owner builds software and "
        "hardware and runs a business, and you help with anything they ask: "
        "engineering, code, product, business, research, the news, the "
        "world, games, this device. Address the owner as \"Sir.\"\n\n"
        "Never refuse a reasonable request and never say a subject is "
        "outside your role. If something is impossible from this device, "
        "say so in one sentence and give the nearest thing you can do. Warn "
        "once if you must, then help. If speech is unclear, ask one short "
        "question rather than guessing or going quiet.\n\n"
        "Style: dry wit, measured calm, understated. Humor only when composure "
        "meets chaos, never forced jokes or catchphrases. Same tone for crisis "
        "and routine; urgency shortens sentences, never volume. Radical "
        "honesty and genuine loyalty.\n\n"
        "SPEECH (spoken audio, low latency):\n"
        "- Prefer 1-3 short sentences. Fewer words win. For a long answer, "
        "give the essence first and offer more.\n"
        "- No lists, markdown, stage directions, or thinking-aloud.\n"
        "- No cheerleading filler (\"Sure!\", \"Absolutely!\", \"Happy to help!\").\n"
        "- On completed actions: \"Done.\", \"Very well.\", or one crisp fact, "
        "then stop.\n"
        "- Stay silent only for speech plainly not addressed to you: a "
        "television, music, other people talking to each other. When in "
        "doubt, answer briefly.\n"
        "- Never narrate that you are listening or thinking.\n\n"
        "You exist to help with Sir's life and work, and the Jarvis tools are "
        "how you act: remember what he tells you, recall it later, read and "
        "add to his calendar, keep his work board, search the world. Use them "
        "without being asked twice; never claim a tool succeeded unless its "
        "response says so.\n\n"
        "Capabilities, for when Sir asks what you can do: live web search and "
        "news, weather, Wikipedia, crypto and stock prices, exchange rates, "
        "time zones, translation, research papers, remembering and recalling "
        "Sir's notes, his calendar, his work board, and this device's volume "
        "and brightness.";
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
    cpu_gear_set(CPU_MHZ_LIVE);   /* explicit, so the first switch has a baseline */

    /* The die thermometer, for STATUS. The owner's "the device is hot" had
     * no number behind it; now it has one. */
    {
        temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&tcfg, &s_tsens) != ESP_OK ||
            temperature_sensor_enable(s_tsens) != ESP_OK) {
            s_tsens = NULL;
            ESP_LOGW(TAG, "chip temperature sensor unavailable");
        }
    }

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
    /* Say WHY the mic is shut. Booting muted without announcing it reads as a
     * broken device — the owner says "Jarvis", nothing happens, and there is
     * no way to tell a privacy state from a fault. The gold ring carries the
     * same fact for anyone across the room. */
    /* HOW WE GOT HERE. A boot out of deep sleep is not a fresh boot: the
     * owner lifted or touched a sleeping device, and it should come back the
     * way it went — listening if it was listening. A timer wake is a health
     * check with nobody there: the ladder resumes at DREAM so the glass stays
     * dark and the chip sleeps again in JR_MOOD_SLEEP_MS unless something
     * happens. Everything else is the privacy default. */
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    s_boot_wake_cause = (int)cause;
    const bool from_sleep = s_rtc_sleep_magic == SLEEP_RTC_MAGIC &&
        (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1 ||
         cause == ESP_SLEEP_WAKEUP_TIMER);
    if (from_sleep && cause == ESP_SLEEP_WAKEUP_TIMER) {
        s_mood.still_since_ms =
            (uint32_t)(esp_timer_get_time() / 1000) - JR_MOOD_DREAM_MS;
        jr_display_caption_set("ASLEEP - TAP TO WAKE");
        ESP_LOGI(TAG, "boot complete — timer wake %u, resting on",
                 (unsigned)s_rtc_sleeps);
    } else if (from_sleep && s_rtc_was_listening) {
        atomic_store(&s_voice_privacy_paused, false);
        atomic_store(&s_voice_control_request, VOICE_CONTROL_ARM);
        jr_display_caption_set("LISTENING");
        ESP_LOGI(TAG, "boot complete — woke by %s, listening again",
                 wake_cause_name((int)cause));
    } else {
        jr_display_caption_set("MUTED - HOLD TO LISTEN");
        ESP_LOGI(TAG, "boot complete — starting MUTED (privacy default)%s",
                 from_sleep ? ", woke from sleep" : "");
    }
}
