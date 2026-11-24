/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app.h — the composition root's internal seam.
 *
 * main.c grew to nine thousand lines doing four jobs; this header is what
 * the split files share: the types, the caps, and every global or function
 * that crosses a file. Nothing here is public API — only the sources beside
 * it (the .c files under main). Names are the ones main.c always had; a
 * symbol lost `static` only because it is now used from more than one file.
 */
#pragma once

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
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "driver/temperature_sensor.h"
#include "esp_pm.h"
#include "cJSON.h"

#include "jr_ports/ports.h"
#include "jr_display/jr_display.h"
#include "jr_display/hud_render.h"
#include "jr_core/session.h"
#include "jr_core/mood.h"
#include "jr_core/glance.h"
#include "jr_core/orchestrator.h"
#include "jr_core/snapshot.h"
#include "jr_core/turn_policy.h"
#include "jr_core/utt_watch.h"
#include "jr_core/echo_gate.h"
#include "jr_core/tool_policy.h"
#include "jr_core/pace.h"
#include "jr_core/timer.h"
#include "jr_core/notes.h"
#include "jr_core/desk_mode.h"
#include "jr_tools/jr_weather.h"
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


/* ---- shared types and caps (moved verbatim from main.c) ---------------- */

#define TOOL_ID_CAP          96
#define TOOL_NAME_CAP        64
#define TOOL_ARGS_CAP        2048
#define VOICE_ALWAYS_READY   1

/* The catalog lives in main.c; this literal is what the other files see.
 * main.c static-asserts it against the real array, so adding a tool
 * without bumping this is a compile error, not a blank petal. */
#define DEVICE_TOOL_DECL_COUNT 27U

/* Render cadence per rung of the rest ladder (frames per second). 24 is the
 * measured CO5300 ceiling; 12 keeps AMBIENT's dimmed ring fluid under a
 * finger; 6 breathes the WHISPER slit; 3 is a dark DREAM face and a caption
 * that can still change. Measured in docs/reference/power-modes.md. */
#define RENDER_FPS_LIVE     24U
#define RENDER_FPS_AMBIENT  12U
#define RENDER_FPS_WHISPER   6U
#define RENDER_FPS_DREAM     3U
/* Until the voice loop's first cadence tick. app_main runs at priority 1 on
 * the render task's core, and a frame that overruns its period no longer
 * sleeps (N14.4), so a watch dial on the glass during boot (the glance after
 * a lift) left boot a few percent of the core: 69 s to "boot complete" and a
 * dead touch for all of it (2026-11-10; the render task measured 96.5 % of
 * core 0 on a dial). At 6 the costliest dial leaves boot more than half. */
#define RENDER_FPS_BOOT      6U
#define LOCAL_TOOL_RESULT_CAP 4U
#define LOCAL_TOOL_ERROR_CAP  576U
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

#define LOGRING_CAP (128U * 1024U)

typedef struct {
    jr_event_t ev;
    char call_id_text[TOOL_ID_CAP];
    char tool_name[TOOL_NAME_CAP];
    char tool_args[TOOL_ARGS_CAP];
} inbound_event_t;

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

typedef enum {
    VOICE_CONTROL_NONE = 0,
    VOICE_CONTROL_ARM,       /* explicit physical unmute only */
    VOICE_CONTROL_RESUME,    /* resume only when privacy permits */
    VOICE_CONTROL_DISARM,    /* deliberate privacy mute */
    VOICE_CONTROL_PAUSE,     /* operational stop; privacy unchanged */
} voice_control_request_t;

#define SLEEP_TIMER_WAKE_S     (4U * 3600U)
#define SLEEP_MIN_UPTIME_MS    180000U   /* three minutes of reachability */
#define SLEEP_RTC_MAGIC        0x534C5031u
#define SLEEP_ARMED_LIFT       1U
#define SLEEP_ARMED_TOUCH      2U
#define SLEEP_LIFT_ARM_FAILED  8U    /* the WoM write failed              */
#define SLEEP_LIFT_LINE_HIGH   16U   /* INT1 already high, wake skipped    */
#define CPU_MHZ_LIVE   240
#define CPU_MHZ_REST   160
#define BATTERY_SAVER_PCT 20U
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

#define GLANCE_MS 8000u

typedef enum {
    TOOL_CONSENT_ALLOW = 0,
    TOOL_CONSENT_DENY,
    TOOL_CONSENT_TIMEOUT,
    TOOL_CONSENT_CANCEL,
} tool_consent_outcome_t;

typedef struct {
    bool ok;
    const char *reason;
    const esp_partition_t *running;
    const esp_partition_t *target;
    uint8_t active_slot;
    uint8_t target_slot;
} ota_preflight_t;

typedef enum {
    OPERATOR_OWNER_NONE = 0,
    OPERATOR_OWNER_CODEX,
    OPERATOR_OWNER_ZEROCHAT,
} operator_mode_owner_t;

/* ---- owned by main.c — composition root, the voice task, presentation ---- */

extern const jr_gemini_fn_decl_t s_device_tool_fns[];
extern device_tool_diag_t s_tool_diag;
extern local_tool_result_t s_local_tool_results[LOCAL_TOOL_RESULT_CAP];
extern size_t s_local_tool_head;
extern size_t s_local_tool_count;
extern jr_tool_result_t *s_tool_poll_result;
extern pending_tool_consent_t s_tool_consent;
extern _Atomic uint32_t s_sim_touch;
extern _Atomic uint32_t s_debug_choices_req;
extern _Atomic uint32_t s_sim_shake;
extern _Atomic uint32_t s_sim_flip;
extern uint32_t s_watch_peek_until_ms;
extern uint32_t s_hold_start_ms;
extern _Atomic uint32_t s_pairing_claim_until_ms;
extern _Atomic uint32_t s_pairing_claim_code;
extern _Atomic uint32_t s_pairing_claim_attempts;
extern int s_out_vol;
/* Interface cues (jr_core/cue.h): the owner's switch, on by default. Set it
 * through ui_sounds_set() so it persists. */
extern _Atomic bool s_ui_sounds;
void ui_sounds_set(bool on);
extern uint8_t s_brightness_cap;
extern _Atomic int s_level_volume_request;
extern _Atomic int s_level_brightness_request;
extern char *s_logring;
extern volatile size_t s_logring_head;
extern volatile size_t s_logring_len;
extern portMUX_TYPE s_logring_mux;
extern _Atomic uint32_t s_operator_lease_until_ms;
extern _Atomic bool s_operator_mode_active;
extern _Atomic uint32_t s_operator_mode_entered_ms;
extern _Atomic int s_operator_mode_owner;
extern _Atomic bool s_ota_active;
extern _Atomic uint32_t s_ota_received_bytes;
extern _Atomic uint32_t s_ota_total_bytes;
extern _Atomic int s_ota_last_error;
extern _Atomic bool s_ota_preflight_blocked;
extern _Atomic bool s_http_ready;
extern _Atomic bool s_demo_req;
extern uint32_t s_demo_start_ms;
extern jr_app_t s_app;
extern _Atomic uint32_t s_last_tx_drop_ms;
extern QueueHandle_t s_say_q;
extern char s_last_said[192];
extern vadlog_entry_t *s_vadlog;
extern _Atomic uint32_t s_vadlog_seq;
extern TaskHandle_t s_voice_task;
extern volatile bool s_voice_task_running;
extern volatile uint32_t s_voice_task_heartbeat_ms;
extern volatile bool s_local_barge_enabled;
extern _Atomic int s_voice_control_request;
extern _Atomic bool s_voice_privacy_paused;
extern jr_mood_state_t s_mood;
extern _Atomic uint8_t s_mood_id;
extern _Atomic uint8_t s_mood_brightness;
extern bool s_mood_rest_disarmed;
extern bool s_flip_muted;
extern _Atomic bool s_vad_use_clean;
extern _Atomic bool s_audio_diag_requested;
extern _Atomic uint32_t s_audio_diag_until_ms;
extern _Atomic bool s_ui_shade_open;
extern _Atomic uint32_t s_touch_events;
extern _Atomic uint32_t s_touch_taps;
extern _Atomic uint32_t s_touch_long_presses;
extern _Atomic uint32_t s_touch_swipes;
extern _Atomic uint32_t s_touch_last_kind;
extern _Atomic uint32_t s_touch_last_x;
extern _Atomic uint32_t s_touch_last_y;
extern _Atomic int s_touch_last_dx;
extern _Atomic int s_touch_last_dy;
extern _Atomic uint32_t s_touch_last_duration_ms;
extern _Atomic bool s_touch_challenge_start_requested;
extern _Atomic bool s_touch_challenge_cancel_requested;
extern _Atomic bool s_touch_challenge_active;
extern _Atomic bool s_touch_challenge_verified;
extern _Atomic uint32_t s_touch_challenge_expected;
extern _Atomic uint32_t s_touch_challenge_correct;
extern _Atomic uint32_t s_touch_challenge_attempts;
extern _Atomic uint32_t s_touch_challenge_wrong;
extern _Atomic uint32_t s_touch_challenge_last_mapped;
extern _Atomic uint32_t s_touch_challenge_last_latency_ms;
extern _Atomic uint32_t s_touch_challenge_restore_ms;
extern SemaphoreHandle_t s_agent_link_lock;
extern agent_link_state_t s_agent_link;
extern uint32_t s_agent_link_revision_hwm;
extern SemaphoreHandle_t s_brain_lock;
extern brain_surface_state_t s_brain_surface;
extern brain_action_event_t s_brain_events[BRAIN_EVENT_CAP];
extern uint32_t s_brain_inbox_seq_hwm;
extern uint32_t s_brain_event_seq;
extern uint32_t s_brain_last_seen_ms;

typedef enum {
    TIMER_REQ_NONE = 0,
    TIMER_REQ_SET,
    TIMER_REQ_CANCEL,
    TIMER_REQ_DISMISS,
    TIMER_REQ_FOCUS,
} timer_req_kind_t;

typedef struct {
    bool active;
    bool ringing;
    bool silent;
    uint16_t rem_s;
    uint16_t total_s;
    uint8_t pct_left;
    uint8_t count;
    char label[JR_TIMER_LABEL_CAP];
} timer_snap_t;

typedef struct {
    timer_req_kind_t kind;
    uint32_t seconds;
    bool silent;
    char label[JR_TIMER_LABEL_CAP];
} timer_debug_req_t;

typedef struct {
    jr_desk_mode_t mode;
    uint32_t rem_s;
    uint8_t pct_left;
    uint8_t sessions_today;
    uint16_t minutes_today;
    jr_breathe_phase_t breathe_phase;
    uint8_t breathe_sec_left;
    bool silent;
    bool dnd_silent;
    char label[JR_DESK_LABEL_CAP];
} desk_mode_snap_t;

typedef struct {
    jr_desk_mode_t mode;
    uint32_t duration_s;             /* 0 = default (focus 25m; on_air open-ended) */
    jr_breathe_pattern_t pattern;
    uint8_t cycles;                  /* 0 = default (4) */
    bool silent;
    char label[JR_DESK_LABEL_CAP];
} desk_mode_req_t;

typedef struct {
    bool active;
    bool running;
    uint32_t elapsed_ms;
    uint8_t lap_count;
    uint32_t laps_ms[JR_STOPWATCH_MAX_LAPS];
} stopwatch_snap_t;

extern jr_timer_bank_t s_timers;
extern portMUX_TYPE s_timer_mux;
extern timer_snap_t s_timer_snap;
extern timer_debug_req_t s_debug_timer_req_data;
extern _Atomic uint32_t s_debug_timer_req;
extern _Atomic bool s_glass_poke_req;

extern jr_desk_state_t s_desk_mode;
extern portMUX_TYPE s_desk_mux;
extern desk_mode_snap_t s_desk_snap;
extern desk_mode_req_t s_desk_req_data;
extern _Atomic uint32_t s_desk_req_pending;

/* Cross-component Wave 2 display contracts (exact signatures from jr_display.h). */
void jr_display_stopwatch_set(bool active, bool running, uint32_t elapsed_ms, uint8_t lap_count);
void jr_display_set_hbm(bool enabled);
bool jr_display_get_hbm(void);
void jr_display_get_te_stats(bool *enabled, uint32_t *waits, uint32_t *timeouts);

/* Cross-component Wave 4 audio/display contracts (exact signatures). */
void jr_audio_get_spectrum(uint8_t out_bands[16]);
void jr_display_spectrum_set(const uint8_t bands[16]);
void jr_display_date_set_ymd(uint16_t year, uint8_t month, uint8_t day);
void jr_display_holo_set(bool enabled);
bool jr_display_holo_get(void);

/* Cross-component Glass 3.5 display contracts (exact signatures from jr_display.h). */
#ifndef JR_DISPLAY_INBOX_MAX
#define JR_DISPLAY_INBOX_MAX 3U
typedef struct {
    uint32_t id;
    bool     done;
    uint8_t  tag;        /* 0=LIFE, 1=WORK */
    char     text[36];
} jr_display_inbox_item_t;
void jr_display_boot_ignition_set(bool active, uint8_t stage, uint8_t progress_pct);
bool jr_display_boot_ignition_active(void);
void jr_display_desk_mode_set(uint8_t mode, uint32_t rem_s, uint8_t pct,
                              uint8_t breathe_phase, uint8_t breathe_sec,
                              uint8_t sessions_done, uint8_t sessions_total,
                              bool warn_pulse, const char *label);
bool jr_display_desk_mode_active(void);
void jr_display_inbox_set(const jr_display_inbox_item_t *items, size_t count, size_t open_count);
uint32_t jr_display_inbox_hit(uint16_t x, uint16_t y);
#endif

void timer_publish_now(uint32_t now_ms);
void desk_mode_snapshot(desk_mode_snap_t *out);
void desk_mode_apply_now(const desk_mode_req_t *req, uint32_t now_ms,
                         bool play_cue);
void desk_mode_request_async(const desk_mode_req_t *req);
void desk_tally_persist(void);
const char *desk_mode_api_name(jr_desk_mode_t m);
const char *desk_breathe_phase_api_name(jr_breathe_phase_t p);
bool desk_mode_parse_name(const char *s, jr_desk_mode_t *out);
bool desk_breathe_parse_pattern(const char *s, jr_breathe_pattern_t *out);
void persist_watch_style(uint8_t style);
void persist_holo_enabled(bool enabled);
void device_confirm_cue(void);
bool device_chip_temp_c(float *out_c);
void persist_ota_attempt(int slot);
void handle_say(const char *text);
void morning_briefing_force(void);          /* POST /api/debug/briefing */
int briefing_compose_live(char *dst, size_t cap, const jr_power_t *bat);
bool operator_lease_active(uint32_t now_ms);
bool operator_mode_active(uint32_t now_ms);
ota_preflight_t ota_preflight(void);
void title_shorten(char *dst, size_t cap, const char *src);
void demo_stop(void);

/* ---- owned by http_routes.c — the HTTP control plane ---- */

int touch_sector_from_point(uint16_t x, uint16_t y);
void secure_zero(void *ptr, size_t size);
bool brain_render_text_safe(const char *value, size_t capacity,
                                   bool allow_space);
void brain_surface_expire(uint32_t now);
bool brain_surface_handle_tap(uint16_t x, uint16_t y, uint32_t now,
                                     bool physical, uint32_t emitted_ms);
void panic_home_clear_glass(void);
bool operator_mode_release(uint32_t now, const char *reason,
                           bool physical_feedback, bool only_if_expired,
                           operator_mode_owner_t expected_owner,
                           uint32_t expected_lease_id);
bool ota_confirm_running_image_if_healthy(void);
void start_diag_http(void);
void crash_note_boot(void);   /* first line of app_main: reset reason, once */

/* ---- owned by power.c — CPU gears, deep sleep, wake state ---- */

extern uint32_t s_rtc_sleep_magic;
extern uint32_t s_rtc_sleeps;
extern uint8_t s_rtc_was_listening;
extern uint8_t s_rtc_armed;
extern _Atomic int s_cpu_mhz;
extern _Atomic int s_cpu_force;
extern _Atomic bool s_power_off_req;
extern _Atomic bool s_sleep_force;
extern _Atomic uint32_t s_sleep_timer_s;
extern int s_boot_wake_cause;

void cpu_gear_set(int mhz);
const char *wake_cause_name(int cause);
bool image_in_probation(void);
void enter_deep_sleep(const char *why, uint32_t timer_s);
unsigned power_restore_rtc_timers(uint32_t now_ms);

/* ---- owned by device_tools.c — device-tool queue, consent, weather, activity ---- */

extern jr_utt_watch_t s_utt;               /* owned by main.c's voice task */
extern jr_echo_gate_t s_echo_gate;         /* voice task writes; cockpit reads totals */
extern jr_reply_cut_t s_reply_cut;
extern _Atomic bool s_echo_gate_enabled;   /* /api/debug/gain?echogate=, not persisted */
extern _Atomic int s_echo_gate_volume_min; /* ?echovol=: gate applies above this volume */
extern _Atomic bool s_wake_barge_enabled;  /* ?wakebarge=: "Jarvis" cuts a reply */
extern _Atomic uint32_t s_wake_cuts;       /* replies cut by the wake word, cockpit */

/* Voice-loop pacing counters for /api/diag/voiceperf. The voice task is the
 * only writer; httpd reads them racily, which is fine for diagnostics. Class 0
 * is the loop while Speaking, class 1 every other frame it reads (listening,
 * asking, thinking, and the wake watch). Times are microseconds; the sums are
 * halved together with the frame count so the mean survives a long run. */
typedef struct {
    _Atomic uint32_t frames;
    _Atomic uint32_t iter_sum_us;   /* whole iteration, codec read included */
    _Atomic uint32_t iter_max_us;
    _Atomic uint32_t read_sum_us;   /* the codec read alone */
    _Atomic uint32_t fps_x100;      /* frames/s over the last full window */
} voiceperf_class_t;
extern voiceperf_class_t s_vperf[2];
extern _Atomic uint32_t s_vperf_tick_skips;   /* iterations that skipped the tick sleep */
extern _Atomic uint32_t s_vperf_tick_sleeps;  /* paced iterations that slept it */

/* /api/diag/voiceperf?bench=1: httpd sets the request, the voice task runs it
 * when it is not reading the microphone, the wake worker runs its own half. */
extern _Atomic bool s_aec_bench_req;
extern _Atomic bool s_aec_bench_have;
extern _Atomic int s_aec_bench_mean_us;      /* -1: no canceller or guard busy */
extern _Atomic uint32_t s_aec_bench_max_us;
extern _Atomic int s_aec_bench_cpu_mhz;
extern _Atomic uint32_t s_srv_interrupts;
extern _Atomic uint32_t s_srv_phantom_interrupts;
extern jr_display_weather_t s_weather;
extern uint32_t s_glance_until_ms;
extern int16_t s_wx_sun_rise_min;          /* minutes past local midnight; -1 unknown */
extern int16_t s_wx_sun_set_min;
int weather_rain_ahead_h(int horizon_h);
void board_take_done(int *count, char *title, size_t cap);

const char *device_tool_last_status(void);
const char *device_tool_last_name(void);
const char *screen_voice_prompt(jr_display_space_t space);
void weather_maybe_fetch(uint32_t now);
void board_maybe_poll(uint32_t now);
void activity_note_tool(const char *tool_name, const char *args_json);
void activity_note_said(const char *text);
void activity_note_turn_end(void);
void device_tool_record_result(const char *name, jr_tool_status_t status,
                                      uint32_t duration_ms, int http_status);
void device_tool_drop_local_results(void);
/* Answers weather_here (always) and current_time (when the gateway is not
 * usable) from the device itself; true when it took the call. */
bool device_try_local_tool(const jr_command_t *cmd);
/* Is the company gateway usable: configured AND answering. */
bool device_gateway_usable(void);
uint32_t device_gateway_poll_ms(void);
const char *device_gateway_why(void);
/* Tools the next setup would declare (all of them with a usable gateway). */
unsigned device_declared_tools(void);
void device_tool_queue_local_error(const jr_command_t *cmd,
                                          esp_err_t submit_error);
void device_tool_drain_results(jr_app_t *a, uint64_t now);
void device_tool_resolve_consent_locked(tool_consent_outcome_t outcome);
bool device_tool_present_consent(const jr_command_t *cmd, uint32_t now);
bool device_tool_cancel_pending_consent(uint32_t call_id,
                                               const char *call_id_text);
void device_tool_abort_pending_consent(void);

/* Wave 2: glass helpers, NVS desk notes ("jr_notes"), stopwatch, HBM sunlight. */
const char *device_glass_space_name(jr_display_space_t space);
bool device_parse_glass_screen(const char *s, jr_display_space_t *out);
bool device_parse_watch_style(const char *s, jr_watch_style_t *out);

void device_notes_init(void);
uint32_t device_notes_add_ex(const char *text, jr_note_tag_t tag);
uint32_t device_notes_add(const char *text);
bool device_notes_toggle(uint32_t id);
bool device_notes_complete(const char *target_or_id, uint32_t *matched_id);
bool device_notes_delete(uint32_t id);
unsigned device_notes_clear(const char *target);
void device_notes_snapshot(jr_note_bank_t *out);
size_t device_notes_counts(size_t *open_out, size_t *done_out);
void device_notes_sync_inbox(void);
int device_notes_format_briefing(char *dst, size_t cap);

bool device_stopwatch_action(const char *action, uint32_t now_ms,
                             stopwatch_snap_t *out_snap);
void device_stopwatch_snapshot(uint32_t now_ms, stopwatch_snap_t *out_snap);
void device_stopwatch_tick(uint32_t now_ms);

bool device_sunlight_set(bool enabled, uint32_t now_ms, const char **out_reason);
void device_sunlight_tick(uint32_t now_ms, const jr_power_t *bat, bool have_bat);
