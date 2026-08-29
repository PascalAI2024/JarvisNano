/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_transport/gemini_live.h — the L2 Gemini Live transport.
 *
 * Three host-testable units, all ESP-IDF-free (they depend only on jr_ports
 * headers, jr_core types, libc, and the vendored cJSON — the SAME source builds
 * the host suite and the device build):
 *
 *   1. BUILDERS  — setup / activityStart / activityEnd / audioStreamEnd / audio.
 *                  Shapes harvested from firmware `gl_send_setup` (:1522) and
 *                  verified against docs/reference/gemini-live-api-v5.md.
 *   2. FRAMER    — the would-block-as-backpressure send path. A would-block /
 *                  0-byte / partial send buffers-or-drops (drop-newest, bounded)
 *                  and KEEPS the connection. It NEVER aborts (v4's worst bug).
 *   3. PARSER    — inbound server JSON -> typed events (server-audio, turn signals,
 *                  goAway{seconds}, sessionResumptionUpdate, toolCall/cancel, error).
 *
 * Upper seam: this impl satisfies the provider-neutral `jr_realtime_voice_client_t`
 * port (jr_ports/realtime_voice_client.h). Lower seam: it drives a byte-level
 * `jr_ws_transport_t` (jr_ports/ws_transport.h) — a device adapter on hardware,
 * a fake on host. Reconnect is NOT here; it is an L3 transition (spec §5.1).
 */
#ifndef JR_TRANSPORT_GEMINI_LIVE_H
#define JR_TRANSPORT_GEMINI_LIVE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "jr_ports/jr_types.h"
#include "jr_ports/clock.h"
#include "jr_ports/ws_transport.h"
#include "jr_ports/realtime_voice_client.h"
#include "jr_core/monitors.h"   /* the Run-2 monitors this transport feeds */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== *
 *  Model pin (docs/reference/gemini-live-api-v5.md §2, ADR 2026-07-07)      *
 * ======================================================================== */
/* Primary: real + current flagship Live/native-audio preview (released
 * 2026-03-11, in Google's own get-started samples). Retest before device
 * commit — v4 saw a stale 404 on this string (unresolved); fall back if so. */
#define JR_GEMINI_MODEL_PRIMARY  "models/gemini-3.1-flash-live-preview"
/* Fallback: successor to the confirmed-dead -09-2025 string. */
#define JR_GEMINI_MODEL_FALLBACK "models/gemini-2.5-flash-native-audio-preview-12-2025"

/* Uplink is 16 kHz PCM16 mono; the model's downlink is a fixed 24 kHz. */
#define JR_GEMINI_TX_RATE 16000u
#define JR_GEMINI_RX_RATE 24000u

/* ======================================================================== *
 *  BUILDERS                                                                 *
 * ======================================================================== */

/* Declared-parameter types. STRING is 0 so a zero-initialized parameter slot is
 * a plain string — the legacy default. STRING_ARRAY is the one that forced this
 * struct to grow: an `options[]` argument (tap-to-answer choice arcs) cannot be
 * expressed by the single-string form below. */
typedef enum {
    JR_GEMINI_PT_STRING = 0,
    JR_GEMINI_PT_STRING_ARRAY,   /* emits "type":"array" + "items":{"type":"string"} */
    JR_GEMINI_PT_INTEGER,
    JR_GEMINI_PT_BOOLEAN,
} jr_gemini_param_type_t;

/* Fixed cap per declaration. Declarations live in .rodata (flash) on device, so
 * this costs no internal RAM — but keep it small anyway: the whole point of a
 * fixed array is that nothing in the declaration path ever mallocs. */
#ifndef JR_GEMINI_MAX_FN_PARAMS
#define JR_GEMINI_MAX_FN_PARAMS 4
#endif

/* NO SILENT TRUNCATION. An over-long declaration used to clamp param_count to
 * JR_GEMINI_MAX_FN_PARAMS and ship a schema missing a property — invisible
 * until Gemini mis-calls the tool at runtime. Two guards now:
 *   1. compile time — declare param_count through this macro and a 5-property
 *      table fails to build;
 *   2. run time     — jr_gemini_build_setup() REFUSES the whole setup (returns
 *      NULL) rather than emitting a lying schema. There is no ESP-IDF logger in
 *      this layer, so "loud" means "fails hard at the seam the caller checks".
 */
#define JR_GEMINI_PARAM_COUNT(n)                                               \
    ((size_t)(n) +                                                             \
     0u * sizeof(char[((n) <= JR_GEMINI_MAX_FN_PARAMS) ? 1 : -1]))

typedef struct {
    const char            *name;        /* NULL => slot skipped                 */
    jr_gemini_param_type_t type;        /* default (0) == STRING                */
    const char            *description; /* NULL => ""                           */
    bool                   required;    /* listed in the schema "required" array*/
} jr_gemini_fn_param_t;

/* A single declared function tool.
 *
 * TWO WAYS to declare arguments, and they are additive:
 *   (a) LEGACY single string — set arg_name/arg_desc. Emitted exactly as before
 *       (one "string" property + a one-element "required" array). Untouched so
 *       every existing tool in main.c keeps its byte-identical schema.
 *   (b) TYPED params[] — up to JR_GEMINI_MAX_FN_PARAMS properties of mixed type,
 *       including STRING_ARRAY. Zero-initialized (param_count == 0) means "none",
 *       so a legacy positional or designated initializer still compiles and
 *       still emits the legacy shape.
 * A declaration may use either, or both (legacy property is emitted first). */
typedef struct {
    const char *name;
    const char *description;
    const char *arg_name;   /* NULL => no legacy single-string arg */
    const char *arg_desc;
    jr_gemini_fn_param_t params[JR_GEMINI_MAX_FN_PARAMS];
    size_t               param_count;
} jr_gemini_fn_decl_t;

/* ---- ask_user: the tap-to-answer choice-arc tool (STATE-05/STATE-06) ------ *
 * The model asks a short question and offers a few short options; the device
 * draws them as arcs, the user taps one, and the answer returns as a
 * functionResponse built by jr_gemini_build_ask_user_response().              */
#define JR_GEMINI_ASK_USER_TOOL        "ask_user"
#define JR_GEMINI_ASK_USER_ARG_QUESTION "question"
#define JR_GEMINI_ASK_USER_ARG_OPTIONS  "options"
/* The UI can only draw three arcs in the free r215-239 band. */
#define JR_GEMINI_ASK_USER_MAX_CHOICES 3

/* Drop-in initializer so the owner can splice ask_user into its `static const`
 * declaration table without a runtime constructor. */
#define JR_GEMINI_ASK_USER_DECL {                                              \
    .name = JR_GEMINI_ASK_USER_TOOL,                                           \
    .description =                                                             \
        "Ask Pascal a short multiple-choice question and wait for the tap. "    \
        "Use at most three options; keep each option under 16 characters.",     \
    .params = {                                                                \
        { .name = JR_GEMINI_ASK_USER_ARG_QUESTION,                             \
          .type = JR_GEMINI_PT_STRING,                                         \
          .description = "The question to show, at most 40 characters.",        \
          .required = true },                                                  \
        { .name = JR_GEMINI_ASK_USER_ARG_OPTIONS,                              \
          .type = JR_GEMINI_PT_STRING_ARRAY,                                   \
          .description =                                                       \
              "Two or three short answer choices, each under 16 characters.",   \
          .required = true },                                                  \
    },                                                                         \
    .param_count = JR_GEMINI_PARAM_COUNT(2),                                   \
}

/* ---- the OWNED ask snapshot (the 120 s lifetime problem) ----------------- *
 * The parse tree dies the instant jr_gemini_pump_rx() returns from the rich
 * callback, but the Asking state holds the question and the options for up to
 * JR_ASK_TIMEOUT_MS (120 s) while a human decides. Every pointer handed out by
 * jr_gemini_tool_arg_string() / _string_array() and every `const char *` on the
 * event ALIASES that tree, so storing one and reading it later is a
 * use-after-free.
 *
 * jr_gemini_event_to_ask() removes the hazard instead of documenting it: it
 * copies into caller-provided fixed storage. No malloc (internal RAM is
 * scarce), ~140 B, lives inside the caller's existing struct. A consumer that
 * holds a jr_gemini_ask_t is safe for the whole 120 s and beyond; a consumer
 * that holds a borrowed pointer is, and always was, wrong.
 *
 * Caps are the UI's real limits (the declaration asks the model for <=40-char
 * questions and <16-char options), so truncation is a bad model response, not
 * a normal path — hence the explicit `truncated` flag rather than silence. */
#define JR_GEMINI_ASK_CALL_ID_CAP   64
#define JR_GEMINI_ASK_QUESTION_CAP  48
#define JR_GEMINI_ASK_OPTION_CAP    20

typedef struct {
    char     call_id[JR_GEMINI_ASK_CALL_ID_CAP];   /* always NUL-terminated */
    char     question[JR_GEMINI_ASK_QUESTION_CAP];
    char     options[JR_GEMINI_ASK_USER_MAX_CHOICES][JR_GEMINI_ASK_OPTION_CAP];
    uint32_t call_id_hash;   /* == ev->call_id; the core's uint32 handle     */
    uint8_t  count;          /* options actually copied (0..MAX_CHOICES)     */
    bool     truncated;      /* some field or the option list was clipped    */
    bool     answered;       /* OWNED BY THE CALLER, never set here: marks
                              * that a functionResponse for call_id has been
                              * accepted by the transport. An eviction path
                              * that finds this false must answer (empty) on
                              * the spot — every ask_user call MUST receive
                              * exactly one response or the model blocks.    */
} jr_gemini_ask_t;

/* (the snapshot function is declared below, next to the event type) */

typedef struct {
    const char *url;                /* wss endpoint (contains the API key at
                                     * runtime — NEVER hardcode in-repo); passed
                                     * to ws.connect(). NULL => "" for host tests*/
    const char *model;              /* NULL => JR_GEMINI_MODEL_PRIMARY        */
    const char *system_instruction; /* NULL => omit systemInstruction         */
    const char *thinking_level;     /* NULL => "low"; e.g. "minimal"/"low"    */
    const char *voice_name;         /* prebuilt voice (e.g. "Charon"); NULL => omit
                                     * speechConfig.voiceConfig, model default   */
    const char *language_code;      /* BCP-47 output-language anchor (e.g."en-US");
                                     * NULL => omit. Stops native-audio language
                                     * drift when paired with a strong system
                                     * instruction.                              */
    jr_vad_mode_t vad_mode;         /* MANUAL => PTT (disabled+NO_INTERRUPTION)*/
    bool  google_search;            /* include the googleSearch tool          */
    bool  input_transcription;      /* include inputAudioTranscription {}      */
    bool  output_transcription;     /* include outputAudioTranscription {} — the
                                     * server then streams a text transcript of
                                     * the model's own speech (proves output
                                     * language + gives a readable log).         */
    bool  proactive_audio;          /* proactivity.proactiveAudio: let the model
                                     * stay silent on ambient/out-of-context
                                     * audio instead of inventing a reply — kills
                                     * phantom self-triggered turns.             */
    const jr_gemini_fn_decl_t *fns; /* functionDeclarations (may be NULL)      */
    size_t fn_count;
} jr_gemini_config_t;

/* Build the setup message. Returns a malloc'd NUL-terminated JSON string the
 * caller frees, or NULL on OOM. thinkingLevel is nested under
 * generationConfig.thinkingConfig (NOT flat). Manual mode sets
 * automaticActivityDetection.disabled=true + activityHandling=NO_INTERRUPTION.
 * NEVER contains audioStreamEnd. */
char *jr_gemini_build_setup(const jr_gemini_config_t *cfg);

/* Small realtimeInput control frames (malloc'd; caller frees). */
char *jr_gemini_build_activity_start(void);   /* {"realtimeInput":{"activityStart":{}}} */
char *jr_gemini_build_activity_end(void);     /* {"realtimeInput":{"activityEnd":{}}}   */
char *jr_gemini_build_audio_stream_end(void); /* auto-VAD ONLY — never in manual mode   */
char *jr_gemini_build_text_turn(const char *text); /* clientContent user turn, turnComplete */
char *jr_gemini_build_tool_response(const char *call_id, const char *name,
                                    const char *response_json);

/* The answered-question functionResponse. Thin wrapper over
 * jr_gemini_build_tool_response() with name == "ask_user" and a
 * {"answer":"<chosen>"} payload, so an answered choice arc goes back over the
 * SAME toolResponse path every other tool already uses. `answer` is JSON-escaped
 * (a quote or backslash in an option cannot corrupt the frame); NULL => "".
 * Returns malloc'd JSON the caller frees, or NULL on OOM. */
char *jr_gemini_build_ask_user_response(const char *call_id, const char *answer);

/* Encode a PCM16 mono frame as a realtimeInput audio blob. `samples` is the
 * count of int16 samples; `rate` is the mimeType rate (JR_GEMINI_TX_RATE).
 * Returns malloc'd JSON, or NULL on OOM. */
char *jr_gemini_build_audio_chunk(const jr_pcm_t *pcm, size_t samples, uint32_t rate);

/* ======================================================================== *
 *  PARSER — inbound server frame -> typed events                           *
 * ======================================================================== */

/* Provider-neutral error classification (mirrors jr_core's jr_error_kind_t but
 * lives in the transport layer; the L3 adapter maps one to the other). */
typedef enum {
    JR_GEMINI_ERRK_QUOTA = 0,  /* 429 / RESOURCE_EXHAUSTED / "quota"          */
    JR_GEMINI_ERRK_AUTH,       /* 401 / 403 / UNAUTHENTICATED / PERMISSION    */
    JR_GEMINI_ERRK_PROTOCOL,   /* 400 / INVALID_ARGUMENT                      */
    JR_GEMINI_ERRK_TRANSIENT,  /* the default involuntary-death kind          */
    JR_GEMINI_ERRK_UNKNOWN,
} jr_gemini_error_kind_t;

typedef enum {
    JR_GEV_SETUP_COMPLETE = 0,
    JR_GEV_AUDIO_CHUNK,          /* pcm / pcm_len / sample_rate (decoded)      */
    JR_GEV_TEXT,                 /* text                                       */
    JR_GEV_INTERRUPTED,          /* serverContent.interrupted == true          */
    JR_GEV_TURN_COMPLETE,        /* serverContent.turnComplete == true         */
    JR_GEV_GENERATION_COMPLETE,  /* serverContent.generationComplete == true   */
    JR_GEV_TOOL_CALL,            /* call_id / tool_name / tool_args            */
    JR_GEV_TOOL_CANCEL,          /* call_id (first cancelled id)               */
    JR_GEV_GO_AWAY,              /* go_away_seconds (protobuf Duration decoded)*/
    JR_GEV_RESUMPTION_UPDATE,    /* heartbeat: resumable + resumption_token    */
    JR_GEV_ERROR,                /* error_kind / code / message                */
    JR_GEV_UNKNOWN,              /* unrecognized top-level frame               */
} jr_gemini_event_kind_t;

typedef struct {
    jr_gemini_event_kind_t kind;

    /* AUDIO_CHUNK: decoded 24 kHz mono PCM16. Owned by the jr_gemini_parse_t;
     * valid until jr_gemini_parse_free(). */
    jr_pcm_t *pcm;
    size_t    pcm_len;      /* sample count */
    uint32_t  sample_rate;

    /* TEXT */
    const char *text;       /* points into the retained cJSON tree */

    /* TOOL_CALL / TOOL_CANCEL */
    uint32_t    call_id;
    const char *call_id_text;  /* original Gemini id; required in toolResponse */
    const char *tool_name;
    const char *tool_args;     /* printed JSON of args; owned, freed in _free  */
    /* The retained `args` cJSON node (opaque). Valid until jr_gemini_parse_free().
     * Feeds jr_gemini_tool_arg_string() / _string_array() so a caller can read a
     * typed argument — notably ask_user's options[] — WITHOUT re-parsing
     * tool_args. NULL when the call carried no args. */
    const void *tool_args_node;

    /* GO_AWAY */
    uint32_t go_away_seconds;

    /* RESUMPTION_UPDATE: token is nonzero ONLY when resumable && handle set
     * (spec: only usable handles are stored). */
    bool        resumable;
    uint32_t    resumption_token;
    const char *handle;

    /* ERROR */
    jr_gemini_error_kind_t error_kind;
    int         code;
    const char *message;
} jr_gemini_event_t;

/* A frame may demux into several events (e.g. modelTurn audio + turnComplete). */
#define JR_GEMINI_MAX_EVENTS 8

typedef struct {
    jr_gemini_event_t events[JR_GEMINI_MAX_EVENTS];
    size_t            count;
    void             *_root;   /* retained cJSON tree (internal; do not touch) */
} jr_gemini_parse_t;

/* Parse ONE inbound server frame. Returns true if `json` was valid JSON (even
 * an unknown frame yields one JR_GEV_UNKNOWN event and true); false only on a
 * parse failure. Call jr_gemini_parse_free() when done to release the retained
 * tree + any decoded audio. Uses cJSON — never a hand-rolled untrusted parser. */
bool jr_gemini_parse(const char *json, jr_gemini_parse_t *out);
void jr_gemini_parse_free(jr_gemini_parse_t *out);

/* ---- typed argument readers for a JR_GEV_TOOL_CALL event ------------------ *
 * Both read straight out of the retained parse tree: NO allocation, NO copy.
 * The returned pointers alias that tree and die at jr_gemini_parse_free() —
 * which jr_gemini_pump_rx() calls the moment the rich callback returns.
 *
 * ⚠ THESE ARE STRICTLY IN-CALLBACK READS. Storing one and reading it later is a
 * use-after-free. For ask_user — whose question and options must survive a
 * 120 s human decision — do NOT use these: call jr_gemini_event_to_ask() and
 * keep the owned snapshot instead.                                            */

/* args[key] as a string, or NULL if absent / not a string. */
const char *jr_gemini_tool_arg_string(const jr_gemini_event_t *ev, const char *key);

/* args[key] as an array of strings. Writes at most `max` element pointers into
 * `out` and returns how many were written. Non-string elements are skipped.
 * Returns 0 (and touches nothing) if absent, not an array, or on a bad arg. */
size_t jr_gemini_tool_arg_string_array(const jr_gemini_event_t *ev, const char *key,
                                       const char **out, size_t max);

/* Snapshot an ask_user JR_GEV_TOOL_CALL event into caller-owned fixed storage
 * (see jr_gemini_ask_t above). Returns true only for a usable ask: an ask_user
 * tool call carrying a non-empty question and at least one non-empty option.
 * `out` is fully zeroed first, so a false return leaves a clean inert struct
 * (count == 0, empty strings) rather than partial garbage. Copies everything —
 * NOTHING in `out` aliases the parse tree, so it is valid after
 * jr_gemini_parse_free() and for the entire 120 s Asking window.
 * Allocation-free, no float. */
bool jr_gemini_event_to_ask(const jr_gemini_event_t *ev, jr_gemini_ask_t *out);

/* ======================================================================== *
 *  CLIENT — the RealtimeVoiceClient impl over a ws_transport (framer + pump)*
 * ======================================================================== */

/* Bounded outbound ring. Payloads are allocated per queued frame so device
 * builds can place them in PSRAM instead of permanently pinning ~32 KiB of
 * internal SRAM inside the client. Depth 16 covers about one second of the
 * two-frame/64 ms microphone cadence while remaining strictly bounded. */
#ifndef JR_GEMINI_TXQ_DEPTH
#define JR_GEMINI_TXQ_DEPTH 16
#endif
#ifndef JR_GEMINI_TXQ_SLOT
#define JR_GEMINI_TXQ_SLOT 4096
#endif
/* Max inbound WS text frame the pump reads at once (a 24 kHz audio chunk in
 * base64 is the large case). */
#ifndef JR_GEMINI_RX_MAX
#define JR_GEMINI_RX_MAX (96 * 1024)
#endif

typedef struct {
    char  *buf;
    size_t len;
    size_t off;   /* bytes already written (partial-frame resume) */
} jr_gemini_txframe_t;

/* Independent uplink/downlink liveness so a dead-uplink/live-downlink
 * split-brain is observable (spec §5.3). */
typedef struct {
    uint64_t last_rx_ms;              /* downlink: last inbound frame time     */
    uint32_t consecutive_tx_failures; /* uplink: soft-fails in a row (not w-b) */
    uint32_t tx_would_block;          /* backpressure count (NOT a failure)    */
    uint32_t tx_drops;                /* frames dropped-newest under overflow  */
    uint32_t rx_parse_errors;         /* complete frames rejected as bad JSON  */
    uint32_t rx_alloc_failures;       /* persistent receive-buffer OOM count   */
    bool     socket_open;
} jr_gemini_liveness_t;

/* Rich event out-channel (carries the full parser contract). Distinct from the
 * neutral jr_rvc_event_cb, which the client ALSO drives for the port. */
typedef void (*jr_gemini_event_cb)(void *user, const jr_gemini_event_t *ev);

typedef struct jr_gemini_client {
    jr_ws_transport_t  ws;
    jr_clock_t         clk;
    jr_gemini_config_t cfg;

    /* outbound framer state */
    jr_gemini_txframe_t txq[JR_GEMINI_TXQ_DEPTH];
    size_t head, tail, depth;

    jr_gemini_liveness_t live;

    /* Persistent receive scratch. Allocated lazily so the 96 KiB maximum lives
     * in PSRAM through malloc on device, never on the owner task's stack. */
    char   *rx_buf;
    size_t  rx_cap;

    /* optional injected monitors this transport feeds (may be NULL) */
    jr_keepalive_monitor_t   *keepalive;
    jr_dead_uplink_monitor_t *dead_uplink;

    /* event out-channels */
    jr_gemini_event_cb rich_cb;  void *rich_user;
    jr_rvc_event_cb    rvc_cb;   void *rvc_user;
} jr_gemini_client_t;

/* Initialize a client over a byte transport + clock. `cfg` is copied by value
 * (its string pointers must outlive the client). */
void jr_gemini_client_init(jr_gemini_client_t *c, jr_ws_transport_t ws,
                           jr_clock_t clk, const jr_gemini_config_t *cfg);

/* Release persistent client scratch. Device firmware keeps one client for the
 * process lifetime; host tests and short-lived clients should call this. */
void jr_gemini_client_deinit(jr_gemini_client_t *c);

/* Wire the injected monitors this transport feeds (optional). */
void jr_gemini_client_set_monitors(jr_gemini_client_t *c,
                                   jr_keepalive_monitor_t *ka,
                                   jr_dead_uplink_monitor_t *du);

/* Register the rich event out-channel. */
void jr_gemini_client_set_event_cb(jr_gemini_client_t *c,
                                   jr_gemini_event_cb cb, void *user);

/* A provider-neutral RealtimeVoiceClient VIEW of this client (upper seam). The
 * returned struct's ctx points at `c`; it stays valid while `c` does. */
jr_realtime_voice_client_t jr_gemini_client_as_rvc(jr_gemini_client_t *c);

/* THE FRAMER (the single most important correctness win): send one already-built
 * frame. Returns:
 *   JR_OK             — fully sent now
 *   JR_ERR_WOULD_BLOCK— backpressure: buffered or drop-newest; connection KEPT
 *   JR_ERR_CLOSED     — socket reported closed: the death path
 * A soft failure (socket still open) is counted for the DeadUplinkMonitor and
 * reported as JR_ERR_WOULD_BLOCK (never a single-send teardown). Never aborts. */
jr_err_t jr_gemini_send_frame(jr_gemini_client_t *c, const char *json, size_t len);

/* Try to drain buffered frames onto a freed socket (partial-write aware). Call
 * from the owner's pump. Returns JR_OK / JR_ERR_WOULD_BLOCK / JR_ERR_CLOSED. */
jr_err_t jr_gemini_flush(jr_gemini_client_t *c);

/* Drop any buffered outbound frames at a session boundary. A reconnect must
 * begin with Setup as its first frame; carrying audio/control from the dead
 * socket into the new one corrupts the Gemini protocol ordering. */
void jr_gemini_reset_tx(jr_gemini_client_t *c);

/* Pump one inbound frame (if any): recv -> parse -> emit events + liveness tick.
 * Returns true if a frame was processed. */
bool jr_gemini_pump_rx(jr_gemini_client_t *c);

/* Depth of the outbound buffer (bounded-memory assertions in tests). */
static inline size_t jr_gemini_txq_depth(const jr_gemini_client_t *c) { return c->depth; }

#ifdef __cplusplus
}
#endif

#endif /* JR_TRANSPORT_GEMINI_LIVE_H */
