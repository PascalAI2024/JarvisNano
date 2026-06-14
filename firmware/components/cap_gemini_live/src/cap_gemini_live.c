/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gemini Live voice capability — all 5 phases:
 *   Phase 1: WSS+TLS handshake + setup/setupComplete
 *   Phase 2: Text round-trip (clientContent → modelTurn console print)
 *   Phase 3: Receive audio (base64 PCM24k → ES8311 DAC)
 *   Phase 4: Send audio (ES7210 ADC 16kHz → realtimeInput)
 *   Phase 5: Touch toggle + emote status overlay
 *
 * Shared-clock I2S design (SOC_I2S_HW_VERSION_1): ADC + DAC are opened ONCE
 * per session, both on the shared 16 kHz clock (intentional — commit 66413b1).
 * Turn transitions only pause/resume capture (state-gated) and mute/unmute
 * the DAC; the codec is never closed/reopened per turn (P2.4/F11). Model
 * audio (24 kHz) is resampled to the session clock before playback.
 *
 * Audio pipeline (stability sprint P2.1–P2.3), one direction per arrow:
 *   WS task        → rx_queue (raw frames, byte-accounted/byte-capped; at the
 *                    cap the WS task WAITS instead of dropping — the unread
 *                    socket becomes TCP backpressure on the server, so a long
 *                    reply throttles to realtime instead of losing audio)
 *   gl_session     → cJSON parse + base64 + gain/limiter + resample → PCM ring
 *   gl_pcm_feeder  → blocking esp_codec_dev_write from the PCM ring — nothing else
 *   gl_audio_tx    → 32 ms 4-ch TDM reads (mics + ES7210 echo-ref lane, D2)
 *                    → demux → AEC(mic, ref) (esp-sr, D3/D4) → 6x gain + knee
 *                    → RMS → per-state policy (P3.4/D5): LISTENING uplink +
 *                    turn-commit VAD; SPEAKING stays OPEN-MIC for the local
 *                    barge detector (and, under server VAD, continuous
 *                    uplink) → tx_frame_queue (drop-oldest)
 *   gl_tx_sender   → drains tx_frame_queue → WS sends (Wi-Fi backpressure can
 *                    never stall the capture cadence)
 *   gl_tool_worker → toolCall frames (30 s HTTPS gl_act_call) off the session task
 *
 * Lifecycle ownership (stability sprint P1.1–P1.4): gl_session_task is the
 * SINGLE owner of codec open/close, TX-task start/stop, WS-client
 * create/destroy and the gl_set_state transitions tied to them. Public entry
 * points reachable from httpd / touch / CLI contexts only post requests:
 * send_text/end_input go through s_gl.cmd_queue (consumed by the session
 * loop in every wait state), start/stop set flags + event bits under
 * s_gl_lifecycle_mutex. No other task may call gl_open_* or gl_close_*,
 * gl_start_tx_task or gl_stop_tx_task, or create/destroy the WS client.
 *
 * NOTE: Gemini Live field names — the plan says realtimeInput.audio.data.
 * If the server rejects that format, switch GEMINI_AUDIO_KEY to "mediaChunks"
 * and wrap the chunk in an array: [{"mimeType":...,"data":...}].
 */

#include "cap_gemini_live.h"
#include "cmd_cap_gemini_live.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "emote.h"
#include "driver/i2s_common.h"
#include "esp_board_manager_includes.h"
#include "esp_board_periph.h"
#include "esp_aec.h"          /* esp-sr direct AEC API (aec-barge-in.md D3) */
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "jarvis_brain.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "cap_gemini_live";

/* forward declarations */
static cJSON *gl_get_object_compat(cJSON *obj, const char *camel, const char *snake);
static bool   gl_enter_speaking(uint32_t sample_rate);
static void   gl_resume_listening(const char *reason);
static void   gl_apply_in_gains(void);   /* state-aware mic/ref PGA (defined below) */
static void   gl_interrupt_playback(const char *reason);
static bool   gl_playback_pending(void);
static void   gl_drain_rx_queue(void);
static void   gl_process_cmd_queue(void);
static void   gl_dac_mute(bool mute);

/* ui_layer forward decls — cap_gemini_live does not REQUIRE ui_layer (avoids a
 * component dep cycle: ui_layer draws the display, cap drives voice), but
 * ui_layer is in the link graph via app_claw, so these symbols resolve at link
 * time. Same forward-decl idiom http_server uses to reach cap_gemini_live. Used
 * by the ask_user tool: show a tappable choice arc, then return the tapped
 * label as the functionResponse. Keep in sync with firmware/ui_layer/ui_layer.h. */
esp_err_t ui_layer_show_choice(const char *question, const char *const *opts, int n);
esp_err_t ui_layer_get_result(int *out_index, bool *out_done);
esp_err_t ui_layer_dismiss(void);
bool      ui_layer_is_active(void);

/* ---- Configuration -------------------------------------------------------- */

/* gemini-3.1-flash-live-preview never existed in production API (404 from server).
 * gemini-2.0-flash-live-001 was shut down 2025-12-09.
 * Current live model confirmed via API models list 2026-05-23. */
#define GEMINI_LIVE_MODEL        "models/gemini-2.5-flash-native-audio-latest"
#define GEMINI_WS_HOST           "generativelanguage.googleapis.com"
#define GEMINI_WS_PATH           "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"
/* JarvisMCP gateway — one authenticated POST runs JS (jarvis.* SDK) and returns
 * JSON. Both the endpoint URL and bearer token come from app_config (NVS) and are
 * NEVER hardcoded here — set jarvis_mcp_url + jarvis_mcp_key via POST /api/config.
 * If either is empty the tool bridge is inert (function calls return an error). */
#define GL_ACT_RESP_MAX          4096
#define GEMINI_PERSONA           "You are JARVIS, a witty British AI butler speaking through a small smart-speaker. " \
                                 "Be extremely concise: answer in ONE short sentence whenever possible, never more than two. " \
                                 "When asked for live facts (weather, news, prices, time), use the search tool, then state just the answer. " \
                                 "No lists, no preamble, no markdown — this is spoken aloud."

/* If the server rejects realtimeInput.audio, set to "mediaChunks" */
#define GEMINI_AUDIO_KEY         "audio"

#define GL_RX_BUF_SIZE           (96 * 1024)   /* WS reassembly buffer (PSRAM) — audio turns can be ~60KB */
#define GL_RX_QUEUE_DEPTH        256           /* completed-frame queue depth. Since P2.1 the session task drains this
                                                * at decode speed (DAC writes moved to the feeder), so depth here only
                                                * covers parse latency, not playback time. */
#define GL_RX_QUEUE_BYTE_CAP     (512 * 1024)  /* byte cap across queued raw frames (P2.2/F9): bounds the PSRAM the
                                                * raw queue can pin even if the session task stalls */
/* At the byte cap the WS task waits (10 ms steps) for the session task to free
 * queue space instead of dropping the frame. Holding the handler parks the WS
 * client task with the socket unread, which closes the TCP window — the server
 * throttles to our realtime drain rate and a long reply arrives without loss
 * (hardware gate 2026-06-12: a ~30 s story burst overflowed ring+queue and
 * dropped 36/785 frames under drop-newest). The deadline is a safety valve for
 * a genuinely wedged consumer: under backpressure a pop frees space every
 * ~50-150 ms (ring drains at the session clock), and even a max-size 96 KB
 * frame clears in ~1.4 s, so 5 s of zero progress means wedged — fall back to
 * dropping (pre-fix behaviour). Stop/teardown bails out within 10 ms via
 * stop_requested/session_active, so a tap-stop is never delayed. */
#define GL_RX_BACKPRESSURE_MAX_MS 5000
/* Decoded-PCM ring between the session task (producer: parse/decode/gain/
 * resample) and the playback feeder (consumer: blocking DAC writes). 1 MB at
 * the 16 kHz session clock (32 kB/s) buffers ~32 s of model speech — a whole
 * burst reply. Allocation degrades by halving down to the floor instead of
 * failing the session (P2.2/F9). */
#define GL_PCM_RING_BYTES        (512 * 1024)   /* ~16 s @16k; halved 2026-06-12 to free PSRAM for the AEC engine (was 1 MB; decode-buf OOM under AEC) */
#define GL_PCM_RING_MIN_BYTES    (128 * 1024)
#define GL_FEEDER_CHUNK_BYTES    2560          /* 80 ms @ 16 kHz mono s16 per DAC write — small enough that an
                                                * interrupt flush takes effect within one chunk (P3.1 < 200 ms) */
#define GL_FEEDER_STOP_WAIT_MS   5000          /* worst case: one in-flight DAC write (~330 ms) */
/* Capture→sender frame queue: 16 × 32 ms ≈ 512 ms of mic audio (P2.3/F10). */
#define GL_TX_FRAME_QUEUE_DEPTH  16
#define GL_TOOL_QUEUE_DEPTH      4
/* Mic capture interval. 32 ms = 512 samples @16 kHz = exactly one esp-sr AEC
 * chunk (aec_get_chunksize, FD/SR modes), so one TDM read feeds one
 * synchronous aec_process call with no rebuffering (D4). Was 20 ms pre-AEC;
 * the VAD constants below are expressed in frames of this size. */
#define GL_TX_CHUNK_MS           32
#define GL_TX_SAMPLE_RATE        16000
#define GL_RX_SAMPLE_RATE        24000
#define GL_CHANNELS              1
#define GL_BITS                  16
/* ---- 4-channel TDM capture (AEC Phase 2, design D2) ------------------------
 * The ES7210 runs TDM with the MEMS mics on lanes 0/1 and the hardware echo
 * reference (ES8311 line-out through the schematic's AEC pad into MIC3) on
 * lane 2; lane 3 (MIC4) is unconnected. Open the record dev with channel=4 /
 * channel_mask=0 and demux lanes in software. NEVER use channel_mask to
 * hardware-pick lanes: the esp_codec_dev STD branch clamps the mask to the 2
 * physical slots (slot_mask & I2S_STD_SLOT_BOTH) and silently drops the ref.
 * Each I2S frame then carries 4×16-bit samples packed as 2×32-bit STD slots:
 * [MIC1][MIC2][REF][NC] expected — verified empirically via lane_rms. */
#define GL_CAPTURE_CHANNELS      4
/* MEASURED buffer-lane order (Phase 2 tone test, hardware, 2026-06-12):
 *   lane 0 = ES7210 MIC3 echo reference (silent when the DAC is muted; rises to
 *            rms~390 / peak~1650 raw, no clipping, while the speaker plays —
 *            it tracks playback, so the HW loopback is LIVE),
 *   lane 1 = live MEMS mic (uplink),
 *   lane 2 = MIC4 / unconnected (dead, peak 2 even with the speaker blasting),
 *   lane 3 = live MEMS mic.
 * i.e. [REF][MIC][NC][MIC] — scrambled vs the design's nominal
 * [MIC1][MIC2][REF][NC], but fully identified. Demux follows the MEASURED order.
 *
 * IMPORTANT: the per-channel GAIN mask is a CHIP-mic index (es7210 maps mask
 * bit N → MIC(N+1) gain register), a DIFFERENT numbering space from these
 * buffer-lane indices. Chip MIC3 (the reference) is gain-mask bit 2 regardless
 * of which buffer lane it lands on — see GL_REF_CHIP_MASK_BIT. */
#define GL_MIC_LANE              1   /* buffer lane: live MEMS mic (uplink)        */
#define GL_REF_LANE              0   /* buffer lane: MIC3 echo reference (Phase 3) */
#define GL_REF_CHIP_MASK_BIT     2   /* es7210 chip MIC3 → REG45 (gain-mask space) */
/* AEC gain staging (calibration 2026-06-12). The +30 dB MEMS-mic PGA railed the
 * echo at full scale (peak 32767, uncancellable nonlinearity) while the ref sat
 * ~35 dB lower at 0 dB. A linear AEC needs an UNCLIPPED mic and a ref at a level
 * comparable to the echo-in-mic. Measured SPEAKING echo-in-mic rms ~10000 @30 dB
 * vs ref rms ~175 @0 dB: cut the mics 12 dB (echo peak ~8200, unclipped) and
 * raise the ref 24 dB (~2800, ≈ post-cut echo ~2500). Tune from aec_atten_db. */
#define GL_MIC_PGA_DB            24   /* LISTENING mic gain (chip MIC1/MIC2) — loud enough to hear the USER (2026-06-13: confirmed responses at 24 dB; 6 dB was too quiet for the local VAD). Runtime-tunable listen default. */
#define GL_MIC_PGA_SPEAK_DB      18   /* SPEAKING mic gain. 2026-06-14: was 9 — that dropped the user's talk-over to ~30-50 RMS (below any safe barge floor), so barge "only worked if you shouted". 18 dB lifts the barge back to ~65-130 while the AEC stays healthy (measured atten 8-24 dB, no clipping at this echo level). The louder echo is rejected by the peak-held proportional gate, not by starving the mic. Runtime: /api/debug/gain?speak=N. */
#define GL_REF_PGA_DB            12   /* echo reference (chip MIC3, mask bit 2) — matched to the post-cut echo level */
/* Make-up gain + soft-knee limiter for model audio. Measured PCM is speech with
 * a high crest factor: peaks ~80% full-scale but RMS only ~15%, so it sounds
 * quiet. A flat gain big enough to raise the average hard-clips the peaks into
 * distortion (that was the earlier "can't understand" at 4x). Instead apply
 * GL_OUT_GAIN to lift the body, then compress 4:1 above GL_LIMIT_KNEE so loud
 * syllables are limited smoothly, not squared off. */
#define GL_OUT_GAIN              4
#define GL_LIMIT_KNEE            24000
#define GL_TX_SAMPLES_PER_CHUNK  (GL_TX_SAMPLE_RATE * GL_TX_CHUNK_MS / 1000)  /* 512 */
#define GL_TX_PCM_BYTES          (GL_TX_SAMPLES_PER_CHUNK * GL_CHANNELS * (GL_BITS / 8)) /* 1024 */
#define GL_TX_RAW_BYTES          (GL_TX_SAMPLES_PER_CHUNK * GL_CAPTURE_CHANNELS * (GL_BITS / 8)) /* 4096 */
/* Native 24 kHz output: the ES8311 DAC + ES7210 ADC share one duplex I2S clock,
 * so to play the model's 24 kHz audio crisply (not downsampled to 16 kHz) the
 * WHOLE codec runs at 24 kHz. The mic is then captured at 24 kHz and downsampled
 * 24->16 to a byte-identical 16 kHz frame right after the read, so the AEC / VAD
 * / barge / uplink pipeline (all 512-sample 16 kHz) is unchanged. */
#define GL_CAP_SAMPLE_RATE       GL_RX_SAMPLE_RATE   /* 24000 — codec/capture clock = model output rate */
#define GL_CAP_SAMPLES_PER_CHUNK (GL_CAP_SAMPLE_RATE * GL_TX_CHUNK_MS / 1000)  /* 768 */
#define GL_CAP_RAW_BYTES         (GL_CAP_SAMPLES_PER_CHUNK * GL_CAPTURE_CHANNELS * (GL_BITS / 8)) /* 6144 */
#define GL_TX_B64_BYTES          (((GL_TX_PCM_BYTES + 2) / 3) * 4 + 1)
/* Frames dumped when reads resume after a GENUINE capture pause (no reads at
 * all): the ADC stays open across turns (P2.4) and the RX DMA accumulates
 * stale audio — including speaker echo — while nobody reads. 7 × 32 ms ≈
 * 224 ms (was 10 × 20 ms). Since P3.4 the mic stays open through
 * LISTENING↔SPEAKING transitions, so those never arm this dump — only real
 * pauses (manual-mode THINKING, AEC-degraded SPEAKING, IDLE/CONNECTING). */
#define GL_CAPTURE_FLUSH_FRAMES  7
/* Ignore taps within this window of the last accepted one. A session start runs
 * a multi-second WSS+TLS handshake; rapid taps otherwise toggle start/stop mid-
 * connect and wedge the session on "connecting". */
#define GL_TOGGLE_COOLDOWN_MS    2000
#define GL_WS_TIMEOUT_MS         5000
/* Mic frames fail fast: a frame is worth 32 ms of audio, so blocking the TX
 * loop 5 s on Wi-Fi backpressure only skews VAD and stretches the TX stop
 * window. Losing a frame is recoverable; a parked capture loop is not.
 * Root-cause #1, Part B: 500 ms was short enough that a brief RTT spike made
 * the underlying send return 0 (would-block) instead of waiting the stall out —
 * and esp_websocket_client turns that write-0 into a fatal abort. 1500 ms rides
 * out a typical spike (one frame's worth of extra latency, recoverable) while
 * still leaving margin under GL_TX_STOP_WAIT_MS (8 s) so the "genuinely wedged"
 * semantics of the TX stop wait hold. */
#define GL_WS_MIC_TIMEOUT_MS     1500
/* TX stop wait must exceed the worst-case blocked iteration of the TX loop
 * (codec read ~200 ms + one mic send timeout) AND the old full control-send
 * timeout, so the timeout firing means "genuinely wedged", never "just slow".
 * The old 3 s wait raced the 5 s WS send timeout and the loser got
 * force-deleted (F1). */
#define GL_TX_STOP_WAIT_MS       8000
/* Upper bound for joining the async WS cleanup task before a new client may
 * be created (stop can block up to network_timeout_ms = 10 s). */
#define GL_WS_CLEANUP_JOIN_MS    15000
/* In-session WS resume (root-cause #1 cure). ESP-IDF esp_websocket_client treats
 * a transport write that returns 0 (a benign TCP would-block / poll-write
 * timeout; errno=0) as fatal: it aborts the connection and fires
 * WEBSOCKET_EVENT_DISCONNECTED. Before this, the session loop saw ws_connected
 * go false and tore the WHOLE session down to IDLE (mute until a fresh session)
 * — observed 11x "WS dropped, cleaning up session". Instead, on a drop we now
 * stop()+start() the SAME client handle (reuses the registered event handler +
 * shared rx_buf — no two-clients window, no destroy/re-init), re-send setup, and
 * re-arm the activity, keeping the PCM ring + converters + feeder + TX task open.
 * Only if every attempt fails do we fall through to session_cleanup. */
#define GL_WS_RESUME_ATTEMPTS    3
#define GL_WS_RESUME_CONNECT_MS  8000
#define GL_WS_RESUME_SETUP_MS    8000
#define GL_WS_RESUME_BACKOFF_MS  300
#define GL_SPEAK_WATCHDOG_MS     20000  /* was 4500 — too short for native-audio first-token latency. Firing this resumes listening (sends activityStart), which the server reads as a USER interrupt and CANCELS the reply (observed mute, 2026-06-13). 20 s only catches genuine drops; it disarms the instant the first audio arrives. */
#define GL_LISTEN_RECOVERY_MS    2000
#define GL_CODEC_HANDLE_RETRY_MS 250
#define GL_CODEC_HANDLE_RETRIES  6
#define GL_PCM_DECODE_RETRY_MS   8     /* yield once on PCM-decode OOM. PSRAM is healthy (~720 KB free, 2026-06-13); the OOM is a momentary low-water dip on the first 24k chunk of a turn (AEC/feeder/rwave-clip churn), not a fragmentation ceiling. One short yield lets the transient allocation land. */

/* Turn-taking mode (P3.4, aec-barge-in.md D5).
 *
 * 1 = server VAD (auto mode, PRIMARY): the AEC-cleaned mic streams to the
 *     server continuously — including during SPEAKING — and Gemini's
 *     automaticActivityDetection owns turn boundaries, barge-in semantics and
 *     history truncation (activityHandling: START_OF_ACTIVITY_INTERRUPTS).
 *     Client activityStart/End are forbidden in this mode (no-op'd below);
 *     any uplink pause >1 s must send realtimeInput.audioStreamEnd (capture
 *     task). The local turn-commit VAD is bypassed at runtime; the local
 *     barge detector still owns the fast playback kill (see GL_BARGE_RMS) —
 *     the server `interrupted` frame is confirmation, not the trigger.
 *
 * 0 = manual mode (FALLBACK — keep fully compiled, do not delete): client
 *     sends activityStart before mic frames and activityEnd to commit a turn
 *     (local VAD / tap / HTTP). Mic frames are sent only while LISTENING;
 *     a barge trigger re-opens the activity via the interrupt→resume path.
 *
 * History: server VAD was enabled 2026-05-25 (ae0ed99) and disabled
 * 2026-05-26 (4639cbb) — "server never returns serverContent... mic looks
 * alive, zero reply". Root cause was mic LEVEL, not protocol: raw
 * conversation RMS was 50-300 (≈ −50 dBFS — silence to a server VAD), and
 * the SAME commit that disabled the flag added the 6x digital gain that
 * fixed it; the flag never ran with the gained signal (git log -S, see
 * aec-barge-in.md "Findings"). Re-enabled for P3.4 with the AEC (P3.3)
 * removing the echo confound for the during-playback stream. */
#define GL_USE_SERVER_VAD        0  /* 2026-06-14: REVERTED to manual mode after testing server VAD on hardware. Server VAD (the "official" barge path) hit the same wall the dev found twice: the user's normal-volume voice reads ~230 RMS at the mic — below Google's server-VAD speech floor — so the server heard silence and NEVER replied (40s of user speech -> 0 turns, the documented "server never returns serverContent"). Manual mode replies reliably. Real talk-over barge needs the mic-to-server SNR raised (acoustic/gain work, a dedicated effort), not a flag flip. Local RMS barge stays available (GL_BARGE_RMS, default high = off-ish) but self-barges if set low (the choppiness). Tap-to-interrupt is the reliable manual barge. */

/* On-device VAD (hands-free turn commit). Server VAD does not return
 * serverContent on this Waveshare board (see above), so instead of disabling
 * hands-free we detect end-of-speech locally: the TX task watches the mic RMS
 * it already computes for the face, and once the user has spoken and then
 * stayed quiet for GL_VAD_HANG_FRAMES it asks the session task to commit the
 * turn (the same end_input the tap / HTTP path uses). Thresholds calibrated
 * live on the ES7210 capture (post-6x-gain RMS): silence floor ~150-400,
 * speech 1000-4900. The VAD now sees the AEC-cleaned signal (P3.3), but with
 * a muted DAC during LISTENING the ref lane is silent and the AEC is ~pass-
 * through, so the calibration holds. Hysteresis between SILENCE_RMS and
 * SPEECH_RMS prevents flicker. */
#define GL_USE_LOCAL_VAD         1
#define GL_VAD_SPEECH_RMS        150   /* 2026-06-14: was 120, then validated at 150 in a live hands-free test (user spoke normally -> THINKING -> SPEAKING with no tap). Measured: ambient ~63 RMS, this user's normal voice 130-256, inter-word dips ~93-117. 150 starts a turn on real speech without latching on room noise. Runtime-tunable: /api/debug/gain?vadspeech=N */
                                       /* above the measured idle ceiling (~939   */
                                       /* raw over 60s silent) at the bottom of   */
                                       /* the measured speech band (1000-4900) —  */
                                       /* 1200 missed quiet/distant speech, which */
                                       /* read as "it never replies".             */
#define GL_VAD_SILENCE_RMS       130   /* 2026-06-14: was 80, then validated at 130 in the same live test. RMS below this = silence (end-of-turn hysteresis). 80 sat below this user's inter-word/pause level (~93-117) so a real end-of-turn pause never read as silence and the turn never committed (no hands-free reply). 130 sits above the pause level so trailing silence completes and the turn commits. Runtime-tunable: /api/debug/gain?vadsilence=N */
/* VAD accumulators count 32 ms capture frames since the AEC rechunk (D4).
 * HANG: 22 frames = 704 ms ≈ the field-tuned 700 ms hang. (The design table's
 * "16 frames = 512 ms" was derived from a stale 500 ms baseline; keeping the
 * tuned value is what actually preserves turn-commit timing — the Phase-3
 * acceptance bar. Drop toward 16 only with a fresh 10-turn field test.)
 * MIN_SPEECH: 8 frames = 256 ms of sustained speech before a commit can arm
 * (was 240 ms; 300 swallowed short replies like "yes"). */
#define GL_VAD_HANG_FRAMES       22
#define GL_VAD_MIN_SPEECH_FRAMES 8

/* ---- Hands-free barge-in (P3.4, design D5) ---------------------------------
 * RMS speech detector on the AEC-CLEANED, post-6x-gain mic during SPEAKING
 * only. GL_BARGE_RMS is deliberately separate from (and above)
 * GL_VAD_SPEECH_RMS: during playback the floor is post-AEC residual echo, not
 * room noise — the AEC suppresses ~20-30 dB but not everything, and the 6x
 * digital gain amplifies the residue. Conservative default: the measured
 * normal-speech band post-gain is 1000-4900 while worst-case post-gain
 * residual-echo estimates sit near ~2000, so 2500 favours ZERO
 * self-interruptions over catching whispered barges (the Phase-4 acceptance
 * bar is zero self-interruptions over 10 turns at vol 100, R4). Calibrate on
 * hardware WITHOUT reflashing:
 *     gemini-live --barge-rms <n>      (0 disables; cap_gemini_live_set_barge_rms)
 * and read the live floor from aec_atten / mic_level in /api/gemini/live.
 * The latch = GL_BARGE_LATCH_FRAMES consecutive frames (2 × 32 ms = 64 ms of
 * sustained speech) — long enough to skip pops and taps, short enough that
 * perceived stop is fluid. Since 2026-06-14 the capture task mutes the codec
 * the instant the latch arms (fast-kill, s_playback_kill) rather than waiting
 * for the session task's cmd-queue hop + ring flush — so the perceived stop is
 * ~latch + I2C mute, no longer the 135-262 ms variance that tracked ring fill.
 * Still measured per event as `barge_latency` in the log.
 * Requires a live AEC engine: with the AEC degraded the detector stays OFF
 * (raw echo would self-trigger) and barge-in remains tap-only. */
#define GL_BARGE_RMS             80     /* Absolute floor of the adaptive barge gate (the other term is GL_BARGE_RATIO_PCT × peak playback). 2026-06-14: at the 18 dB SPEAKING gain the user's normal talk-over reads ~65-130 RMS and a SPEAKING-pause ambient ~22, so 80 catches a normal barge in her pauses while staying clear of ambient; the peak-held proportional term handles loud playback. Runtime-tunable: /api/debug/gain?barge=N (0 = off). */
#define GL_BARGE_LATCH_FRAMES    2    /* 2026-06-14: was 4 (128 ms). 2 frames (64 ms) of sustained 220+ RMS over playback is a real barge — the guard window + threshold already reject echo transients. Halves the fixed detection floor for a fluid talk-over stop. */
/* Post-SPEAKING-entry guard: do NOT arm the local barge detector for this long
 * after gl_enter_speaking. The 24 dB LISTENING mic gain drops to
 * GL_MIC_PGA_SPEAK_DB via an async codec control write that does not land
 * instantly, and the AEC must re-converge on the fresh echo path at turn start
 * — during that settling the first SPEAKING frames can carry a loud,
 * not-yet-cancelled echo transient that would latch a phantom barge and flush
 * the model's own reply (self-interrupt stutter, R4). 300 ms ≈ 9 capture
 * frames: past the gain transient + initial AEC adaptation, far below
 * native-audio first-token latency, so a genuine early user barge is still
 * caught once the window elapses (real barges are sustained speech, not one
 * transient). Anchored on s_gl.speak_enter_us (set in gl_enter_speaking). */
#define GL_BARGE_GUARD_MS        500    /* 2026-06-14: was 200. The cold AEC needs ~300-500 ms to converge below the barge threshold; at 200 ms the residual echo (~230 RMS) self-barged her own reply 280 ms in (logged). 500 ms covers convergence — genuine talk-over after she's a few words in still fires at ~79 ms. Runtime-tunable: /api/debug/gain?guard=N. */
/* Auto-VAD stream contract: an uplink pause longer than this must be flushed
 * with realtimeInput.audioStreamEnd or the server keeps stale cached audio
 * that bleeds into the next utterance (aec-barge-in.md §4). */
#define GL_STREAM_END_PAUSE_MS   1000

/* ---- Acoustic echo cancellation (P3.3, design D3/D4) -----------------------
 * esp-sr direct esp_aec.h API: no AFE framework, no model files/partition, no
 * internal task — aec_process runs synchronously in the capture task, one
 * 512-sample/32 ms chunk per TDM read. mic = the demuxed MEMS lane
 * (GL_MIC_LANE); ref = the ES7210 MIC3 hardware loopback (GL_REF_LANE,
 * verified live 2026-06-12: ref rises only while the DAC plays, raw peak
 * ~1655 ≈ -26 dBFS at 0 dB PGA — under-driven, zero clipping risk; +3/+6 dB
 * headroom available for calibration). FD_LOW_COST + aggressive NLP per D3.
 * R3 CPU gate: if the measured p95 cost exceeds GL_AEC_COST_GATE_US, switch
 * GL_AEC_MODE to AEC_MODE_SR_HIGH_PERF (linear-only, lighter). Engine create
 * failure degrades to un-cancelled capture — never a crash. */
#define GL_USE_AEC               1
#define GL_AEC_MODE              AEC_MODE_FD_LOW_COST
#define GL_AEC_FILTER_LENGTH     4              /* esp_aec.h recommended value */
#define GL_AEC_COST_GATE_US      10000          /* <10 ms per 32 ms frame (R3) */
#define GL_AEC_STAT_FRAMES       60             /* ~2 s stats window (AEC now runs SPEAKING-only, so this is ~2 s of speech) — short enough to publish aec_atten/aec_cost on brief replies */
#define GL_AEC_ATTEN_MIN_FRAMES  15             /* ≥~0.5 s of playback in the window */

/* Per-lane capture diagnostic (Phase 2 bring-up tool, retained through the
 * AEC verification phases): the capture task logs one I-level `lane_rms` line
 * per second with the raw (pre-AEC, pre-digital-gain) RMS + peak of all four
 * demuxed lanes, and the diagnostics JSON carries lane_rms/lane_peak arrays —
 * the cross-check for the aec_atten estimate while the speaker is live.
 * Remove in the Phase-4 cleanup once hands-free barge-in is field-verified. */
#define GL_LANE_DIAG             1

#define GL_I2S_READ_TIMEOUT_MS   200
#define GL_AUDIO_WRITE_MARGIN_MS 250
#define GL_AUDIO_DAC_PERIPH      "i2s_audio_out"
#define GL_AUDIO_ADC_PERIPH      "i2s_audio_in"

/* ---- State ---------------------------------------------------------------- */

typedef enum {
    GL_STATE_IDLE = 0,
    GL_STATE_CONNECTING,
    GL_STATE_READY,        /* setupComplete received */
    GL_STATE_LISTENING,
    GL_STATE_THINKING,
    GL_STATE_SPEAKING,
} gl_state_t;

/* EventGroup bits */
#define GL_BIT_STOP         (1 << 0)
#define GL_BIT_SESSION_ON   (1 << 1)
#define GL_BIT_TX_STOP      (1 << 2)
#define GL_BIT_TX_DONE      (1 << 3)
#define GL_BIT_SETUP_OK     (1 << 4)
#define GL_BIT_SESSION_DONE (1 << 5)
#define GL_BIT_WS_CLEANED   (1 << 6)   /* async WS stop/destroy finished; a new
                                        * client may be created (F6 join) */
#define GL_BIT_TXS_DONE     (1 << 7)   /* TX sender task exited (P2.3) */
#define GL_BIT_FEEDER_STOP  (1 << 8)   /* park the playback feeder (P2.1) */
#define GL_BIT_FEEDER_DONE  (1 << 9)   /* playback feeder exited */

/* ---- Session command queue -------------------------------------------------
 * Single-owner lifecycle: every state-mutating request from a non-session
 * context (httpd send_text/end_input, touch tap-commit, TX-task local-VAD
 * commit) is posted here and consumed by gl_session_task, which alone runs
 * the codec/TX/WS lifecycle. This generalises the old vad_commit_request
 * flag (same pattern, one mechanism). start/stop are not queued: they were
 * already flag/event posts (stop_requested + GL_BIT_*), now serialised by
 * s_gl_lifecycle_mutex. */
typedef enum {
    GL_CMD_END_INPUT = 1,   /* commit the user's audio turn (tap / HTTP / VAD) */
    GL_CMD_SEND_TEXT,       /* send a text turn (HTTP / CLI) */
    GL_CMD_INTERRUPT,       /* tap during SPEAKING — flush playback, back to
                             * LISTENING (P3.1); shares the path the server
                             * `interrupted` frame uses (P3.2) */
} gl_cmd_type_t;

typedef struct {
    gl_cmd_type_t type;
    char         *text;     /* heap copy for GL_CMD_SEND_TEXT; consumer frees */
} gl_cmd_t;

#define GL_CMD_QUEUE_DEPTH  8

/* JarvisMCP tool execution rides a dedicated worker task: gl_act_call blocks
 * up to 30 s on HTTPS, which must never stall the session task's rx drain
 * (P2.1/F8). Jobs are tagged with the session generation so a result from a
 * dead session is dropped instead of being sent into the next one. */
typedef struct {
    uint32_t  gen;          /* s_gl.session_gen at queue time */
    cJSON    *tool_call;    /* detached "toolCall" object; worker deletes */
} gl_tool_job_t;

typedef struct {
    char                         api_key[320];
    char                         mcp_key[192];      /* JarvisMCP /act bearer token (NVS) */
    char                         mcp_url[192];      /* JarvisMCP /act endpoint URL (NVS) */
    volatile gl_state_t          state;
    volatile bool                stop_requested;
    volatile bool                session_active;
    esp_websocket_client_handle_t ws_client;
    volatile bool                ws_connected;
    char                        *rx_buf;            /* GL_RX_BUF_SIZE reassembly scratch, PSRAM */
    QueueHandle_t                rx_queue;           /* completed frames (char*, PSRAM) → session task */
    QueueHandle_t                cmd_queue;          /* gl_cmd_t requests → session task (single owner) */
    volatile uint32_t            rx_drops;           /* frames dropped when queue full */
    volatile uint32_t            rx_frames;          /* parsed server frames */
    uint32_t                     text_part_hits;
    uint32_t                     audio_part_hits;
    uint32_t                     turn_complete_hits;
    uint32_t                     generation_complete_hits;
    uint32_t                     interrupted_hits;
    uint32_t                     barge_hits;          /* local barge detector fires (P3.4) */
    uint32_t                     audio_stream_end_hits; /* audioStreamEnd sent (auto VAD) */
    uint32_t                     tool_cancel_hits;    /* toolCallCancellation frames */
    volatile int64_t             barge_onset_us;      /* speech-onset stamp → barge_latency log.
                                                       * Written by the capture task immediately
                                                       * before posting GL_CMD_INTERRUPT; consumed
                                                       * (and zeroed) by the session task inside
                                                       * gl_interrupt_playback — ordered by the
                                                       * cmd queue, so no torn read in practice. */
    uint32_t                     tool_call_hits;
    uint32_t                     unhandled_hits;
    uint32_t                     resume_count;
    uint32_t                     watchdog_resume_count;
    int64_t                      last_frame_us;
    int64_t                      last_audio_us;
    int64_t                      last_resume_us;
    bool                         waiting_terminal;
    char                         last_resume_reason[32];
    char                         last_audio_error[96];
    EventGroupHandle_t           ev;
    TaskHandle_t                 session_task;
    TaskHandle_t                 tx_task;            /* mic capture (32 ms cadence) */
    TaskHandle_t                 tx_sender_task;     /* drains tx_frame_queue → WS (P2.3) */
    TaskHandle_t                 feeder_task;        /* PCM ring → DAC (P2.1) */
    TaskHandle_t                 tool_task;          /* JarvisMCP tool worker (P2.1) */
    QueueHandle_t                tx_frame_queue;     /* 32 ms mic frames, by value */
    QueueHandle_t                tool_queue;         /* gl_tool_job_t → tool worker */
    SemaphoreHandle_t            ring_mutex;         /* guards pcm_ring indices */
    uint8_t                     *pcm_ring;           /* decoded PCM at the DAC rate (PSRAM) */
    size_t                       pcm_ring_cap;
    size_t                       pcm_ring_head;      /* write index (session task) */
    size_t                       pcm_ring_tail;      /* read index (feeder task) */
    volatile size_t              pcm_ring_bytes;     /* occupancy; volatile for lock-free peeks */
    volatile uint32_t            pcm_ring_epoch;     /* bumped on flush — aborts in-flight producer */
    uint32_t                     pcm_ring_drop_bytes;/* producer bytes dropped (flush/stop mid-write) */
    volatile bool                feeder_writing;     /* feeder mid-DAC-write */
    volatile bool                pending_resume;     /* terminal frame seen, ring still draining */
    char                         pending_resume_reason[32];
    int64_t                      speak_enter_us;     /* enter_speaking timestamp (latency log) */
    volatile bool                first_audio_pending;/* set per turn until the first DAC feed */
    uint32_t                     last_first_audio_ms;/* last measured first-audio latency */
    volatile uint32_t            session_gen;        /* bumped per session; stale tool jobs dropped */
    SemaphoreHandle_t            ws_mutex;          /* serialises WS sends */
    esp_codec_dev_handle_t       dac;
    esp_codec_dev_handle_t       adc;
    i2s_chan_handle_t           adc_chan;
    i2s_chan_handle_t           dac_chan;
    bool                         adc_raw;
    bool                         dac_raw;
    bool                         dac_codec_failed;
    bool                         adc_codec_failed;
    esp_lcd_touch_handle_t       touch;
    /* I2S codec open-state tracking. esp_codec_dev_open/close are NOT idempotent:
     * a redundant close drives i2s_channel_disable on an already-disabled channel
     * ("channel has not been enabled yet") and wedges the audio path under rapid
     * start/stop. Track open state + the rate so open is rate-aware and close is
     * a no-op when already closed. */
    bool                         dac_open;
    uint32_t                     dac_rate;
    uint32_t                     dac_raw_rate_hz;
    bool                         adc_open;
    uint32_t                     adc_rate;
    uint32_t                     adc_raw_rate_hz;
    uint32_t                     last_audio_mime_rate;
    uint32_t                     rate_mismatch_chunks;
    uint32_t                     tx_frames_sent;
    uint32_t                     tx_send_failures;
    uint32_t                     tx_read_failures;
    uint32_t                     tx_codec_reads;
    uint32_t                     tx_raw_reads;
    uint32_t                     ws_resume_count;   /* in-session WS resumes survived (root-cause #1) */
    int64_t                      last_input_end_us;
    bool                         activity_open;
    int64_t                      thinking_since_us;  /* when THINKING was entered; gates the tap-to-stop grace */
} gl_ctx_t;

static gl_ctx_t s_gl;

/* Serialises gl_gateway_start / gl_gateway_stop / session activation so the
 * check-then-create on s_gl.session_task is atomic — exactly one gl_session
 * task can ever exist (F5 / P1.4). Statically backed; materialised on the
 * single-threaded boot path (gl_cap_init) before any concurrent caller. */
static SemaphoreHandle_t s_gl_lifecycle_mutex;
static StaticSemaphore_t s_gl_lifecycle_mutex_buf;

static void gl_lifecycle_lock(void)
{
    if (!s_gl_lifecycle_mutex) {
        s_gl_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_gl_lifecycle_mutex_buf);
    }
    xSemaphoreTake(s_gl_lifecycle_mutex, portMAX_DELAY);
}

static void gl_lifecycle_unlock(void)
{
    xSemaphoreGive(s_gl_lifecycle_mutex);
}

/* Post a request to the session task (defined with the other cmd-queue
 * helpers below; forward-declared here for the TX task's VAD commit). */
static esp_err_t gl_post_cmd(gl_cmd_type_t type, char *text);

static const char *gl_state_name(gl_state_t st)
{
    switch (st) {
    case GL_STATE_IDLE:
        return "IDLE";
    case GL_STATE_CONNECTING:
        return "CONNECTING";
    case GL_STATE_READY:
        return "READY";
    case GL_STATE_LISTENING:
        return "LISTENING";
    case GL_STATE_THINKING:
        return "THINKING";
    case GL_STATE_SPEAKING:
        return "SPEAKING";
    default:
        return "UNKNOWN";
    }
}

static void gl_mark_resume_reason(const char *reason)
{
    if (!reason) {
        return;
    }
    strlcpy(s_gl.last_resume_reason, reason, sizeof(s_gl.last_resume_reason));
    s_gl.last_resume_us = esp_timer_get_time();
}

static void gl_set_audio_error(const char *reason)
{
    if (!reason) {
        return;
    }
    strlcpy(s_gl.last_audio_error, reason, sizeof(s_gl.last_audio_error));
}

static void gl_clear_audio_error(void)
{
    s_gl.last_audio_error[0] = '\0';
}

static void gl_reset_diag_counters(void)
{
    s_gl.rx_frames             = 0;
    s_gl.text_part_hits        = 0;
    s_gl.audio_part_hits       = 0;
    s_gl.turn_complete_hits    = 0;
    s_gl.generation_complete_hits = 0;
    s_gl.interrupted_hits      = 0;
    s_gl.barge_hits            = 0;
    s_gl.audio_stream_end_hits = 0;
    s_gl.tool_cancel_hits      = 0;
    s_gl.barge_onset_us        = 0;
    s_gl.tool_call_hits        = 0;
    s_gl.unhandled_hits        = 0;
    s_gl.resume_count          = 0;
    s_gl.watchdog_resume_count = 0;
    s_gl.last_frame_us         = 0;
    s_gl.last_audio_us         = 0;
    s_gl.last_resume_us        = 0;
    s_gl.waiting_terminal      = false;
    s_gl.last_resume_reason[0] = '\0';
    s_gl.last_audio_mime_rate  = 0;
    s_gl.last_audio_error[0]   = '\0';
    s_gl.rate_mismatch_chunks  = 0;
    s_gl.tx_frames_sent        = 0;
    s_gl.tx_send_failures      = 0;
    s_gl.ws_resume_count       = 0;
    s_gl.tx_read_failures      = 0;
    s_gl.tx_codec_reads        = 0;
    s_gl.tx_raw_reads          = 0;
    s_gl.last_input_end_us     = 0;
    s_gl.activity_open         = false;
    s_gl.pcm_ring_drop_bytes   = 0;
}

/* P0.4 (logging half): one heap line at session start/stop so leaks and PSRAM
 * fragmentation are trendable from the SD log across sessions. */
static void gl_log_heap_snapshot(const char *when)
{
    ESP_LOGI(TAG,
             "heap[%s]: int free=%u largest=%u min=%u | psram free=%u largest=%u min=%u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

/* ---- Audio level (RMS) for the reactive waveform -------------------------- *
 * Updated lock-free from the audio TX task (mic) and the playback path (out),
 * read by the display layer via the public getters. Stored as int16 RMS
 * (0..32767); getters normalise to 0..1 float. Plain atomics, no locks. */
static _Atomic uint16_t s_mic_rms;   /* ES7210 capture level (LISTENING) */
static _Atomic uint16_t s_out_rms;   /* decoded playback level (SPEAKING) */

/* Barge-detector threshold (P3.4): RMS on the AEC-cleaned post-gain mic
 * during SPEAKING. Atomic so the CLI (gemini-live --barge-rms) can calibrate
 * it live mid-session without a reflash. 0 disables the detector. */
static _Atomic uint16_t s_barge_rms = GL_BARGE_RMS;
/* Fast-barge playback kill (2026-06-14): the capture task sets this the instant
 * the barge latch arms, BEFORE the session task services GL_CMD_INTERRUPT (which
 * queues behind PCM decode — the 135-262 ms / "huge delay" variance in the
 * barge_latency logs). The feeder honours it within one chunk: mutes the codec
 * (instant ES8311 register silence) and stops feeding the DMA. The session task
 * clears it on flush/resume. Perceived stop drops to ~latch + one feeder chunk,
 * independent of session-task load. */
static _Atomic bool     s_playback_kill = false;
/* Barge guard window (ms after SPEAKING entry where the detector stays OFF while
 * the AEC re-converges). Runtime-tunable via /api/debug/gain?guard=N so the
 * convergence window can be swept live without a 10-min reflash — the cold-AEC
 * residual self-barges if this is too short (observed: rms=230 echo at 280 ms
 * tripped a phantom barge with the old 200 ms guard). */
static _Atomic int      s_barge_guard_ms = GL_BARGE_GUARD_MS;
/* Adaptive barge gate (2026-06-14): the echo residual is PROPORTIONAL to the
 * current playback level (s_out_rms), but the user's voice is independent of it.
 * An absolute threshold can't separate them — measured: this user's normal
 * talk-over (~<450 RMS) overlaps the echo spikes (~382). So the effective barge
 * threshold is max(absolute floor, ratio%% × playback level): low during her
 * pauses (a soft barge fires), rising only as loud as her own playback demands
 * (rejecting her echo). Both terms runtime-tunable: barge=<floor>, ratio=<pct>.
 * Echo measured at ~3%% of playback (382 at out_rms 12615); default 6%% = 2x
 * margin. 0 disables the proportional term (pure absolute floor). */
#define GL_BARGE_RATIO_PCT       10
#define GL_BARGE_PLAY_WIN        4    /* frames (×32 ms = 128 ms) the adaptive floor holds the playback max, to cover the DAC/DMA + acoustic echo delay before releasing into her pauses. */
static _Atomic int      s_barge_ratio_pct = GL_BARGE_RATIO_PCT;
/* Runtime-tunable AEC gain staging (calibration 2026-06-12). Sweep live via
 * GET /api/debug/gain?mic=&ref=&vol= — no reflash needed. Defaults track the
 * #defines; mic/ref are ES7210 PGA dB, out_vol is the ES8311 0-100 scale. */
static _Atomic int      s_mic_pga_db = GL_MIC_PGA_DB;
static _Atomic int      s_mic_pga_speak_db = GL_MIC_PGA_SPEAK_DB;  /* during-SPEAKING mic gain; raise to make a barge audible over the residual echo. Runtime: /api/debug/gain?speak=N */
static _Atomic int      s_ref_pga_db = GL_REF_PGA_DB;
static _Atomic int      s_out_vol    = 100;
/* Manual-mode barge-in: server-side activityHandling. 1 = START_OF_ACTIVITY_INTERRUPTS
 * (a client activityStart on real barge cancels the model's reply — required for
 * talk-over-her barge to actually work). 0 = NO_INTERRUPTION (server ignores
 * client activity; replies never cancelled, but barge can't stop her). Default 1
 * so barge works; runtime-toggle via /api/debug/gain?interrupt=0/1 (applies on
 * the next session — restart voice to take effect). The 20 s speak-watchdog
 * makes the old spurious-cancel (4.5 s watchdog) a non-issue now. */
static _Atomic int      s_activity_interrupts = 1;
/* Local-VAD turn-commit thresholds (manual mode), runtime-tunable so hands-free
 * turn detection can be matched to the user's actual mic level live (their voice
 * was 130-256 RMS vs the old 1000 default). /api/debug/gain?vadspeech=N&vadsilence=N */
static _Atomic int      s_vad_speech_rms  = GL_VAD_SPEECH_RMS;
static _Atomic int      s_vad_silence_rms = GL_VAD_SILENCE_RMS;

#if GL_LANE_DIAG
/* Last 1 s window of raw per-lane RMS/peak, published by the capture task,
 * surfaced in the diagnostics JSON (lane_rms/lane_peak). */
static _Atomic uint16_t s_lane_rms[GL_CAPTURE_CHANNELS];
static _Atomic uint16_t s_lane_peak[GL_CAPTURE_CHANNELS];
#endif

#if GL_USE_AEC
/* AEC telemetry (P3.3): published by the capture task, read by diagnostics.
 * cost = per-frame aec_process duration percentiles over the last ~10 s
 * window (tag `aec_cost`); atten = echo attenuation during playback, raw-mic
 * RMS vs clean-mic RMS in tenths of dB (tag `aec_atten`). */
static _Atomic bool     s_aec_enabled;
static _Atomic uint32_t s_aec_frames;        /* aec_process calls this session */
static _Atomic uint32_t s_aec_cost_p50_us;
static _Atomic uint32_t s_aec_cost_p95_us;
static _Atomic int32_t  s_aec_atten_db10;
#endif

/* Raw rx-queue byte accounting (P2.2): WS task adds, session task subtracts. */
static _Atomic uint32_t s_rx_queue_bytes;
/* Tool jobs queued-but-not-finished (session task posts, worker completes). */
static _Atomic uint32_t s_tool_inflight;

/* RMS of a block of mono int16 PCM, clamped to uint16. */
static uint16_t gl_compute_rms(const int16_t *samples, size_t n)
{
    if (!samples || n == 0) {
        return 0;
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t s = samples[i];
        acc += (uint64_t)(s * s);
    }
    double rms = sqrt((double)acc / (double)n);
    if (rms > 32767.0) {
        rms = 32767.0;
    }
    return (uint16_t)rms;
}

/* ---- Helpers -------------------------------------------------------------- */

static void gl_set_state(gl_state_t st, const char *detail)
{
    if (s_gl.state == GL_STATE_SPEAKING && st == GL_STATE_LISTENING) {
        s_gl.waiting_terminal = false;
    }
    if (st == GL_STATE_LISTENING) {
        s_gl.last_audio_us = 0;
    }
    s_gl.state = st;

    /* Re-apply state-aware mic gain on the SPEAKING/LISTENING edges: loud for
     * hearing the user, quiet (GL_MIC_PGA_SPEAK_DB) for unclipping her echo. */
    if (st == GL_STATE_SPEAKING || st == GL_STATE_LISTENING) {
        gl_apply_in_gains();
    }

    /* Settle the playback waveform whenever we are not speaking; the mic level
     * is zeroed by the TX task itself when capture stops. */
    if (st != GL_STATE_SPEAKING) {
        atomic_store(&s_out_rms, 0);
    }

    /* Drive the emote face from the single state-transition point so every
     * GL_STATE has a face (Connecting/Thinking were previously faceless).
     * Face setters are declared in emote.h (display teammate's voice states). */
    switch (st) {
    case GL_STATE_CONNECTING:
        emote_set_connecting();
        break;
    case GL_STATE_LISTENING:
        emote_set_listening();
        break;
    case GL_STATE_THINKING:
        s_gl.thinking_since_us = esp_timer_get_time();
        emote_set_thinking();
        break;
    case GL_STATE_SPEAKING:
        ESP_LOGI(TAG, "gl_set_state: emote_set_speaking enter");
        emote_set_speaking();
        ESP_LOGI(TAG, "gl_set_state: emote_set_speaking done");
        break;
    case GL_STATE_IDLE:
        emote_set_voice_idle();
        break;
    case GL_STATE_READY:
    default:
        break;
    }

    emote_set_status_detail(detail ? detail : "");
}

static bool gl_ws_send_text_to(const char *json, uint32_t timeout_ms)
{
    if (!s_gl.ws_mutex) {
        return false;
    }
    int len = (int)strlen(json);
    bool ok = false;
    xSemaphoreTake(s_gl.ws_mutex, portMAX_DELAY);
    /* Snapshot the client handle INSIDE the mutex: session_cleanup NULLs
     * s_gl.ws_client under this same mutex, so destruction waits for any
     * in-flight send and later senders see NULL instead of a freed client
     * (F2 use-after-free). */
    esp_websocket_client_handle_t client = s_gl.ws_client;
    if (client && s_gl.ws_connected) {
        int sent = esp_websocket_client_send_text(client, json, len, pdMS_TO_TICKS(timeout_ms));
        ok = (sent == len);
    }
    xSemaphoreGive(s_gl.ws_mutex);
    return ok;
}

static bool gl_ws_send_text(const char *json)
{
    return gl_ws_send_text_to(json, GL_WS_TIMEOUT_MS);
}

/* Base64-encode pcm bytes into out_b64 (caller provides GL_TX_B64_BYTES+ buf) */
static bool gl_b64_encode(const uint8_t *in, size_t in_len, char *out_b64, size_t out_size, size_t *out_len)
{
    return mbedtls_base64_encode((unsigned char *)out_b64, out_size, out_len, in, in_len) == 0;
}

static esp_lcd_touch_handle_t gl_resolve_touch_handle(void *touch_h)
{
    /* Board-manager returns dev_lcd_touch_i2c_handles_t* (or dev_lcd_touch_handles_t*).
     * Both structs have esp_lcd_touch_handle_t as their first member, so dereference
     * once to get the actual driver handle rather than the wrapper struct pointer. */
    return *(esp_lcd_touch_handle_t *)touch_h;
}

static uint32_t gl_parse_audio_rate_mime(const char *mime_type)
{
    if (!mime_type) {
        return 0;
    }

    const char *p = strstr(mime_type, "rate=");
    if (!p) {
        return 0;
    }
    p += 5;

    uint32_t rate = 0;
    bool has_digit = false;
    while (*p && isdigit((unsigned char)*p)) {
        has_digit = true;
        rate = rate * 10 + (uint32_t)(*p - '0');
        p++;
    }

    return has_digit ? rate : 0;
}

static uint32_t gl_i2s_cfg_sample_rate(const periph_i2s_config_t *cfg, i2s_dir_t dir)
{
    if (!cfg) {
        return 0;
    }

    switch (cfg->mode) {
    case I2S_COMM_MODE_STD:
        if (cfg->i2s_cfg.std.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        }
        break;
    case I2S_COMM_MODE_PDM:
        if (dir == I2S_DIR_RX && cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz;
        }
        if (dir == I2S_DIR_TX && cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz;
        }
        break;
    case I2S_COMM_MODE_TDM:
        if (cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz;
        }
        break;
    default:
        break;
    }

    if (dir == I2S_DIR_RX) {
        if (cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz;
        }
        if (cfg->i2s_cfg.std.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        }
    } else {
        if (cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz;
        }
        if (cfg->i2s_cfg.std.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        }
        if (cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz;
        }
    }
    return 0;
}

static uint32_t gl_resolve_playback_rate(uint32_t model_rate)
{
    uint32_t source = model_rate ? model_rate : GL_RX_SAMPLE_RATE;
    /* The ES8311 DAC and ES7210 ADC share one I2S STD-duplex clock, so TX and RX
     * MUST hold the same rate. We now run the whole codec at 24 kHz (GL_CAP_SAMPLE_RATE
     * == the model output rate) and downsample the MIC 24->16 right after the
     * read (gl_downsample_capture_24to16). That lets the model's 24 kHz audio play
     * NATIVELY (no downsample, crisp) while the AEC/VAD/uplink pipeline still sees
     * 16 kHz. Because capture is also 24 kHz, the shared clock never has to move
     * mid-session — the old 16 kHz-lock + output-resample is gone. */
    if (s_gl.adc || s_gl.adc_raw) {
        return GL_CAP_SAMPLE_RATE;
    }
    /* Playback-only sessions (no capture path) can use the codec natively. */
    if (s_gl.dac && !s_gl.dac_codec_failed) {
        return source;
    }
    if (s_gl.dac_raw && s_gl.dac_raw_rate_hz) {
        return s_gl.dac_raw_rate_hz;
    }
    return source;
}

static uint32_t gl_audio_write_timeout_ms(size_t bytes, uint32_t sample_rate)
{
    uint32_t rate = sample_rate ? sample_rate : GL_RX_SAMPLE_RATE;
    uint32_t bytes_per_sec = rate * GL_CHANNELS * (GL_BITS / 8);
    if (!bytes_per_sec) {
        return GL_TX_CHUNK_MS + GL_AUDIO_WRITE_MARGIN_MS;
    }
    uint32_t ms = (uint32_t)((bytes * 1000ULL) / bytes_per_sec) + GL_AUDIO_WRITE_MARGIN_MS;
    if (ms < GL_TX_CHUNK_MS + GL_AUDIO_WRITE_MARGIN_MS) {
        ms = GL_TX_CHUNK_MS + GL_AUDIO_WRITE_MARGIN_MS;
    }
    return ms;
}

static uint32_t gl_clamp_rate(uint32_t rate)
{
    if (rate == 0) {
        return GL_RX_SAMPLE_RATE;
    }
    return rate;
}

/* ---- Board device handles ------------------------------------------------- */

static void gl_reset_audio_path_state(void)
{
    atomic_store(&s_playback_kill, false);   /* never carry a barge kill into a new session */
    s_gl.dac = NULL;
    s_gl.adc = NULL;
    s_gl.dac_chan = NULL;
    s_gl.adc_chan = NULL;
    s_gl.dac_raw = false;
    s_gl.adc_raw = false;
    s_gl.dac_raw_rate_hz = 0;
    s_gl.adc_raw_rate_hz = 0;
    s_gl.dac_rate = 0;
    s_gl.adc_rate = 0;
}

static esp_codec_dev_handle_t gl_extract_codec_handle(void *dev_h)
{
    if (!dev_h) {
        return NULL;
    }
    return ((dev_audio_codec_handles_t *)dev_h)->codec_dev;
}

static bool gl_acquire_codec_handles(void)
{
    /* esp_board_manager_get_device_handle() writes the device's INNER handle
     * struct pointer directly into the out-ptr (e.g. dev_audio_codec_handles_t*),
     * NOT the esp_board_device_handle_t wrapper. Reading ->device_handle off it
     * double-derefs and yields a flash-mapped junk pointer (0x420b...), which
     * crashes esp_codec_dev_open with LoadStoreError. Cast the out-ptr directly,
     * mirroring main.c's patch-0010 touch fix and the board_manager example
     * (record_and_play.c: dac_handle->codec_dev). */
    void *dac_h = NULL;
    void *adc_h = NULL;
    void *touch_h = NULL;
    void *raw_dac_h = NULL;
    void *raw_adc_h = NULL;
    void *raw_adc_cfg_h = NULL;
    void *raw_dac_cfg_h = NULL;

    gl_reset_audio_path_state();

    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &dac_h) == ESP_OK &&
        dac_h) {
        s_gl.dac = gl_extract_codec_handle(dac_h);
    }
    if (!s_gl.dac && esp_board_manager_get_device_handle("fake_audio_dac", &dac_h) == ESP_OK &&
        dac_h) {
        s_gl.dac = gl_extract_codec_handle(dac_h);
    }
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_ADC, &adc_h) == ESP_OK &&
        adc_h) {
        s_gl.adc = gl_extract_codec_handle(adc_h);
    }
    if (esp_board_periph_get_handle(GL_AUDIO_ADC_PERIPH, &raw_adc_h) == ESP_OK &&
        raw_adc_h) {
        s_gl.adc_raw = true;
        s_gl.adc_chan = (i2s_chan_handle_t)raw_adc_h;
    }
    if (esp_board_periph_get_config(GL_AUDIO_ADC_PERIPH, (void **)&raw_adc_cfg_h) == ESP_OK &&
        raw_adc_cfg_h) {
        s_gl.adc_raw_rate_hz = gl_i2s_cfg_sample_rate((const periph_i2s_config_t *)raw_adc_cfg_h,
                                                     I2S_DIR_RX);
    }
    if (esp_board_periph_get_handle(GL_AUDIO_DAC_PERIPH, &raw_dac_h) == ESP_OK &&
        raw_dac_h) {
        s_gl.dac_raw = true;
        s_gl.dac_chan = (i2s_chan_handle_t)raw_dac_h;
    }
    if (esp_board_periph_get_config(GL_AUDIO_DAC_PERIPH, (void **)&raw_dac_cfg_h) == ESP_OK &&
        raw_dac_cfg_h) {
        s_gl.dac_raw_rate_hz = gl_i2s_cfg_sample_rate((const periph_i2s_config_t *)raw_dac_cfg_h,
                                                     I2S_DIR_TX);
    }

    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_h) == ESP_OK &&
        touch_h) {
        s_gl.touch = gl_resolve_touch_handle(touch_h);
    }

    if (!s_gl.dac && !s_gl.adc && !s_gl.dac_raw && !s_gl.adc_raw) {
        gl_set_audio_error("no audio device handles");
    }

    ESP_LOGI(TAG, "Audio paths: dac=%p raw_dac=%p adc=%p raw_adc=%p touch=%p",
             (void *)s_gl.dac, (void *)s_gl.dac_chan, (void *)s_gl.adc,
             (void *)s_gl.adc_chan, (void *)s_gl.touch);
    return s_gl.dac || s_gl.adc || s_gl.dac_raw || s_gl.adc_raw;
}

static bool gl_ensure_codec_handles(const char *tag, bool require_dac, bool require_adc)
{
    bool need_dac = require_dac &&
                    !(s_gl.dac_raw || (s_gl.dac && !s_gl.dac_codec_failed));
    bool need_adc = require_adc &&
                    !(s_gl.adc_raw || (s_gl.adc && !s_gl.adc_codec_failed));

    if (!need_dac && !need_adc) {
        return true;
    }

    for (int i = 0; i < GL_CODEC_HANDLE_RETRIES; ++i) {
        if (gl_acquire_codec_handles() &&
            !need_dac && !need_adc) {
            return true;
        }
        if (!need_dac || s_gl.dac_raw || (s_gl.dac && !s_gl.dac_codec_failed)) {
            need_dac = false;
        }
        if (!need_adc || s_gl.adc_raw || (s_gl.adc && !s_gl.adc_codec_failed)) {
            need_adc = false;
        }
        if (!need_dac && !need_adc) {
            if (i > 0) {
                ESP_LOGI(TAG, "%s: audio handles ready after %d attempts",
                         tag ? tag : "codec", i + 1);
            }
            gl_clear_audio_error();
            return true;
        }

        if (i + 1 < GL_CODEC_HANDLE_RETRIES) {
            ESP_LOGW(TAG, "%s: audio handles unavailable (dac=%p raw_dac=%d adc=%p raw_adc=%d), retrying (%d/%d)",
                     tag ? tag : "codec", (void *)s_gl.dac, (int)s_gl.dac_raw,
                     (void *)s_gl.adc, (int)s_gl.adc_raw,
                     i + 1, GL_CODEC_HANDLE_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(GL_CODEC_HANDLE_RETRY_MS));
        }
    }

    if (need_dac && need_adc) {
        gl_set_audio_error("audio path incomplete (missing both input and output)");
    } else if (need_dac) {
        gl_set_audio_error("missing output path (audio_dac/fake_audio_dac or i2s_audio_out)");
    } else if (need_adc) {
        gl_set_audio_error("missing input path (audio_adc or i2s_audio_in)");
    }
    return false;
}

/* ---- I2S open/close helpers ----------------------------------------------- */

static int gl_open_dac(uint32_t sample_rate)
{
    if (!gl_ensure_codec_handles("open_dac", true, false)) {
        ESP_LOGE(TAG, "DAC handle unavailable");
        gl_set_audio_error("output path missing while opening DAC");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_gl.dac && !s_gl.dac_codec_failed) {
        /* Already open at this rate → no-op. Open at a different rate (TX 16k vs RX
         * 24k) → close first so the codec re-inits cleanly. */
        if (s_gl.dac_open) {
            if (s_gl.dac_rate == sample_rate) {
                return ESP_CODEC_DEV_OK;
            }
            esp_codec_dev_close(s_gl.dac);
            s_gl.dac_open = false;
        }
        esp_codec_dev_sample_info_t fs = {
            .sample_rate     = sample_rate,
            .channel         = GL_CHANNELS,
            .bits_per_sample = GL_BITS,
        };
        int r = esp_codec_dev_open(s_gl.dac, &fs);
        ESP_LOGI(TAG, "esp_codec_dev_open(dac, %u Hz) = %d (%s)", (unsigned)sample_rate, r, esp_err_to_name(r));
        if (r == ESP_CODEC_DEV_OK) {
            s_gl.dac_codec_failed = false;
            esp_codec_dev_set_out_mute(s_gl.dac, false);
            esp_codec_dev_set_out_vol(s_gl.dac, atomic_load(&s_out_vol));
            s_gl.dac_open = true;
            s_gl.dac_rate = sample_rate;
            return r;
        }
        s_gl.dac_codec_failed = true;
        ESP_LOGW(TAG, "DAC open failed rate=%u err=%d (%s), trying raw I2S if available",
                 (unsigned)sample_rate, r, esp_err_to_name(r));
    }

    if (s_gl.dac_raw) {
        s_gl.dac_open = true;
        /* Raw I2S clock is fixed at peripheral-config time — record the actual
         * hardware rate, not the requested one, so callers can detect mismatch
         * and resample. */
        s_gl.dac_rate = s_gl.dac_raw_rate_hz ? s_gl.dac_raw_rate_hz : sample_rate;
        return ESP_CODEC_DEV_OK;
    }

    if (s_gl.dac) {
        gl_set_audio_error("DAC open failed (codec handle)");
    }
    return ESP_ERR_INVALID_STATE;
}

/* Apply the runtime-tunable per-channel input PGA to the open ADC: MEMS mics
 * (chip MIC1/MIC2 = mask bits 0|1) and the echo reference (chip MIC3 =
 * GL_REF_CHIP_MASK_BIT). Safe from any task — set_in_channel_gain is an I2C
 * control write, off the I2S read path. Driven by s_mic_pga_db / s_ref_pga_db. */
static void gl_apply_in_gains(void)
{
    if (!s_gl.adc || s_gl.adc_codec_failed) {
        return;
    }
    /* State-aware mic gain (2026-06-13): drop to GL_MIC_PGA_SPEAK_DB while the
     * model speaks so the echo stays unclipped for the AEC; otherwise use the
     * louder, runtime-tunable listen gain so the user's voice is actually heard.
     * Re-applied from gl_set_state on every SPEAKING/LISTENING transition. */
    int mic_db = (s_gl.state == GL_STATE_SPEAKING)
                 ? atomic_load(&s_mic_pga_speak_db) : atomic_load(&s_mic_pga_db);
    esp_codec_dev_set_in_channel_gain(s_gl.adc,
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), (float)mic_db);
    esp_codec_dev_set_in_channel_gain(s_gl.adc,
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(GL_REF_CHIP_MASK_BIT), (float)atomic_load(&s_ref_pga_db));
}

static int gl_open_adc(uint32_t sample_rate)
{
    if (!gl_ensure_codec_handles("open_adc", false, true)) {
        ESP_LOGE(TAG, "ADC handle unavailable");
        gl_set_audio_error("input path missing while opening ADC");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_gl.adc && !s_gl.adc_codec_failed) {
        if (s_gl.adc_open) {
            if (s_gl.adc_rate == sample_rate) {
                return ESP_CODEC_DEV_OK;
            }
            esp_codec_dev_close(s_gl.adc);
            s_gl.adc_open = false;
        }
        esp_codec_dev_sample_info_t fs = {
            .sample_rate     = sample_rate,
            .channel         = GL_CAPTURE_CHANNELS, /* all 4 TDM lanes; SW demux (D2) */
            .bits_per_sample = GL_BITS,
            .channel_mask    = 0,                   /* never lane-pick — see GL_CAPTURE_CHANNELS */
        };
        int r = esp_codec_dev_open(s_gl.adc, &fs);
        ESP_LOGI(TAG, "esp_codec_dev_open(adc, %u Hz, %d ch) = %d (%s)",
                 (unsigned)sample_rate, GL_CAPTURE_CHANNELS, r, esp_err_to_name(r));
        if (r == ESP_CODEC_DEV_OK) {
            s_gl.adc_codec_failed = false;
            /* Per-channel PGA (D2 gain fix). es7210_open just re-applied
             * +30 dB to ALL enabled channels — including the echo-reference
             * lane, which already arrives only ≈ −23.5 dB below line-out
             * through the AEC pad; +30 dB nets ≈ +6.5 dB ABOVE line-out and
             * clips at volume. Keep 30 dB on the MEMS mics (chip MIC1/MIC2 =
             * mask bits 0/1; uplink level unchanged vs the old all-channel
             * set_in_gain), drop the ref (chip MIC3 → REG45 = mask bit 2) to
             * 0 dB. These masks are CHIP-mic indices, not buffer lanes. */
            gl_apply_in_gains();
            s_gl.adc_open = true;
            s_gl.adc_rate = sample_rate;
            return r;
        }
        s_gl.adc_codec_failed = true;
        ESP_LOGW(TAG, "ADC open failed rate=%u err=%d (%s), trying raw I2S if available",
                 (unsigned)sample_rate, r, esp_err_to_name(r));
    }

    if (s_gl.adc_raw) {
        s_gl.adc_open = true;
        s_gl.adc_rate = sample_rate;
        return ESP_CODEC_DEV_OK;
    }

    if (s_gl.adc) {
        gl_set_audio_error("ADC open failed (codec handle)");
    }
    return ESP_ERR_INVALID_STATE;
}

static void gl_close_dac(void)
{
    if (s_gl.dac && s_gl.dac_open && !s_gl.dac_codec_failed) {
        esp_codec_dev_close(s_gl.dac);
    }
    s_gl.dac_open = false;
    s_gl.dac_rate = 0;
}

static void gl_close_adc(void)
{
    if (s_gl.adc && s_gl.adc_open && !s_gl.adc_codec_failed) {
        esp_codec_dev_close(s_gl.adc);
    }
    s_gl.adc_open = false;
    s_gl.adc_rate = 0;
}

/* Session-long codec (P2.4/F11): the DAC stays open on the shared session
 * clock; turn transitions toggle mute instead of close/reopen. Muting while
 * listening also prevents I2S TX underrun artifacts from reaching the
 * speaker between turns. Raw-I2S fallback has no mute — acceptable, the
 * codec path is the real hardware path on this board. */
static void gl_dac_mute(bool mute)
{
    if (s_gl.dac && s_gl.dac_open && !s_gl.dac_codec_failed) {
        esp_codec_dev_set_out_mute(s_gl.dac, mute);
    }
}

/* ---- Decoded-PCM ring (session task → playback feeder, P2.1/P2.2) --------- */

/* Drop all buffered playback. Bumping the epoch aborts any producer blocked
 * in gl_pcm_ring_write. SESSION TASK ONLY. */
static void gl_pcm_ring_flush(void)
{
    if (!s_gl.ring_mutex || !s_gl.pcm_ring) {
        return;
    }
    xSemaphoreTake(s_gl.ring_mutex, portMAX_DELAY);
    s_gl.pcm_ring_head  = 0;
    s_gl.pcm_ring_tail  = 0;
    s_gl.pcm_ring_bytes = 0;
    s_gl.pcm_ring_epoch++;
    xSemaphoreGive(s_gl.ring_mutex);
}

/* Allocate the ring for a session. Degrades by halving instead of failing
 * (P2.2/F9) — a smaller ring just means earlier producer backpressure, never
 * a crash. A full-size ring is deliberately retained across sessions (see
 * session_cleanup): emote clip churn fragments PSRAM, and a released 1 MB
 * block can never be re-allocated later. SESSION TASK ONLY. */
static void gl_pcm_ring_alloc(void)
{
    if (s_gl.pcm_ring) {
        /* Retained from the previous session (or a feeder that refused to
         * park): reuse in place. */
        gl_pcm_ring_flush();
        return;
    }
    size_t want = GL_PCM_RING_BYTES;
    while (want >= GL_PCM_RING_MIN_BYTES) {
        s_gl.pcm_ring = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_gl.pcm_ring) {
            break;
        }
        ESP_LOGE(TAG, "PCM ring: %u B alloc failed (largest PSRAM block %u), halving",
                 (unsigned)want,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        want /= 2;
    }
    if (!s_gl.pcm_ring) {
        ESP_LOGE(TAG, "PCM ring: no allocation >= %u B — degraded synchronous playback",
                 (unsigned)GL_PCM_RING_MIN_BYTES);
        s_gl.pcm_ring_cap = 0;
        return;
    }
    s_gl.pcm_ring_cap   = want;
    s_gl.pcm_ring_head  = 0;
    s_gl.pcm_ring_tail  = 0;
    s_gl.pcm_ring_bytes = 0;
    ESP_LOGI(TAG, "PCM ring: %u B (~%u s at the session clock)",
             (unsigned)want,
             (unsigned)(want / (GL_TX_SAMPLE_RATE * GL_CHANNELS * (GL_BITS / 8))));
}

/* SESSION TASK ONLY, and only after the feeder is known parked. */
static void gl_pcm_ring_free(void)
{
    if (!s_gl.pcm_ring) {
        return;
    }
    if (s_gl.feeder_task) {
        /* Never free under a live reader; the next session reuses (flushes) it. */
        ESP_LOGE(TAG, "PCM ring: feeder still alive, keeping ring allocated");
        return;
    }
    heap_caps_free(s_gl.pcm_ring);
    s_gl.pcm_ring       = NULL;
    s_gl.pcm_ring_cap   = 0;
    s_gl.pcm_ring_bytes = 0;
}

/* Copy len bytes into the ring. SESSION TASK ONLY. When the ring is full
 * (> ~32 s buffered — rare) this blocks in 10 ms steps, but keeps consuming
 * the cmd queue so a tap interrupt stays responsive mid-burst; a flush
 * (epoch bump), stop request or session end aborts the write. Returns false
 * if any bytes were dropped. */
static bool gl_pcm_ring_write(const uint8_t *data, size_t len)
{
    if (!s_gl.pcm_ring || !s_gl.ring_mutex) {
        return false;
    }
    uint32_t epoch0 = s_gl.pcm_ring_epoch;
    size_t   off = 0;
    while (off < len) {
        xSemaphoreTake(s_gl.ring_mutex, portMAX_DELAY);
        size_t space = s_gl.pcm_ring_cap - s_gl.pcm_ring_bytes;
        size_t n = len - off;
        if (n > space) {
            n = space;
        }
        if (n) {
            size_t first = s_gl.pcm_ring_cap - s_gl.pcm_ring_head;
            if (first > n) {
                first = n;
            }
            memcpy(s_gl.pcm_ring + s_gl.pcm_ring_head, data + off, first);
            if (n > first) {
                memcpy(s_gl.pcm_ring, data + off + first, n - first);
            }
            s_gl.pcm_ring_head = (s_gl.pcm_ring_head + n) % s_gl.pcm_ring_cap;
            s_gl.pcm_ring_bytes += n;
        }
        xSemaphoreGive(s_gl.ring_mutex);
        off += n;
        if (off == len) {
            return true;
        }
        /* Ring full: pump requests while waiting so GL_CMD_INTERRUPT can
         * flush us free (P3.1 latency target). */
        gl_process_cmd_queue();
        if (s_gl.stop_requested || !s_gl.session_active ||
            s_gl.pcm_ring_epoch != epoch0) {
            s_gl.pcm_ring_drop_bytes += (uint32_t)(len - off);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

/* Pop up to max bytes. FEEDER TASK ONLY. Returns 0 after ~wait_ms idle. */
static size_t gl_pcm_ring_read(uint8_t *out, size_t max, uint32_t wait_ms)
{
    if (!s_gl.pcm_ring || !s_gl.ring_mutex) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        return 0;
    }
    uint32_t waited = 0;
    for (;;) {
        xSemaphoreTake(s_gl.ring_mutex, portMAX_DELAY);
        size_t n = s_gl.pcm_ring_bytes;
        if (n > max) {
            n = max;
        }
        if (n) {
            size_t first = s_gl.pcm_ring_cap - s_gl.pcm_ring_tail;
            if (first > n) {
                first = n;
            }
            memcpy(out, s_gl.pcm_ring + s_gl.pcm_ring_tail, first);
            if (n > first) {
                memcpy(out + first, s_gl.pcm_ring, n - first);
            }
            s_gl.pcm_ring_tail = (s_gl.pcm_ring_tail + n) % s_gl.pcm_ring_cap;
            s_gl.pcm_ring_bytes -= n;
        }
        xSemaphoreGive(s_gl.ring_mutex);
        if (n || waited >= wait_ms) {
            return n;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

/* True while decoded audio is still buffered or mid-write to the DAC. */
static bool gl_playback_pending(void)
{
    return (s_gl.pcm_ring && s_gl.pcm_ring_bytes > 0) || s_gl.feeder_writing;
}

/* ---- JSON send helpers ---------------------------------------------------- */

/* Declare one Gemini function-call tool taking a single required string arg.
 * Keeps gl_send_setup readable as the JarvisMCP skill set grows. */
static void gl_add_str_fn(cJSON *fdecls, const char *name, const char *desc,
                          const char *arg, const char *arg_desc)
{
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "description", desc);
    cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *p = cJSON_AddObjectToObject(props, arg);
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", arg_desc);
    cJSON *reqd = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(reqd, cJSON_CreateString(arg));
    cJSON_AddItemToArray(fdecls, fn);
}

static bool gl_send_setup(void)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model", GEMINI_LIVE_MODEL);

    /* Google Search grounding — lets the model fetch live data (weather, news,
     * facts) and speak a grounded answer with NO client-side tool handling. Raw
     * Live-API setup form: "tools":[{"googleSearch":{}}] (proto JSON accepts the
     * camelCase form; the cookbook uses google_search — both parse). */
    cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
    cJSON *gs = cJSON_CreateObject();
    cJSON_AddItemToObject(gs, "googleSearch", cJSON_CreateObject());
    cJSON_AddItemToArray(tools, gs);

    /* Function calling → JarvisMCP /act bridge. DISCRIMINATOR STEP: one declared
     * function alongside googleSearch, to verify the two tool types coexist in
     * setup for this model (watch for setupComplete + a toolCall frame). The
     * handler + HTTPS call to the gateway are added once coexistence is proven. */
    cJSON *fd_tool = cJSON_CreateObject();
    cJSON *fdecls  = cJSON_AddArrayToObject(fd_tool, "functionDeclarations");
    cJSON *fn      = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", "crypto_price");
    cJSON_AddStringToObject(fn, "description",
                            "Get the current market price of a cryptocurrency by symbol or name.");
    cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props  = cJSON_AddObjectToObject(params, "properties");
    cJSON *sym    = cJSON_AddObjectToObject(props, "symbol");
    cJSON_AddStringToObject(sym, "type", "string");
    cJSON_AddStringToObject(sym, "description", "Coin symbol or name, e.g. bitcoin, ethereum, solana");
    cJSON *reqd = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(reqd, cJSON_CreateString("symbol"));
    cJSON_AddItemToArray(fdecls, fn);

    /* JarvisMCP-backed skills. Each maps to a jarvis.* SDK call in
     * gl_run_tool_call (tool worker task) → /act gateway. Company brain
     * (memory) + knowledge. */
    gl_add_str_fn(fdecls, "recall_memory",
                  "Search Jarvis's own long-term memory and knowledge base for facts, notes, decisions, and context from past sessions. Use for 'what do you know about...', 'do you remember...', or anything personal or project-specific.",
                  "query", "What to look up, in natural language.");
    gl_add_str_fn(fdecls, "remember",
                  "Save a fact or note to Jarvis's long-term memory so it persists across sessions. Use when the user asks you to remember something.",
                  "note", "The fact to store, as a clear standalone sentence.");
    gl_add_str_fn(fdecls, "wikipedia",
                  "Look up a concise factual summary of a topic, person, place, or thing.",
                  "topic", "The subject to summarise.");
    gl_add_str_fn(fdecls, "country_info",
                  "Get facts about a country: capital, population, currencies, region.",
                  "country", "Country name, e.g. Japan.");
    gl_add_str_fn(fdecls, "current_time",
                  "Get the current date and time for a timezone.",
                  "timezone", "IANA timezone like America/New_York, Europe/London, or UTC.");
    gl_add_str_fn(fdecls, "ask_jarvis",
                  "Escape hatch for capabilities the other tools don't cover: run a JavaScript expression against the JarvisMCP SDK and return its result. Available: jarvis.crypto(coin), jarvis.weather(lat,lon), jarvis.exchange(base,targets), jarvis.wiki(q), jarvis.memory.search({query,area:'all'}), jarvis.dns(domain), jarvis.country(name), jarvis.time(tz). The value MUST be one statement starting with 'return await', e.g. return await jarvis.crypto('bitcoin').",
                  "code", "A JavaScript expression starting with 'return await jarvis.'.");

    /* ask_user — show tappable choice arcs on the round display and wait for the
     * user to TAP an answer (the VISION's "asks you questions you tap to
     * answer"). Two args: a short `question` (string) and `options` (array of 2..6
     * short strings). The tool worker shows the arc via ui_layer_show_choice,
     * waits (bounded) for the tap, and returns the chosen label as the
     * functionResponse — zero new transport. Declared inline because it takes an
     * array arg that gl_add_str_fn (single-string) can't express. */
    {
        cJSON *au = cJSON_CreateObject();
        cJSON_AddStringToObject(au, "name", "ask_user");
        cJSON_AddStringToObject(au, "description",
            "Ask the user a question they answer by TAPPING one of a few options on the device's round touchscreen. Use this when you need a decision or a choice from a small fixed set (2 to 6 options) and want a reliable tapped answer instead of relying on speech. Returns the label the user tapped.");
        cJSON *aup = cJSON_AddObjectToObject(au, "parameters");
        cJSON_AddStringToObject(aup, "type", "object");
        cJSON *aprops = cJSON_AddObjectToObject(aup, "properties");
        cJSON *qp = cJSON_AddObjectToObject(aprops, "question");
        cJSON_AddStringToObject(qp, "type", "string");
        cJSON_AddStringToObject(qp, "description", "The short question shown at the top of the screen.");
        cJSON *op = cJSON_AddObjectToObject(aprops, "options");
        cJSON_AddStringToObject(op, "type", "array");
        cJSON_AddStringToObject(op, "description", "2 to 6 short answer labels the user can tap.");
        cJSON *items = cJSON_AddObjectToObject(op, "items");
        cJSON_AddStringToObject(items, "type", "string");
        cJSON *aureq = cJSON_AddArrayToObject(aup, "required");
        cJSON_AddItemToArray(aureq, cJSON_CreateString("question"));
        cJSON_AddItemToArray(aureq, cJSON_CreateString("options"));
        cJSON_AddItemToArray(fdecls, au);
    }

    cJSON_AddItemToArray(tools, fd_tool);

    cJSON *gc = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *rm = cJSON_AddArrayToObject(gc, "responseModalities");
    cJSON_AddItemToArray(rm, cJSON_CreateString("AUDIO"));
    /* thinkingLevel accepted by both gemini-2.5-flash-native-audio-latest and 3.x models.
     * "minimal" = lowest latency, appropriate for voice. Confirmed: setupComplete received. */
    cJSON *tc = cJSON_AddObjectToObject(gc, "thinkingConfig");
    cJSON_AddStringToObject(tc, "thinkingLevel", "minimal");

    /* Hands-free conversation via server-side VAD (P3.4, aec-barge-in.md §4).
     * The server detects speech start/end on the continuously streamed,
     * AEC-cleaned mic and ends the user's turn after `silenceDurationMs` of
     * quiet — no client activity signals. START_SENSITIVITY_HIGH catches
     * speech onset fast; END_SENSITIVITY_LOW + 700 ms silence tolerates
     * mid-sentence pauses (matches the field-tuned local-VAD hang).
     * prefixPaddingMs 200 is the anti-false-positive guard against post-AEC
     * residual echo — raise toward 300 if self-interruptions appear; its
     * latency cost is non-critical because the LOCAL barge detector owns the
     * perceived playback stop (hybrid, D5). activityHandling
     * START_OF_ACTIVITY_INTERRUPTS makes detected speech alone cancel
     * generation server-side — the documented barge-in path; the resulting
     * `interrupted` frame is our confirmation signal (P3.2 flush path). */
    cJSON *ric = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(ric, "automaticActivityDetection");
    if (GL_USE_SERVER_VAD) {
        cJSON_AddBoolToObject(aad, "disabled", false);
        cJSON_AddStringToObject(aad, "startOfSpeechSensitivity", "START_SENSITIVITY_HIGH");
        cJSON_AddStringToObject(aad, "endOfSpeechSensitivity",   "END_SENSITIVITY_LOW");
        cJSON_AddNumberToObject(aad, "prefixPaddingMs", 200);
        cJSON_AddNumberToObject(aad, "silenceDurationMs", 700);
        cJSON_AddStringToObject(ric, "activityHandling", "START_OF_ACTIVITY_INTERRUPTS");
    } else {
        cJSON_AddBoolToObject(aad, "disabled", true);
        /* Manual mode: the LOCAL barge detector owns barge-in — it flushes
         * playback + mutes the DAC locally (gl_interrupt_playback) and re-opens
         * the activity on real speech. The server must therefore NOT treat a
         * client activityStart as a barge-in: the default
         * START_OF_ACTIVITY_INTERRUPTS made any resume-path activityStart (the
         * input watchdog, or a stale resume) cancel the model's pending reply
         * before it produced audio (observed mute, 2026-06-13 — a no-audio turn
         * stuck in LISTENING with audio_parts=0). That spurious source was the
         * 4.5 s watchdog; it is now 20 s and disarms on first audio, so the only
         * activityStart that lands during a reply is a REAL barge. Default back
         * to START_OF_ACTIVITY_INTERRUPTS so barge-in actually stops her (the
         * local detector kills LOCAL audio, but only this makes the SERVER stop
         * generating). Runtime-tunable: /api/debug/gain?interrupt=0 reverts to
         * NO_INTERRUPTION if reply-cancellation regressions reappear. */
        cJSON_AddStringToObject(ric, "activityHandling",
                                atomic_load(&s_activity_interrupts)
                                    ? "START_OF_ACTIVITY_INTERRUPTS"
                                    : "NO_INTERRUPTION");
    }

    /* Ask the server for an input transcript stream so we can flip the face to
     * THINKING the instant VAD decides the user's turn ended — closes the
     * "silent gap" between speech-end and audio-reply that made the device
     * look frozen and tempted the user to tap-to-abort mid-response. */
    cJSON_AddItemToObject(setup, "inputAudioTranscription", cJSON_CreateObject());

    /* System instruction = JARVIS's persistent on-device self (identity +
     * recent memory from /sdcard/brain). Fail-soft: jarvis_brain_load_context
     * always returns a usable, NUL-terminated persona — the built-in default if
     * the SD store is unavailable — so this never breaks the WSS setup. */
    char persona[2048];
    jarvis_brain_load_context(persona, sizeof persona);
    /* Tell the model, in-voice, what it can actually do — so it offers its tools
     * naturally instead of claiming it has no skills. */
    size_t plen = strlen(persona);
    snprintf(persona + plen, sizeof(persona) - plen,
             "\n\nYou are not limited to conversation — you have tools and should use them rather than guess. "
             "recall_memory / remember access your own long-term memory; wikipedia, country_info, and current_time "
             "answer factual lookups; crypto_price gives live coin prices; ask_jarvis runs anything else via the "
             "JarvisMCP SDK; and you have live web search for general facts and news. When the user asks what you "
             "can do, mention these capabilities concretely.");

    cJSON *si   = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *parts = cJSON_AddArrayToObject(si, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", persona);
    cJSON_AddItemToArray(parts, part);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    bool ok = gl_ws_send_text(json);
    free(json);
    return ok;
}

static bool gl_send_text_turn(const char *text)
{
    cJSON *root   = cJSON_CreateObject();
    cJSON *cc     = cJSON_AddObjectToObject(root, "clientContent");
    cJSON *turns  = cJSON_AddArrayToObject(cc, "turns");
    cJSON *turn   = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "role", "user");
    cJSON *parts  = cJSON_AddArrayToObject(turn, "parts");
    cJSON *part   = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(turns, turn);
    cJSON_AddBoolToObject(cc, "turnComplete", true);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    bool ok = gl_ws_send_text(json);
    free(json);
    return ok;
}

static bool gl_send_activity_start(void)
{
    /* With server VAD enabled, the server detects activity on the streamed
     * audio itself; client-side activityStart is rejected/ignored. Return true
     * so the caller's bookkeeping still treats the "activity" as open. */
    if (GL_USE_SERVER_VAD) {
        return true;
    }
    return gl_ws_send_text("{\"realtimeInput\":{\"activityStart\":{}}}");
}

static bool gl_send_activity_end(void)
{
    if (GL_USE_SERVER_VAD) {
        return true;
    }
    return gl_ws_send_text("{\"realtimeInput\":{\"activityEnd\":{}}}");
}

/* Auto-VAD stream-contract obligation (P3.4, aec-barge-in.md §4): any uplink
 * pause >GL_STREAM_END_PAUSE_MS must flush the server's cached audio with
 * audioStreamEnd, or a stale tail bleeds into the next utterance. Called from
 * the CAPTURE task while the uplink is idle — uses the short mic timeout so
 * Wi-Fi backpressure can never park the capture loop. No-op in manual mode
 * (activityEnd already closes the stream there). */
static bool gl_send_audio_stream_end(void)
{
    if (!GL_USE_SERVER_VAD) {
        return true;
    }
    return gl_ws_send_text_to("{\"realtimeInput\":{\"audioStreamEnd\":true}}",
                              GL_WS_MIC_TIMEOUT_MS);
}

static bool gl_begin_audio_activity(const char *reason)
{
    if (s_gl.activity_open) {
        return true;
    }
    bool ok = gl_send_activity_start();
    if (ok) {
        s_gl.activity_open = true;
        ESP_LOGI(TAG, "Audio activity start sent (%s)", reason ? reason : "listen");
    } else {
        s_gl.tx_send_failures++;
        ESP_LOGW(TAG, "Audio activity start send failed (%s)", reason ? reason : "listen");
    }
    return ok;
}

static bool gl_end_audio_activity(const char *reason)
{
    bool ok = true;
    if (s_gl.activity_open) {
        ok = gl_send_activity_end();
        if (ok) {
            ESP_LOGI(TAG, "Audio activity end sent (%s)", reason ? reason : "end_input");
        } else {
            s_gl.tx_send_failures++;
            ESP_LOGW(TAG, "Audio activity end send failed (%s)", reason ? reason : "end_input");
        }
    }
    s_gl.activity_open = false;
    return ok;
}

/* Send one PCM frame as a realtimeInput audio chunk */
static bool gl_send_audio_frame(const uint8_t *pcm, size_t len)
{
    static char b64[GL_TX_B64_BYTES];
    size_t b64_len = 0;

    if (!gl_b64_encode(pcm, len, b64, sizeof(b64), &b64_len)) {
        return false;
    }
    b64[b64_len] = '\0';

    /*
     * realtimeInput.audio.data — see GEMINI_AUDIO_KEY note at top.
     * Build the JSON without cJSON to avoid heap allocation in the hot path.
     */
    static char frame_json[GL_TX_B64_BYTES + 128];
    int n = snprintf(frame_json, sizeof(frame_json),
                     "{\"realtimeInput\":{\"%s\":{\"mimeType\":\"audio/pcm;rate=%d\","
                     "\"data\":\"%s\"}}}",
                     GEMINI_AUDIO_KEY, GL_TX_SAMPLE_RATE, b64);
    if (n <= 0 || n >= (int)sizeof(frame_json)) {
        return false;
    }

    /* Mic frames use the short timeout (see GL_WS_MIC_TIMEOUT_MS) so Wi-Fi
     * backpressure cannot park the TX loop beyond the stop wait (P1.2). */
    return gl_ws_send_text_to(frame_json, GL_WS_MIC_TIMEOUT_MS);
}

/* Convert PCM16 sample-rate using linear interpolation (fixed-point 16.16).
 * Nearest-neighbour was audibly harsh for the 24 kHz -> 16 kHz speech path
 * (ratio 1.5 drops every third sample cold → metallic aliasing); linear
 * interpolation costs one multiply per sample and removes most of it. Still
 * single-pass, single output allocation, low latency. */
static int16_t *gl_resample_pcm16_linear(const int16_t *in, size_t nsamp_in,
                                         uint32_t sample_rate_in, uint32_t sample_rate_out,
                                         size_t *nsamp_out)
{
    if (!in || nsamp_in == 0 || sample_rate_in == 0 || sample_rate_out == 0 ||
        sample_rate_in == sample_rate_out) {
        return NULL;
    }

    size_t out_samples = (size_t)(((uint64_t)nsamp_in * (uint64_t)sample_rate_out +
                                  (uint64_t)sample_rate_in - 1) / (uint64_t)sample_rate_in);
    if (out_samples == 0) {
        return NULL;
    }

    int16_t *out = heap_caps_malloc(out_samples * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        return NULL;
    }

    uint64_t step = ((uint64_t)sample_rate_in << 16) / (uint64_t)sample_rate_out;
    uint64_t pos = 0;
    for (size_t i = 0; i < out_samples; ++i) {
        size_t idx = (size_t)(pos >> 16);
        uint32_t frac = (uint32_t)(pos & 0xFFFF);   /* 16-bit fractional position */
        if (idx >= nsamp_in - 1) {
            out[i] = in[nsamp_in - 1];
        } else {
            int32_t a = in[idx];
            int32_t b = in[idx + 1];
            out[i] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
        }
        pos += step;
    }

    if (nsamp_out) {
        *nsamp_out = out_samples;
    }
    return out;
}

/* Downsample one 4-lane interleaved s16 capture frame from the 24 kHz codec
 * clock to the 16 kHz the AEC/VAD/uplink pipeline expects (3:2 ratio,
 * GL_CAP_SAMPLES_PER_CHUNK -> GL_TX_SAMPLES_PER_CHUNK). Linear interp per lane,
 * fixed in/out buffers (no malloc — runs every 32 ms capture frame). Lane order
 * is preserved so the downstream demux is byte-identical to the old 16 kHz read.
 * Native 24 kHz playback is the whole point; the mic just rides the same clock. */
static void gl_downsample_capture_24to16(const int16_t *in4, int16_t *out4)
{
    for (size_t j = 0; j < GL_TX_SAMPLES_PER_CHUNK; ++j) {
        uint32_t pos = j * 3u;            /* in-frame position = j * 1.5 = (j*3)/2 */
        size_t   lo  = pos >> 1;          /* floor(j * 1.5) */
        size_t   hi  = lo + 1u;
        if (hi >= GL_CAP_SAMPLES_PER_CHUNK) {
            hi = GL_CAP_SAMPLES_PER_CHUNK - 1u;
        }
        int32_t frac = (int32_t)(pos & 1u) << 14;   /* 0 or 0.5 in Q15 (0 / 16384) */
        for (int l = 0; l < GL_CAPTURE_CHANNELS; ++l) {
            int32_t a = in4[lo * GL_CAPTURE_CHANNELS + l];
            int32_t b = in4[hi * GL_CAPTURE_CHANNELS + l];
            out4[j * GL_CAPTURE_CHANNELS + l] = (int16_t)(a + (((b - a) * frac) >> 15));
        }
    }
}

static bool gl_extract_audio_data_chunk(cJSON *audio, const char **out_data, uint32_t *out_rate)
{
    if (!audio || !out_data) {
        return false;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(audio, "data");
    cJSON *mime = cJSON_GetObjectItemCaseSensitive(audio, "mimeType");
    if (!cJSON_IsString(data)) {
        cJSON *inline_obj = gl_get_object_compat(audio, "inlineData", "inline_data");
        if (!inline_obj) {
            return false;
        }
        data = cJSON_GetObjectItemCaseSensitive(inline_obj, "data");
        mime = cJSON_GetObjectItemCaseSensitive(inline_obj, "mimeType");
    }

    if (!cJSON_IsString(data)) {
        return false;
    }

    *out_data = data->valuestring;
    if (out_rate) {
        if (cJSON_IsString(mime)) {
            *out_rate = gl_parse_audio_rate_mime(mime->valuestring);
        } else {
            *out_rate = 0;
        }
    }
    return true;
}

/* ---- Audio playback ------------------------------------------------------- */

/* Decode base64 PCM and play back to speaker (codec or raw I2S path). */
static void gl_play_audio_b64(const char *b64_str, uint32_t sample_rate)
{
    /* Require a handle, but NOT an already-open DAC: the first audio chunks of
     * a turn race enter_speaking's gl_open_dac(), and dropping them clipped the
     * start of every utterance. gl_open_dac() below opens on demand. */
    if (!s_gl.dac && !s_gl.dac_raw) {
        gl_set_audio_error("playback: DAC unavailable");
        ESP_LOGW(TAG, "Speaking: no DAC handle, dropping audio chunk");
        return;
    }

    uint32_t model_rate = gl_clamp_rate(sample_rate);
    uint32_t requested_rate = gl_resolve_playback_rate(model_rate);
    s_gl.last_audio_mime_rate = model_rate;
    if (requested_rate == 0) {
        requested_rate = model_rate;
    }

    size_t b64_len = strlen(b64_str);
    size_t pcm_max = (b64_len / 4) * 3 + 4;
    uint8_t *pcm = heap_caps_malloc(pcm_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        /* Transient low-water dip (not a fragmentation ceiling): yield once so the
         * AEC/feeder/rwave-clip churn frees a block, then retry. Dropping the chunk
         * clips the start of an utterance, so only give up if the retry also fails. */
        vTaskDelay(pdMS_TO_TICKS(GL_PCM_DECODE_RETRY_MS));
        pcm = heap_caps_malloc(pcm_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!pcm) {
            ESP_LOGE(TAG, "OOM for PCM decode buf (%u B, retry failed)", (unsigned)pcm_max);
            return;
        }
    }
    if (gl_open_dac(requested_rate) != ESP_CODEC_DEV_OK) {
        free(pcm);
        return;
    }
    /* Use the rate the DAC actually opened at (codec adapts; raw is fixed). */
    uint32_t playback_rate = s_gl.dac_rate ? s_gl.dac_rate : requested_rate;
    if (model_rate != playback_rate) {
        s_gl.rate_mismatch_chunks++;
        ESP_LOGD(TAG, "audio rate mismatch: model=%u playback=%u; resampling",
                 model_rate, playback_rate);
    }

    size_t pcm_len = 0;
    if (mbedtls_base64_decode(pcm, pcm_max, &pcm_len,
                              (const unsigned char *)b64_str, b64_len) != 0) {
        ESP_LOGE(TAG, "base64 decode failed");
        free(pcm);
        return;
    }
    if (pcm_len == 0 || (pcm_len & 0x1)) {
        ESP_LOGW(TAG, "PCM decode invalid len=%u", (unsigned)pcm_len);
        free(pcm);
        return;
    }

    /* Make-up gain + soft-knee limiter (see GL_OUT_GAIN comment). Also measure
     * the pre-gain peak/RMS and log it so the real signal level stays visible. */
    int16_t *s16 = (int16_t *)pcm;
    size_t   nsamp = pcm_len / sizeof(int16_t);
    int32_t  peak_in = 0;
    int64_t  sumsq = 0;
    for (size_t i = 0; i < nsamp; i++) {
        int32_t s = s16[i];
        int32_t a0 = s < 0 ? -s : s;
        if (a0 > peak_in) {
            peak_in = a0;
        }
        sumsq += (int64_t)s * s;

        int32_t v = s * GL_OUT_GAIN;
        int32_t a = v < 0 ? -v : v;
        if (a > GL_LIMIT_KNEE) {
            a = GL_LIMIT_KNEE + ((a - GL_LIMIT_KNEE) >> 2);   /* 4:1 above knee */
            if (a > 32767) {
                a = 32767;
            }
            v = (v < 0) ? -a : a;
        }
        s16[i] = (int16_t)v;
    }
    if (nsamp > 10) {
        int32_t rms_in = (int32_t)sqrt((double)(sumsq / (int64_t)nsamp));
        ESP_LOGD(TAG, "audio level: peak=%d rms=%d (full-scale=32767) gain=%d samples=%u",
                 (int)peak_in, (int)rms_in, GL_OUT_GAIN, (unsigned)nsamp);
    }

    int16_t  *out_pcm = s16;
    size_t    out_samples = nsamp;
    size_t    out_bytes = pcm_len;
    int16_t  *resampled = NULL;

    /* Resample whenever the model rate differs from the rate the DAC actually
     * runs at — codec path included. (Previously only the raw path resampled,
     * so codec playback at a mismatched rate played at the wrong speed.) */
    if (model_rate != playback_rate) {
        resampled = gl_resample_pcm16_linear((const int16_t *)s16, nsamp,
                                             model_rate, playback_rate, &out_samples);
        if (resampled) {
            out_pcm = resampled;
            out_bytes = out_samples * sizeof(int16_t);
        }
    }

    if (s_gl.stop_requested || !s_gl.session_active) {
        if (resampled) {
            free(resampled);
        }
        free(pcm);
        return;
    }

    if (s_gl.pcm_ring && s_gl.feeder_task) {
        /* P2.1 (F8): hand the conditioned PCM to the feeder via the ring —
         * the session task never blocks on the DAC, so the rx_queue drains
         * at decode speed. The feeder publishes s_out_rms as chunks actually
         * play, keeping the face in sync with the speaker. */
        if (!gl_pcm_ring_write((const uint8_t *)out_pcm, out_bytes)) {
            ESP_LOGD(TAG, "PCM ring write aborted (flush/stop)");
        }
    } else {
        /* Degraded path (ring allocation or feeder create failed): the old
         * synchronous write. Audio still works; it just blocks this task. */
        atomic_store(&s_out_rms, gl_compute_rms(out_pcm, out_samples));
        if (s_gl.first_audio_pending) {
            s_gl.first_audio_pending = false;
            uint32_t ms = (uint32_t)((esp_timer_get_time() - s_gl.speak_enter_us) / 1000);
            s_gl.last_first_audio_ms = ms;
            ESP_LOGI(TAG, "first-audio latency: %u ms (enter_speaking -> DAC, sync path)",
                     (unsigned)ms);
        }
        if (s_gl.dac && !s_gl.dac_codec_failed) {
            int wr = esp_codec_dev_write(s_gl.dac, out_pcm, (int)out_bytes);
            if (wr != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "DAC write failed: codec rc=%d bytes=%u rate=%u",
                         wr, (unsigned)out_bytes, (unsigned)playback_rate);
            }
        } else if (s_gl.dac_raw) {
            size_t bytes_written = 0;
            uint32_t timeout_ms = gl_audio_write_timeout_ms(out_bytes, playback_rate);
            esp_err_t err = i2s_channel_write(s_gl.dac_chan, out_pcm, out_bytes, &bytes_written,
                                              pdMS_TO_TICKS(timeout_ms));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "DAC write failed: i2s err=%d bytes=%u timeout_ms=%u rate=%u",
                         (int)err, (unsigned)out_bytes, (unsigned)timeout_ms, (unsigned)playback_rate);
            } else if (bytes_written != out_bytes) {
                ESP_LOGW(TAG, "DAC write short: %u != %u timeout_ms=%u",
                         (unsigned)bytes_written, (unsigned)out_bytes, (unsigned)timeout_ms);
            }
        }
    }

    if (resampled) {
        free(resampled);
    }
    free(pcm);
}

/* ---- Playback feeder task (P2.1/F8) ---------------------------------------
 * The ONLY job of this task: pop PCM from the ring and run the blocking
 * esp_codec_dev_write. No JSON, no base64, no network, no lifecycle. */

static void gl_playback_feeder_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[GL_FEEDER_CHUNK_BYTES];

    ESP_LOGI(TAG, "Feeder: started (ring %u B)", (unsigned)s_gl.pcm_ring_cap);
    while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_FEEDER_STOP)) {
        if (atomic_load(&s_playback_kill)) {
            /* Barge fast-kill: the capture task already muted the codec. Stop
             * feeding the DMA so no buffered audio survives into the next turn;
             * gl_enter_speaking clears the flag + un-mutes for the new reply. */
            atomic_store(&s_out_rms, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t got = gl_pcm_ring_read(chunk, sizeof(chunk), 50);
        if (!got) {
            continue;
        }

        if (s_gl.first_audio_pending) {
            s_gl.first_audio_pending = false;
            uint32_t ms = (uint32_t)((esp_timer_get_time() - s_gl.speak_enter_us) / 1000);
            s_gl.last_first_audio_ms = ms;
            ESP_LOGI(TAG, "first-audio latency: %u ms (enter_speaking -> DAC feed)",
                     (unsigned)ms);
        }

        /* Playback level for the SPEAKING waveform — what is actually playing. */
        atomic_store(&s_out_rms, gl_compute_rms((const int16_t *)chunk,
                                                got / sizeof(int16_t)));

        s_gl.feeder_writing = true;
        if (s_gl.dac && s_gl.dac_open && !s_gl.dac_codec_failed) {
            int wr = esp_codec_dev_write(s_gl.dac, chunk, (int)got);
            if (wr != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "Feeder: DAC write failed rc=%d bytes=%u", wr, (unsigned)got);
            }
        } else if (s_gl.dac_raw && s_gl.dac_chan) {
            size_t written = 0;
            uint32_t timeout_ms = gl_audio_write_timeout_ms(got, s_gl.dac_rate);
            esp_err_t err = i2s_channel_write(s_gl.dac_chan, chunk, got, &written,
                                              pdMS_TO_TICKS(timeout_ms));
            if (err != ESP_OK || written != got) {
                ESP_LOGW(TAG, "Feeder: raw write %s (%u/%u B)",
                         esp_err_to_name(err), (unsigned)written, (unsigned)got);
            }
        }
        s_gl.feeder_writing = false;
    }

    s_gl.feeder_writing = false;
    atomic_store(&s_out_rms, 0);
    ESP_LOGI(TAG, "Feeder: stopped");
    xEventGroupSetBits(s_gl.ev, GL_BIT_FEEDER_DONE);
    if (s_gl.feeder_task == xTaskGetCurrentTaskHandle()) {
        s_gl.feeder_task = NULL;
    }
    claw_task_delete(NULL);
}

/* SESSION TASK ONLY. */
static void gl_start_feeder_task(void)
{
    if (s_gl.feeder_task) {
        return;     /* a stuck survivor is still serving the (reused) ring */
    }
    if (!s_gl.pcm_ring) {
        return;     /* degraded sync mode — gl_play_audio_b64 writes directly */
    }
    xEventGroupClearBits(s_gl.ev, GL_BIT_FEEDER_STOP | GL_BIT_FEEDER_DONE);
    static const claw_task_config_t feeder_cfg = {
        .name         = "gl_pcm_feeder",
        .stack_size   = 6144,
        .priority     = 5,
        /* Pin to core 0 (Wi-Fi/sender core), AWAY from gl_audio_tx which runs the
         * ~18 ms AEC at priority 6 on core 1. With NO_AFFINITY the feeder could
         * land on core 1 and be starved by the AEC mid-frame -> the DAC DMA
         * underruns -> the playback "hiccup". Core 0's Wi-Fi bursts are short and
         * the sender is the same priority (5), so they timeshare without
         * starving the ~80 ms feeder cadence. */
        .core_id      = 0,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    if (claw_task_create(&feeder_cfg, gl_playback_feeder_task, NULL, &s_gl.feeder_task) != pdPASS) {
        s_gl.feeder_task = NULL;
        ESP_LOGE(TAG, "Feeder: task create failed — degraded synchronous playback");
    }
}

/* SESSION TASK ONLY. Same no-force-delete doctrine as gl_stop_tx_task. */
static bool gl_stop_feeder_task(void)
{
    if (!s_gl.feeder_task) {
        return true;
    }
    xEventGroupSetBits(s_gl.ev, GL_BIT_FEEDER_STOP);
    EventBits_t bits = xEventGroupWaitBits(s_gl.ev, GL_BIT_FEEDER_DONE, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(GL_FEEDER_STOP_WAIT_MS));
    if (!(bits & GL_BIT_FEEDER_DONE)) {
        if (!s_gl.feeder_task) {
            return true;
        }
        ESP_LOGE(TAG, "Feeder: stop timed out; leaving task parked (no force-delete)");
        return false;
    }
    s_gl.feeder_task = NULL;
    return true;
}

/* ---- Audio TX task (Phase 4) --------------------------------------------- */

#if GL_USE_AEC
static int gl_u16_cmp(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* One 16-byte-aligned mono frame buffer for aec_process (esp_aec.h warns the
 * mic/ref/out buffers must be 16-byte aligned, non-interleaved int16).
 * Internal RAM preferred for speed (3 KB total across the three buffers),
 * PSRAM acceptable; NULL means the caller degrades to no-AEC. */
static int16_t *gl_aec_buf_alloc(void)
{
    int16_t *p = heap_caps_aligned_alloc(16, GL_TX_PCM_BYTES,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_aligned_alloc(16, GL_TX_PCM_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return p;
}
#endif

static void gl_audio_tx_task(void *arg)
{
    (void)arg;
    /* Raw 4-lane TDM frames straight off the codec (D2): 512 frames × 4 lanes
     * × 16-bit per 32 ms read, demuxed into the mono mic chunk below. The raw
     * I2S fallback path (no codec handle at all) still reads mono into pcm —
     * the board YAML config governs that path, not the 4-ch codec open. */
    static int16_t raw4[GL_TX_SAMPLES_PER_CHUNK * GL_CAPTURE_CHANNELS];      /* 16 kHz frame (downsampled) — feeds the unchanged demux */
    static int16_t raw4_cap[GL_CAP_SAMPLES_PER_CHUNK * GL_CAPTURE_CHANNELS]; /* 24 kHz codec read, downsampled into raw4 */
    static uint8_t pcm[GL_TX_PCM_BYTES];

#if GL_LANE_DIAG
    /* TEMPORARY per-lane accumulators — one log line per second (50 frames). */
    uint64_t lane_sumsq[GL_CAPTURE_CHANNELS] = {0};
    int32_t  lane_pk[GL_CAPTURE_CHANNELS]    = {0};
    uint32_t lane_diag_frames = 0;
#endif
#if GL_USE_LOCAL_VAD
    /* Local VAD accumulators (32 ms frames) — TX-task-private, reset every
     * capture cycle. */
    uint32_t vad_speech_frames  = 0;
    uint32_t vad_silence_frames = 0;
    bool     vad_speech_seen    = false;
#endif
    /* Barge detector state (P3.4/D5): consecutive above-threshold frames on
     * the AEC-cleaned post-gain mic during SPEAKING. barge_posted makes the
     * detector one-shot per SPEAKING turn (re-armed when state leaves
     * SPEAKING) so a slow cmd-queue drain cannot stack duplicate interrupts. */
    uint32_t barge_frames   = 0;
    bool     barge_posted   = false;
    int64_t  barge_onset_us = 0;
    /* Sliding-window max of the playback level for the adaptive barge floor. The
     * echo at the mic lags the playback by the DAC/DMA + acoustic delay
     * (~60-100 ms), so the instantaneous s_out_rms collapses while the echo tail
     * is still loud (seen: mic=736 echo while play had already dropped to 141).
     * Holding the max over the last GL_BARGE_PLAY_WIN frames (~128 ms) keeps the
     * floor up exactly long enough to cover the tail, then drops cleanly into her
     * pauses where a soft barge can fire (an exponential decay over-held: 25% of
     * a ~13000 peak stays high for ~400 ms and blocks pause-barges). */
    uint16_t play_hist[GL_BARGE_PLAY_WIN] = {0};
    unsigned play_idx       = 0;
    /* Auto-VAD uplink bookkeeping (P3.4): when the last frame was queued for
     * send, and the once-per-pause audioStreamEnd latch. */
    int64_t  last_uplink_us  = 0;
    bool     stream_end_sent = false;
#if GL_USE_AEC
    /* P3.3 instrumentation: per-frame aec_process cost window (p50/p95 logged
     * once per ~10 s, tag aec_cost) and the playback-window echo-attenuation
     * accumulators (raw vs clean mic RMS, tag aec_atten). The window array is
     * consumed (sorted in place) at every rollover. */
    static uint16_t aec_cost_win[GL_AEC_STAT_FRAMES];
    uint32_t aec_win_n         = 0;
    uint64_t atten_raw_sumsq   = 0;
    uint64_t atten_clean_sumsq = 0;
    uint32_t atten_frames      = 0;
#endif
    /* The ADC stays open across turns (P2.4), so the I2S RX DMA accumulates
     * stale audio — including speaker echo — while capture is paused. Dump
     * ~220 ms of frames on every pause→listen transition so neither the VAD
     * nor the server sees it. */
    int flush_frames = GL_CAPTURE_FLUSH_FRAMES;

    /* Codec lifetime is owned by the session task. This worker only reads from
     * the already-open ADC so teardown cannot double-close codec/I2S handles
     * from two tasks. */
    ESP_LOGI(TAG, "Audio TX: started (24kHz capture -> 16kHz pipeline; native 24kHz playback)");
    if (!s_gl.adc && !s_gl.adc_raw) {
        gl_set_audio_error("TX: missing ADC path");
        ESP_LOGE(TAG, "Audio TX: missing ADC handle");
        xEventGroupSetBits(s_gl.ev, GL_BIT_TX_DONE);
        if (s_gl.tx_task == xTaskGetCurrentTaskHandle()) {
            s_gl.tx_task = NULL;
        }
        claw_task_delete(NULL);
    }

#if GL_USE_AEC
    /* AEC engine (D3): created per capture-task generation (= per session
     * under P2.4) so the echo filter converges once and stays converged
     * across turn boundaries. Working state prefers internal RAM (~31 KB per
     * the esp-sr FD_LOW_COST budget), falls back to PSRAM, then degrades to
     * un-cancelled capture — never crashes (task rule + R3). */
    aec_handle_t *aec       = NULL;
    int16_t      *aec_mic   = NULL;
    int16_t      *aec_ref   = NULL;
    int16_t      *aec_clean = NULL;
    {
        size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t spi_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        aec_config_t aec_cfg = {
            .mic_num       = 1,
            .ref_num       = 1,
            .out_num       = 1,
            .filter_length = GL_AEC_FILTER_LENGTH,
            .sample_rate   = GL_TX_SAMPLE_RATE,    /* aec_create: must be 16000 */
            .caps          = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
            .mode          = GL_AEC_MODE,
            .nlp_level     = AEC_NLP_LEVEL_AGGR,   /* manual mode: strongest echo suppression (local barge off, so no double-talk-ducking concern) */
        };
        aec = aec_create_from_config(&aec_cfg);
        if (!aec) {
            ESP_LOGW(TAG, "AEC: internal-RAM create failed, retrying in PSRAM");
            aec_cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
            aec = aec_create_from_config(&aec_cfg);
        }
        if (aec) {
            int chunk = aec_get_chunksize(aec);
            if (chunk != GL_TX_SAMPLES_PER_CHUNK) {
                /* The whole 32 ms pipeline assumes one TDM read = one AEC
                 * chunk; a different chunk size would need rebuffering. */
                ESP_LOGE(TAG, "AEC: chunksize %d != %d, disabling",
                         chunk, (int)GL_TX_SAMPLES_PER_CHUNK);
                aec_destroy(aec);
                aec = NULL;
            }
        }
        if (aec) {
            aec_mic   = gl_aec_buf_alloc();
            aec_ref   = gl_aec_buf_alloc();
            aec_clean = gl_aec_buf_alloc();
            if (!aec_mic || !aec_ref || !aec_clean) {
                ESP_LOGE(TAG, "AEC: frame buffer alloc failed, disabling");
                heap_caps_free(aec_mic);
                heap_caps_free(aec_ref);
                heap_caps_free(aec_clean);
                aec_mic = aec_ref = aec_clean = NULL;
                aec_destroy(aec);
                aec = NULL;
            }
        }
        if (aec) {
            atomic_store(&s_aec_frames, 0);
            atomic_store(&s_aec_cost_p50_us, 0);
            atomic_store(&s_aec_cost_p95_us, 0);
            atomic_store(&s_aec_atten_db10, 0);
            atomic_store(&s_aec_enabled, true);
            /* Heap delta at create (P0.4 doctrine) — verifies the §3 budget. */
            ESP_LOGI(TAG, "AEC: created (%s, filter=%d, chunk=%d) heap delta int=-%d B psram=-%d B",
                     aec_get_mode_string(GL_AEC_MODE), GL_AEC_FILTER_LENGTH,
                     (int)GL_TX_SAMPLES_PER_CHUNK,
                     (int)(int_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     (int)(spi_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        } else {
            /* Degrade, never crash: capture continues un-cancelled (pre-P3.3
             * behaviour); barge-in stays tap-only. */
            ESP_LOGE(TAG, "AEC: unavailable — capture continues without echo cancellation");
        }
    }
#endif

    while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP)) {
        const gl_state_t st  = s_gl.state;
        const bool listening = (st == GL_STATE_LISTENING);
        const bool speaking  = (st == GL_STATE_SPEAKING);

        /* Auto-VAD stream contract (P3.4): if the uplink has gone quiet for
         * >1 s — capture paused, or reading without sending — flush the
         * server's cached audio ONCE per pause. Checked every iteration; all
         * loop paths cycle in <= ~42 ms (10 ms paused delay / 32 ms read). */
        if (GL_USE_SERVER_VAD && !stream_end_sent && last_uplink_us &&
            s_gl.session_active && s_gl.ws_connected &&
            (esp_timer_get_time() - last_uplink_us) >
                (int64_t)GL_STREAM_END_PAUSE_MS * 1000) {
            if (gl_send_audio_stream_end()) {
                s_gl.audio_stream_end_hits++;
                ESP_LOGI(TAG, "audioStreamEnd sent (uplink paused > %d ms)",
                         (int)GL_STREAM_END_PAUSE_MS);
            }
            stream_end_sent = true;   /* once per pause, even on send failure */
        }

        /* P3.4: the mic stays OPEN during SPEAKING — capture → AEC → clean
         * frames keep flowing while the feeder plays, so the AEC stays
         * converged AND the clean signal feeds the barge detector (plus, in
         * server-VAD mode, the continuous uplink). What changes per state is
         * the POLICY applied to the clean frame below, not the read itself.
         * The SPEAKING read needs the 4-lane codec path (the echo reference);
         * the raw-I2S mono fallback has no ref, so it captures only while
         * LISTENING (and THINKING under auto VAD, where the DAC is muted and
         * there is no echo to cancel). RX-only codec reads are duplex-safe
         * against the feeder's TX writes (independent I2S channels on the
         * shared clock). */
        const bool codec_capture = s_gl.adc && s_gl.adc_open && !s_gl.adc_codec_failed;
        bool speaking_read = speaking && codec_capture;
#if GL_USE_AEC
        speaking_read = speaking_read && (aec != NULL || GL_LANE_DIAG);
#else
        speaking_read = speaking_read && GL_LANE_DIAG;
#endif
        /* Auto VAD streams continuously across turn boundaries: THINKING
         * (text turn sent, reply pending) keeps capturing so server VAD hears
         * the user change their mind. Manual mode keeps the protocol pause
         * there (frames after activityEnd are stale). */
        const bool thinking_read = GL_USE_SERVER_VAD && st == GL_STATE_THINKING &&
                                   (codec_capture || s_gl.adc_raw);
        if (!listening) {
#if GL_USE_LOCAL_VAD
            /* Turn-commit VAD state is meaningless outside LISTENING: clear
             * it so a stale "speech seen" cannot insta-commit the next
             * listening segment. */
            vad_speech_frames  = 0;
            vad_silence_frames = 0;
            vad_speech_seen    = false;
#endif
            if (!speaking_read && !thinking_read) {
                /* Genuinely paused — no reads happen, so the I2S RX DMA
                 * accumulates stale audio: arm the flush dump for whenever
                 * reads resume. Continuous LISTENING↔SPEAKING transitions
                 * never pass through here, so a barge resume keeps the live
                 * audio instead of dumping the user's first 224 ms. */
                flush_frames = GL_CAPTURE_FLUSH_FRAMES;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }
        if (!speaking) {
            /* Barge detection is SPEAKING-only; re-arm across turns. */
            barge_frames = 0;
            barge_posted = false;
        }

        if (xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP) {
            break;
        }

        int r = ESP_FAIL;
        bool four_lane = false;
        if (s_gl.adc && s_gl.adc_open && !s_gl.adc_codec_failed) {
            /* 4-lane TDM read (D2): measured buffer order is [REF][MIC][NC][MIC]
             * (see GL_MIC_LANE / GL_REF_LANE), confirmed by the lane_rms diag. */
            r = esp_codec_dev_read(s_gl.adc, raw4_cap, GL_CAP_RAW_BYTES);
            if (r == ESP_CODEC_DEV_OK) {
                /* Codec now runs at 24 kHz (for native 24 kHz playback). Downsample
                 * the 4-lane frame 24->16 so the demux + AEC + VAD + uplink below
                 * are byte-identical to the old native-16 kHz read. */
                gl_downsample_capture_24to16(raw4_cap, raw4);
                s_gl.tx_codec_reads++;
                four_lane = true;
            }
        }
        if (r != ESP_CODEC_DEV_OK && s_gl.adc_raw) {
            size_t bytes_read = 0;
            esp_err_t read_err = i2s_channel_read(s_gl.adc_chan, pcm, GL_TX_PCM_BYTES,
                                                  &bytes_read, pdMS_TO_TICKS(GL_I2S_READ_TIMEOUT_MS));
            if (read_err != ESP_OK || bytes_read != GL_TX_PCM_BYTES) {
                s_gl.tx_read_failures++;
                vTaskDelay(pdMS_TO_TICKS(GL_TX_CHUNK_MS));
                continue;
            }
            r = ESP_CODEC_DEV_OK;
            s_gl.tx_raw_reads++;
        }
        if (r != ESP_CODEC_DEV_OK) {
            s_gl.tx_read_failures++;
            vTaskDelay(pdMS_TO_TICKS(GL_TX_CHUNK_MS));
            continue;
        }

        if (flush_frames > 0) {
            /* Stale RX-DMA backlog from a genuinely paused window — dump it
             * BEFORE it can reach the AEC (stale mic against the current ref
             * would briefly perturb the converged filter) or the uplink. */
            flush_frames--;
            continue;
        }

        /* Mono uplink frame for this iteration: AEC-cleaned when the engine
         * is live, raw demuxed mic otherwise (and on the raw-I2S fallback
         * path, where the mono read above filled pcm directly). */
        int16_t *frame = (int16_t *)pcm;
        if (four_lane) {
            /* Demux mic + hardware echo-ref out of the interleaved TDM frame
             * using the MEASURED buffer-lane order ([REF][MIC][NC][MIC] — see
             * GL_MIC_LANE / GL_REF_LANE). */
#if GL_USE_AEC
            /* Run the canceller only while the model is SPEAKING — that is the
             * only time there is echo to cancel. During LISTENING it would burn
             * ~37% of core 1 on silence and starve the Wi-Fi/sender path
             * (2026-06-12: contributor to transport_poll_write(0) deaths). The
             * else branch passes the raw demuxed mic for local VAD. */
            if (aec && speaking) {
                uint64_t raw_sumsq = 0;   /* pre-AEC mic energy, for aec_atten */
                for (size_t i = 0; i < GL_TX_SAMPLES_PER_CHUNK; ++i) {
                    int16_t m = raw4[i * GL_CAPTURE_CHANNELS + GL_MIC_LANE];
                    aec_mic[i] = m;
                    aec_ref[i] = raw4[i * GL_CAPTURE_CHANNELS + GL_REF_LANE];
                    raw_sumsq += (uint64_t)((int32_t)m * (int32_t)m);
                }
                /* D4: synchronous cancellation, one 512-sample chunk per
                 * read. The 6x digital gain + soft-knee run AFTER this, on
                 * the clean mic only — never on the ref, never pre-AEC. */
                int64_t aec_t0 = esp_timer_get_time();
                aec_process(aec, aec_mic, aec_ref, aec_clean);
                uint32_t cost_us = (uint32_t)(esp_timer_get_time() - aec_t0);
                frame = aec_clean;
                atomic_fetch_add(&s_aec_frames, 1);
                if (aec_win_n < GL_AEC_STAT_FRAMES) {
                    aec_cost_win[aec_win_n++] =
                        (cost_us > 0xFFFF) ? 0xFFFF : (uint16_t)cost_us;
                }
                if (speaking) {
                    /* SPEAKING read — echo present: feed the attenuation
                     * estimate (raw-mic vs clean-mic RMS, P3.3 gate).
                     * THINKING reads (auto VAD) are excluded: the DAC is
                     * muted there, so they would dilute the estimate. */
                    uint64_t clean_sumsq = 0;
                    for (size_t i = 0; i < GL_TX_SAMPLES_PER_CHUNK; ++i) {
                        int32_t c = aec_clean[i];
                        clean_sumsq += (uint64_t)(c * c);
                    }
                    atten_raw_sumsq   += raw_sumsq;
                    atten_clean_sumsq += clean_sumsq;
                    atten_frames++;
                }
                if (aec_win_n >= GL_AEC_STAT_FRAMES) {
                    /* ~10 s of processed frames: publish cost percentiles. */
                    qsort(aec_cost_win, aec_win_n, sizeof(aec_cost_win[0]),
                          gl_u16_cmp);
                    uint16_t p50 = aec_cost_win[aec_win_n / 2];
                    uint16_t p95 = aec_cost_win[(aec_win_n * 95) / 100];
                    atomic_store(&s_aec_cost_p50_us, p50);
                    atomic_store(&s_aec_cost_p95_us, p95);
                    ESP_LOGI(TAG, "aec_cost: p50=%u us p95=%u us per %d ms frame (%u frames)",
                             (unsigned)p50, (unsigned)p95, GL_TX_CHUNK_MS,
                             (unsigned)aec_win_n);
                    if (p95 > GL_AEC_COST_GATE_US) {
                        /* R3 CPU gate: if this fires sustained, flip
                         * GL_AEC_MODE to AEC_MODE_SR_HIGH_PERF (linear-only,
                         * lighter) and re-measure. */
                        ESP_LOGW(TAG, "aec_cost: p95 %u us exceeds the %u us gate (R3) — consider AEC_MODE_SR_HIGH_PERF",
                                 (unsigned)p95, (unsigned)GL_AEC_COST_GATE_US);
                    }
                    if (atten_frames >= GL_AEC_ATTEN_MIN_FRAMES) {
                        double nsamp = (double)atten_frames *
                                       (double)GL_TX_SAMPLES_PER_CHUNK;
                        double raw_rms   = sqrt((double)atten_raw_sumsq / nsamp);
                        double clean_rms = sqrt((double)atten_clean_sumsq / nsamp);
                        double atten_db  = (raw_rms > 1.0 && clean_rms > 1.0)
                                           ? 20.0 * log10(raw_rms / clean_rms)
                                           : 0.0;
                        int32_t db10 = (int32_t)(atten_db * 10.0);
                        atomic_store(&s_aec_atten_db10, db10);
                        ESP_LOGI(TAG, "aec_atten: raw_rms=%d clean_rms=%d atten=%d.%u dB (%u playback frames)",
                                 (int)raw_rms, (int)clean_rms,
                                 (int)(db10 / 10),
                                 (unsigned)((db10 < 0 ? -db10 : db10) % 10),
                                 (unsigned)atten_frames);
                    }
                    aec_win_n         = 0;
                    atten_raw_sumsq   = 0;
                    atten_clean_sumsq = 0;
                    atten_frames      = 0;
                }
            } else
#endif
            {
                /* Degraded path (AEC unavailable): raw mic lane straight
                 * through, pre-P3.3 behaviour. */
                for (size_t i = 0; i < GL_TX_SAMPLES_PER_CHUNK; ++i) {
                    frame[i] = raw4[i * GL_CAPTURE_CHANNELS + GL_MIC_LANE];
                }
            }
#if GL_LANE_DIAG
            for (size_t i = 0; i < GL_TX_SAMPLES_PER_CHUNK * GL_CAPTURE_CHANNELS; ) {
                for (int l = 0; l < GL_CAPTURE_CHANNELS; ++l, ++i) {
                    int32_t s = raw4[i];
                    lane_sumsq[l] += (uint64_t)((int64_t)s * (int64_t)s);
                    int32_t a = s < 0 ? -s : s;
                    if (a > lane_pk[l]) {
                        lane_pk[l] = a;
                    }
                }
            }
            if (++lane_diag_frames >= 1000 / GL_TX_CHUNK_MS) {  /* once per second */
                const uint32_t n = lane_diag_frames * GL_TX_SAMPLES_PER_CHUNK;
                uint16_t rms[GL_CAPTURE_CHANNELS];
                uint16_t pk[GL_CAPTURE_CHANNELS];
                for (int l = 0; l < GL_CAPTURE_CHANNELS; ++l) {
                    double v = sqrt((double)lane_sumsq[l] / (double)n);
                    rms[l] = (v > 32767.0) ? 32767 : (uint16_t)v;
                    pk[l]  = (lane_pk[l] > 32767) ? 32767 : (uint16_t)lane_pk[l];
                    atomic_store(&s_lane_rms[l], rms[l]);
                    atomic_store(&s_lane_peak[l], pk[l]);
                }
                ESP_LOGI(TAG, "lane_rms: state=%s rms=[%u %u %u %u] peak=[%u %u %u %u] (expected [MIC1][MIC2][REF][NC])",
                         gl_state_name(s_gl.state),
                         rms[0], rms[1], rms[2], rms[3],
                         pk[0], pk[1], pk[2], pk[3]);
                memset(lane_sumsq, 0, sizeof(lane_sumsq));
                memset(lane_pk, 0, sizeof(lane_pk));
                lane_diag_frames = 0;
            }
#endif
        }

        /* AEC engine liveness for this frame — gates the barge detector and
         * the during-SPEAKING uplink (raw echo must never reach either). */
        bool aec_live = false;
#if GL_USE_AEC
        aec_live = (aec != NULL) && four_lane;
#endif

        /* Digital mic-gain stage — runs on the AEC-cleaned mic only, after
         * cancellation (D4: the gain + knee are nonlinear and would break AEC
         * linearity if applied pre-AEC or to the ref). ES7210 analog gain is
         * already maxed (30 dB); still, normal-conversation RMS off this board
         * sits ~50-300 raw, below the server VAD threshold for reliable
         * speech detection. A 6x lift with 4:1 soft-knee above 24000 keeps
         * loud speech from clipping while making quiet speech audible to the
         * server. Both the visual mic_rms and the sent PCM see the gained
         * signal. Applied in EVERY captured state (P3.4) so the barge
         * detector and the auto-VAD uplink see the same calibrated level the
         * LISTENING thresholds were tuned on. */
        {
            int16_t *s = frame;
            const size_t n = GL_TX_PCM_BYTES / sizeof(int16_t);
            const int32_t knee = 24000;
            const int32_t gain = 6;
            for (size_t i = 0; i < n; ++i) {
                int32_t v = (int32_t)s[i] * gain;
                int32_t a = v < 0 ? -v : v;
                if (a > knee) {
                    a = knee + ((a - knee) >> 2);
                    if (a > 32767) a = 32767;
                    v = (v < 0) ? -a : a;
                }
                s[i] = (int16_t)v;
            }
        }

        /* Publish mic level for the LISTENING waveform (and the barge floor
         * during SPEAKING — the display uses out_rms there, so no conflict). */
        uint16_t mic_rms = gl_compute_rms(frame,
                                          GL_TX_PCM_BYTES / sizeof(int16_t));
        atomic_store(&s_mic_rms, mic_rms);

        /* Local barge detector (P3.4/D5): RMS VAD on the AEC-cleaned,
         * post-gain mic during SPEAKING only. GL_BARGE_LATCH_FRAMES (64 ms)
         * of sustained speech post the existing INTERRUPT command — the
         * session task flushes the PCM ring, mutes the DAC and resumes
         * LISTENING (manual mode: + activityStart via the resume path). In
         * auto mode we do NOT wait for the server: it hears the same speech
         * on the continuous uplink, cancels generation, truncates history
         * and sends `interrupted` as confirmation. One-shot per SPEAKING
         * turn (barge_posted); AEC-degraded capture never barges — raw echo
         * would self-trigger (R4), so barge-in stays tap-only there. */
        if (speaking) {
            uint16_t barge_thr = atomic_load(&s_barge_rms);
            /* Guard window after SPEAKING entry: the mic-gain drop (24->6 dB)
             * is an async codec write and the AEC is still re-converging, so
             * the opening frames can carry an uncancelled echo transient. Hold
             * the detector OFF until s_barge_guard_ms past speak_enter_us so
             * that transient can't latch a phantom self-barge (R4). */
            const bool barge_guarded =
                (esp_timer_get_time() - s_gl.speak_enter_us) <
                (int64_t)atomic_load(&s_barge_guard_ms) * 1000;
            /* Adaptive floor: raise the threshold in proportion to what she is
             * currently playing, so her own echo residual (which scales with
             * playback) can't trip it, while a voice independent of the playback
             * still stands out. Falls back to the absolute floor in her pauses
             * (out_rms ~0) so a soft barge there still fires. */
            uint16_t play_now = atomic_load(&s_out_rms);
            play_hist[play_idx] = play_now;
            play_idx = (play_idx + 1u) % GL_BARGE_PLAY_WIN;
            uint16_t play_peak = 0;
            for (unsigned k = 0; k < GL_BARGE_PLAY_WIN; ++k) {
                if (play_hist[k] > play_peak) play_peak = play_hist[k];
            }
            int ratio = atomic_load(&s_barge_ratio_pct);
            uint16_t prop_floor = ratio > 0
                ? (uint16_t)(((uint32_t)play_peak * (uint32_t)ratio) / 100u)
                : 0;
            uint16_t eff_thr = barge_thr > prop_floor ? barge_thr : prop_floor;
            if (!aec_live || barge_thr == 0 || barge_posted || barge_guarded) {
                barge_frames = 0;
            } else if (mic_rms >= eff_thr) {
                if (barge_frames == 0) {
                    /* Speech onset ≈ start of this 32 ms frame. */
                    barge_onset_us = esp_timer_get_time() -
                                     (int64_t)GL_TX_CHUNK_MS * 1000;
                }
                barge_frames++;
                if (barge_frames >= GL_BARGE_LATCH_FRAMES) {
                    s_gl.barge_onset_us = barge_onset_us;
                    if (gl_post_cmd(GL_CMD_INTERRUPT, NULL) == ESP_OK) {
                        s_gl.barge_hits++;
                        barge_posted = true;
                        /* Fast kill: don't wait for the session task to service
                         * the cmd (it queues behind PCM decode). Silence the
                         * codec NOW and tell the feeder to stop feeding the DMA
                         * — the perceived stop lands within one feeder chunk
                         * instead of 135-262 ms later. The session task still
                         * does the ring flush + resume when it gets the cmd. */
                        atomic_store(&s_playback_kill, true);
                        gl_dac_mute(true);
                        ESP_LOGI(TAG, "Barge-in: mic=%u >= eff_thr=%u (floor=%u prop=%u peak=%u play=%u) %u frames -> interrupt",
                                 (unsigned)mic_rms, (unsigned)eff_thr,
                                 (unsigned)barge_thr, (unsigned)prop_floor,
                                 (unsigned)play_peak, (unsigned)play_now,
                                 (unsigned)barge_frames);
                    }
                    /* post failed (queue full): latch resets, retries in
                     * another GL_BARGE_LATCH_FRAMES of sustained speech. */
                    barge_frames = 0;
                }
            } else {
                barge_frames = 0;
            }
        }

#if GL_USE_LOCAL_VAD
        /* Hands-free turn commit — MANUAL MODE ONLY (P3.4/D5): under server
         * VAD the server owns end-of-turn on the continuous stream; a local
         * END_INPUT would race it and pause the uplink mid-protocol. Kept
         * fully compiled as the GL_USE_SERVER_VAD=0 fallback.
         * Detect speech, then GL_VAD_HANG_FRAMES of trailing silence. Counted
         * in 32 ms capture frames (D4 rechunk) so the thresholds track the
         * cadence by construction. Runs here for precise per-frame timing but
         * only *requests* the commit — the session task performs the
         * end_input lifecycle, since this task cannot stop itself
         * (gl_stop_tx_task waits on GL_BIT_TX_DONE). The request rides the
         * same cmd_queue the HTTP and tap paths use; a duplicate commit is
         * harmless because the consumer re-checks state (THINKING after the
         * first one → no-op). The dead zone between SILENCE_RMS and
         * SPEECH_RMS holds the current state, so mid-sentence dips don't end
         * the turn early. */
        if (!GL_USE_SERVER_VAD && s_gl.state == GL_STATE_LISTENING) {
            if (mic_rms >= (uint16_t)atomic_load(&s_vad_speech_rms)) {
                vad_speech_frames++;
                vad_silence_frames = 0;
                if (vad_speech_frames >= GL_VAD_MIN_SPEECH_FRAMES) {
                    vad_speech_seen = true;
                }
            } else if (mic_rms < (uint16_t)atomic_load(&s_vad_silence_rms)) {
                if (vad_speech_seen) {
                    vad_silence_frames++;
                    if (vad_silence_frames >= GL_VAD_HANG_FRAMES) {
                        ESP_LOGI(TAG, "Local VAD: end of speech (%ums speech, %ums silence) -> commit turn",
                                 (unsigned)(vad_speech_frames * GL_TX_CHUNK_MS),
                                 (unsigned)(vad_silence_frames * GL_TX_CHUNK_MS));
                        if (gl_post_cmd(GL_CMD_END_INPUT, NULL) == ESP_OK) {
                            /* Reset NOW, not on the next LISTENING entry: with
                             * stale speech_seen + silence_frames the very next
                             * capture frame re-fired a second commit (seen
                             * live: two "end of speech" logs one frame apart →
                             * double end_input on one turn). */
                            vad_speech_frames  = 0;
                            vad_silence_frames = 0;
                            vad_speech_seen    = false;
                        }
                        /* post failed (queue full): keep the accumulators so
                         * the next silent frame retries the commit. */
                    }
                } else if (vad_speech_frames > 0) {
                    /* Genuine silence before any utterance latched: decay the
                     * accumulator so only *sustained* speech reaches
                     * MIN_SPEECH_FRAMES. Without this, scattered noise spikes
                     * accumulate monotonically and fire a phantom empty turn
                     * during a quiet room. */
                    vad_speech_frames--;
                }
            }
            /* dead zone (SILENCE_RMS..SPEECH_RMS): hold state, no add/decay. */
        }
#endif

        /* tx-abort: a stop was requested while we were reading (F1 / P1.2). */
        if (xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP) {
            break;
        }

        /* Uplink policy (P3.4/D5) — state re-read so a turn that ended while
         * we were reading stales the frame:
         *  - auto VAD: stream the clean frame CONTINUOUSLY — LISTENING,
         *    THINKING and SPEAKING alike. The server needs the audio during
         *    playback to detect barge-in and truncate history at the right
         *    point. SPEAKING uplink additionally requires a live AEC (raw
         *    echo would self-interrupt, R4) — without it the frame is held
         *    back and the >1 s stream-end latch above keeps the protocol
         *    honest.
         *  - manual mode: LISTENING only. During SPEAKING the clean frames
         *    feed the barge detector but are NOT sent; a barge trigger
         *    re-opens the activity (activityStart) via interrupt→resume and
         *    frames then flow as normal LISTENING uplink. */
        bool uplink;
        if (GL_USE_SERVER_VAD) {
            uplink = s_gl.session_active &&
                     (s_gl.state == GL_STATE_LISTENING ||
                      s_gl.state == GL_STATE_THINKING ||
                      (s_gl.state == GL_STATE_SPEAKING && aec_live));
        } else {
            /* manual mode: stream ACTIVE speech only, never idle silence.
             * A silent LISTENING uplink has no server-side consumer once the
             * turn ends, so the TCP send buffer backs up until the socket write
             * returns 0 (transport_poll_write(0)) and the WS tears the session
             * down — observed ~7.5 s into every post-turn listen (2026-06-12).
             * The local VAD above still sees every frame (commit timing intact);
             * vad_speech_seen gives the mid-utterance hangover and the energy
             * test catches onset before the VAD latches. */
            uplink = (s_gl.state == GL_STATE_LISTENING) &&
                     (vad_speech_seen || mic_rms >= (uint16_t)atomic_load(&s_vad_silence_rms));
        }
        if (!uplink || !s_gl.tx_frame_queue) {
            continue;
        }
        /* Decoupled send (P2.3/F10): hand the frame to the sender task via a
         * bounded queue. Wi-Fi backpressure fills the queue and we drop the
         * OLDEST frame, keeping capture cadence + VAD timing intact. Drops
         * land in tx_send_failures (they are failures to deliver). */
        if (xQueueSend(s_gl.tx_frame_queue, frame, 0) != pdTRUE) {
            static uint8_t drop_scratch[GL_TX_PCM_BYTES];
            if (xQueueReceive(s_gl.tx_frame_queue, drop_scratch, 0) == pdTRUE) {
                s_gl.tx_send_failures++;
            }
            if (xQueueSend(s_gl.tx_frame_queue, frame, 0) != pdTRUE) {
                s_gl.tx_send_failures++;
                continue;   /* nothing entered the queue this iteration */
            }
        }
        last_uplink_us  = esp_timer_get_time();
        stream_end_sent = false;
    }

    /* Mic is quiet once capture stops. */
    atomic_store(&s_mic_rms, 0);

#if GL_USE_AEC
    if (aec) {
        size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t spi_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        atomic_store(&s_aec_enabled, false);
        aec_destroy(aec);
        aec = NULL;
        heap_caps_free(aec_mic);
        heap_caps_free(aec_ref);
        heap_caps_free(aec_clean);
        aec_mic = aec_ref = aec_clean = NULL;
        /* Heap delta at destroy (P0.4 doctrine): should mirror the create
         * delta — a shrinking return value across sessions = an AEC leak. */
        ESP_LOGI(TAG, "AEC: destroyed, heap delta int=+%d B psram=+%d B",
                 (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - int_before),
                 (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - spi_before));
    }
#endif

    ESP_LOGI(TAG, "Audio TX: stopped");
    xEventGroupSetBits(s_gl.ev, GL_BIT_TX_DONE);
    if (s_gl.tx_task == xTaskGetCurrentTaskHandle()) {
        s_gl.tx_task = NULL;
    }
    claw_task_delete(NULL);
}

/* Drains the capture queue into the WS (P2.3/F10). ALL mic WS sends happen
 * here, off the 32 ms capture cadence; the worst-case block per frame is the
 * 500 ms mic send timeout, which only delays delivery, never capture. */
static void gl_audio_tx_sender_task(void *arg)
{
    (void)arg;
    static uint8_t frame[GL_TX_PCM_BYTES];

    ESP_LOGI(TAG, "TX sender: started");
    while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP)) {
        if (!s_gl.tx_frame_queue ||
            xQueueReceive(s_gl.tx_frame_queue, frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP) {
            break;
        }
        /* Manual mode: frames queued before a turn ended are stale once the
         * activity closed — audio outside an activity window confuses the
         * manual-VAD protocol. Drop silently (they are trailing silence).
         * Auto VAD (P3.4): there is no activity window — the stream is
         * continuous by design, including during SPEAKING (that is how the
         * server detects barge-in); only a dead session stales a frame. */
        if (GL_USE_SERVER_VAD) {
            if (!s_gl.session_active || !s_gl.ws_connected) {
                continue;
            }
        } else if (!s_gl.activity_open || s_gl.state != GL_STATE_LISTENING) {
            continue;
        }
        if (gl_send_audio_frame(frame, GL_TX_PCM_BYTES)) {
            s_gl.tx_frames_sent++;
        } else {
            s_gl.tx_send_failures++;
        }
    }

    ESP_LOGI(TAG, "TX sender: stopped");
    xEventGroupSetBits(s_gl.ev, GL_BIT_TXS_DONE);
    if (s_gl.tx_sender_task == xTaskGetCurrentTaskHandle()) {
        s_gl.tx_sender_task = NULL;
    }
    claw_task_delete(NULL);
}

/* Park the TX task and wait for it to exit. SESSION TASK ONLY.
 *
 * The old timeout path force-deleted the task with plain vTaskDelete — on a
 * WithCaps PSRAM stack that leaks the 8 KB stack + TCB per event, could kill
 * a task holding ws_mutex mid-send (permanent voice wedge), and could even
 * resolve to vTaskDelete(NULL), deleting the *caller* (F1). The force-delete
 * is gone entirely: the TX loop re-checks GL_BIT_TX_STOP between mic read
 * and WS send, and mic sends time out after GL_WS_MIC_TIMEOUT_MS, so its
 * worst-case park latency is bounded far below GL_TX_STOP_WAIT_MS. On the
 * (should-never-fire) timeout we leave the task alive: it clears
 * s_gl.tx_task itself on exit, and gl_start_tx_task refuses to start a
 * duplicate while the handle is set, so the invariant of at most one TX task
 * holds without killing anything.
 *
 * GL_BIT_TX_DONE is NOT cleared here: gl_start_tx_task clears it before
 * creating each task generation, so the bit being set always means "the
 * current task generation finished" — no clear-then-wait race with a task
 * that already exited. Returns true when the task is known to be gone. */
static bool gl_stop_tx_task(void)
{
    if (!s_gl.tx_task && !s_gl.tx_sender_task) {
        return true;
    }
    xEventGroupSetBits(s_gl.ev, GL_BIT_TX_STOP);
    const EventBits_t both = GL_BIT_TX_DONE | GL_BIT_TXS_DONE;
    EventBits_t bits = xEventGroupWaitBits(s_gl.ev, both,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(GL_TX_STOP_WAIT_MS));
    if ((bits & both) != both) {
        if (!s_gl.tx_task && !s_gl.tx_sender_task) {
            return true;    /* exited between the head check and the wait */
        }
        ESP_LOGE(TAG, "Audio TX: stop timed out (capture=%d sender=%d); leaving parked (no force-delete)",
                 (int)(s_gl.tx_task != NULL), (int)(s_gl.tx_sender_task != NULL));
        return false;
    }
    s_gl.tx_task = NULL;
    s_gl.tx_sender_task = NULL;
    return true;
}

/* Starts the capture + sender pair. Since P2.4 this runs once per session
 * (turn transitions only pause via state), so an "already running" call is
 * the per-turn norm, not an anomaly. */
static void gl_start_tx_task(void)
{
    if (s_gl.tx_task || s_gl.tx_sender_task) {
        ESP_LOGD(TAG, "Audio TX: start ignored; task(s) already running");
        return;
    }
    xEventGroupClearBits(s_gl.ev, GL_BIT_TX_STOP | GL_BIT_TX_DONE | GL_BIT_TXS_DONE);
    static const claw_task_config_t tx_cfg = {
        .name         = "gl_audio_tx",
        .stack_size   = 12288,        /* +4 KB for the synchronous aec_process (D4) */
        .priority     = 6,
        .core_id      = 1,            /* away from the Wi-Fi/lwIP core (D4) */
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    static const claw_task_config_t txs_cfg = {
        .name         = "gl_tx_sender",
        .stack_size   = 8192,
        .priority     = 5,
        .core_id      = 0,            /* pin to the Wi-Fi/lwIP core (2026-06-12):
                                       * tskNO_AFFINITY let it land on core 1 where
                                       * the prio-6 capture+AEC task preempted it,
                                       * stalling the socket drain -> transport_poll_write(0)
                                       * every session. */
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    if (claw_task_create(&tx_cfg, gl_audio_tx_task, NULL, &s_gl.tx_task) != pdPASS) {
        s_gl.tx_task = NULL;
        /* Both DONE bits so a later stop wait can never hang on a task that
         * was never created this generation. */
        xEventGroupSetBits(s_gl.ev, GL_BIT_TX_DONE | GL_BIT_TXS_DONE);
        ESP_LOGE(TAG, "Audio TX: failed to create capture task");
        return;
    }
    if (claw_task_create(&txs_cfg, gl_audio_tx_sender_task, NULL, &s_gl.tx_sender_task) != pdPASS) {
        s_gl.tx_sender_task = NULL;
        xEventGroupSetBits(s_gl.ev, GL_BIT_TXS_DONE);
        ESP_LOGE(TAG, "Audio TX: failed to create sender task (frames will drop)");
    }
}

static void gl_ensure_listening_capture(void)
{
    static int64_t last_recover_us;
    if (s_gl.state != GL_STATE_LISTENING && s_gl.state != GL_STATE_READY) {
        return;
    }
    if (s_gl.tx_task) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if ((uint64_t)(now_us - last_recover_us) < GL_LISTEN_RECOVERY_MS * 1000LL) {
        return;
    }

    int r = gl_open_adc(GL_CAP_SAMPLE_RATE);
    if (r != ESP_CODEC_DEV_OK) {
        gl_set_audio_error("capture retry: ADC open failed");
        ESP_LOGW(TAG, "Listening recovery: ADC open failed (err=%d)", r);
        last_recover_us = now_us;
        return;
    }
    ESP_LOGW(TAG, "Listening recovery: restarting capture task");
    last_recover_us = now_us;
    gl_begin_audio_activity("capture recovery");
    gl_start_tx_task();
}

/* ---- Session command queue (request side + consumer) ---------------------- */

static esp_err_t gl_post_cmd(gl_cmd_type_t type, char *text)
{
    if (!s_gl.cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    gl_cmd_t cmd = {
        .type = type,
        .text = text,
    };
    if (xQueueSend(s_gl.cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, dropping request %d", (int)type);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Commit the user's audio turn. SESSION TASK ONLY. State is re-checked here
 * because the request was queued from another context and the session may
 * have moved on (duplicate commits become no-ops).
 *
 * P2.4 (F11): capture is PAUSED, not torn down — the capture task self-parks
 * the moment state leaves LISTENING and the codec stays open on the session
 * clock. Trailing queued frames are dropped so nothing rides after
 * activityEnd. */
static void gl_do_end_input(void)
{
    if (!s_gl.session_active ||
        (s_gl.state != GL_STATE_READY && s_gl.state != GL_STATE_LISTENING)) {
        return;
    }
    if (s_gl.state == GL_STATE_LISTENING) {
        gl_set_state(GL_STATE_THINKING, "Thinking");
        if (s_gl.tx_frame_queue) {
            xQueueReset(s_gl.tx_frame_queue);
        }
        atomic_store(&s_mic_rms, 0);
    }
    s_gl.last_input_end_us = esp_timer_get_time();
    s_gl.waiting_terminal = true;
    gl_end_audio_activity("end_input");
}

/* Send a text turn. SESSION TASK ONLY (same pause rules as above). */
static void gl_do_send_text(const char *text)
{
    if (!text || !s_gl.session_active ||
        (s_gl.state != GL_STATE_READY && s_gl.state != GL_STATE_LISTENING &&
         s_gl.state != GL_STATE_SPEAKING)) {
        return;
    }
    if (s_gl.state == GL_STATE_LISTENING) {
        gl_set_state(GL_STATE_THINKING, "Thinking");
        if (s_gl.tx_frame_queue) {
            xQueueReset(s_gl.tx_frame_queue);
        }
        gl_end_audio_activity("text turn");
        atomic_store(&s_mic_rms, 0);
    }
    if (!gl_send_text_turn(text)) {
        ESP_LOGW(TAG, "send_text: WS send failed");
    }
}

/* Drain and execute pending requests. SESSION TASK ONLY. Called from every
 * wait state of the session loop so tap / HTTP / VAD requests act promptly
 * even while an audio burst is being drained. */
static void gl_process_cmd_queue(void)
{
    gl_cmd_t cmd;
    if (!s_gl.cmd_queue) {
        return;
    }
    while (xQueueReceive(s_gl.cmd_queue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
        case GL_CMD_END_INPUT:
            gl_do_end_input();
            break;
        case GL_CMD_SEND_TEXT:
            gl_do_send_text(cmd.text);
            break;
        case GL_CMD_INTERRUPT:
            /* Tap (P3.1) or local barge detector (P3.4) during SPEAKING —
             * a pending barge onset stamp tells the two apart. State
             * re-checked: if the turn already ended there is nothing left to
             * interrupt; drop the stamp so a later tap cannot log a bogus
             * barge_latency. */
            if (s_gl.state == GL_STATE_SPEAKING || gl_playback_pending()) {
                gl_interrupt_playback(s_gl.barge_onset_us ? "barge-in"
                                                          : "tap interrupt");
            } else {
                s_gl.barge_onset_us = 0;
            }
            break;
        default:
            ESP_LOGW(TAG, "unknown cmd %d", (int)cmd.type);
            break;
        }
        if (cmd.text) {
            free(cmd.text);
        }
    }
}

/* Discard pending requests without executing them (session start/teardown). */
static void gl_drain_cmd_queue(void)
{
    gl_cmd_t cmd;
    if (!s_gl.cmd_queue) {
        return;
    }
    while (xQueueReceive(s_gl.cmd_queue, &cmd, 0) == pdTRUE) {
        if (cmd.text) {
            free(cmd.text);
        }
    }
}

static cJSON *gl_get_object_compat(cJSON *obj, const char *camel, const char *snake)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, camel);
    return item ? item : cJSON_GetObjectItemCaseSensitive(obj, snake);
}

static bool gl_play_model_audio_from_json(cJSON *audio_obj, uint32_t default_rate)
{
    const char *b64 = NULL;
    uint32_t sample_rate = default_rate;
    bool extracted = gl_extract_audio_data_chunk(audio_obj, &b64, &sample_rate);
    ESP_LOGD(TAG, "play_audio: obj=%s extracted=%d b64=%s rate=%u state=%s",
             audio_obj ? "present" : "null", (int)extracted,
             (b64 && extracted) ? "ok" : "null", (unsigned)sample_rate,
             gl_state_name(s_gl.state));
    if (!extracted) {
        return false;
    }

    if (!b64) {
        return false;
    }

    if (s_gl.state == GL_STATE_LISTENING || s_gl.state == GL_STATE_READY ||
        s_gl.state == GL_STATE_THINKING) {
        if (!gl_enter_speaking(sample_rate)) {
            gl_resume_listening("output-open-failed");
            return false;
        }
    }
    s_gl.audio_part_hits++;
    s_gl.last_audio_us = esp_timer_get_time();
    s_gl.last_audio_mime_rate = sample_rate;
    s_gl.waiting_terminal = true;
    gl_play_audio_b64(b64, sample_rate);
    return true;
}

static bool gl_enter_speaking(uint32_t sample_rate)
{
    if (s_gl.state == GL_STATE_SPEAKING) {
        return true;
    }

    /* Teardown race guard: if a stop was already requested, do not start a new
     * playback path. Otherwise an audio chunk landing during shutdown opens the
     * DAC just to have it slammed shut by the teardown — produces a flash of
     * green on screen + no audible output. Drop the chunk; the session is
     * ending anyway. */
    if (s_gl.stop_requested || !s_gl.session_active) {
        ESP_LOGW(TAG, "enter_speaking: skipped (stop_requested=%d active=%d)",
                 (int)s_gl.stop_requested, (int)s_gl.session_active);
        return false;
    }

    /* P2.4 (F11): capture is NOT torn down — the capture task self-pauses the
     * instant state leaves LISTENING, and the codec stays open on the shared
     * session clock, so the first sample needs no reopen. Drop any
     * queued-but-unsent mic frames so they cannot trail into the model's
     * turn. */
    if (s_gl.tx_frame_queue) {
        xQueueReset(s_gl.tx_frame_queue);
    }
    s_gl.waiting_terminal = true;
    s_gl.speak_enter_us = esp_timer_get_time();
    s_gl.first_audio_pending = true;
    atomic_store(&s_playback_kill, false);   /* new reply: release any prior barge fast-kill */
    gl_set_state(GL_STATE_SPEAKING, "Speaking");

    uint32_t playback_rate = gl_resolve_playback_rate(sample_rate);
    int r = gl_open_dac(playback_rate);   /* no-op: opened for the session */
    if (r != ESP_CODEC_DEV_OK) {
        gl_set_audio_error("speaking: failed to open DAC");
        ESP_LOGE(TAG, "Speaking: failed to open DAC @%u Hz (err=%d)",
                 (unsigned)playback_rate, r);
        s_gl.first_audio_pending = false;
        return false;
    }
    gl_dac_mute(false);
    ESP_LOGI(TAG, "Speaking: DAC live (model_rate=%u playback_rate=%u)",
             (unsigned)sample_rate, (unsigned)playback_rate);
    return true;
}

static void gl_resume_listening(const char *reason)
{
    s_gl.pending_resume = false;
    if (s_gl.state == GL_STATE_LISTENING && s_gl.tx_task) {
        gl_mark_resume_reason(reason ? reason : "already listening");
        s_gl.resume_count++;
        return;
    }

    gl_mark_resume_reason(reason ? reason : "turn complete");
    s_gl.resume_count++;

    /* P2.4 (F11): mute instead of close — the codec keeps running on the
     * session clock. This is what erases the per-turn i2s_channel_disable
     * error pairs and the reopen latency. */
    gl_dac_mute(true);
    s_gl.first_audio_pending = false;
    gl_set_state(GL_STATE_LISTENING, "Listening");

    if (!s_gl.adc_open) {
        int adc_r = gl_open_adc(GL_CAP_SAMPLE_RATE);
        if (adc_r != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Listening: ADC reopen failed after %s (adc=%d)",
                     reason ? reason : "turn", adc_r);
            return;
        }
    }

    gl_begin_audio_activity(reason ? reason : "resume");
    gl_start_tx_task();
    ESP_LOGI(TAG, "Listening: resumed capture (%s)", reason ? reason : "turn complete");
}

/* Cut playback NOW and go back to listening. Tap-during-SPEAKING (P3.1) and
 * the server `interrupted` frame (P3.2) share this path. SESSION TASK ONLY.
 * Flushes the PCM ring (epoch bump aborts an in-flight producer), drops the
 * buffered (undecoded) reply frames, mutes the DAC, and resumes listening —
 * which sends activityStart via gl_begin_audio_activity. */
static void gl_interrupt_playback(const char *reason)
{
    bool playing = (s_gl.state == GL_STATE_SPEAKING) || gl_playback_pending();
    s_gl.pending_resume = false;
    if (playing) {
        ESP_LOGI(TAG, "Interrupt (%s): flushing ring=%u B, rx_queue=%u frames",
                 reason ? reason : "?", (unsigned)s_gl.pcm_ring_bytes,
                 (unsigned)(s_gl.rx_queue ? uxQueueMessagesWaiting(s_gl.rx_queue) : 0));
        gl_pcm_ring_flush();
        gl_drain_rx_queue();
        gl_dac_mute(true);
    }
    /* The fast-kill flag only needs to hold the feeder off across the
     * latch→flush window (the variable session-task-busy gap). The ring is now
     * flushed and the DAC muted, so release it — playback stays silent (muted)
     * through LISTENING; gl_enter_speaking un-mutes for the next reply. Clearing
     * here (not only in gl_enter_speaking) stops the flag sticking true across a
     * barge that has no following speaking turn (teardown / WS-resume). */
    atomic_store(&s_playback_kill, false);
    /* barge_latency (P3.4 gate): the capture task stamped speech onset when
     * the barge latch armed; playback is audibly dead as of the mute above
     * (any in-flight ≤80 ms feeder chunk drains into a muted DAC). Perceived
     * stop = onset → here. Target < 250 ms. Tap/server interrupts carry no
     * onset stamp and skip the log. */
    int64_t barge_onset = s_gl.barge_onset_us;
    if (barge_onset) {
        s_gl.barge_onset_us = 0;
        ESP_LOGI(TAG, "barge_latency: %u ms (speech onset -> playback flushed, reason=%s)",
                 (unsigned)((esp_timer_get_time() - barge_onset) / 1000),
                 reason ? reason : "?");
    }
    s_gl.waiting_terminal = false;
    gl_resume_listening(reason ? reason : "interrupt");
}

/* ---- WS event handler ---------------------------------------------------- */

static void gl_ws_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WS connected");
        s_gl.ws_connected = true;
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "WS disconnected");
        s_gl.ws_connected = false;
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        /* Accept TEXT and BINARY — Gemini Live may use either */
        if (ev->op_code != WS_TRANSPORT_OPCODES_TEXT &&
            ev->op_code != WS_TRANSPORT_OPCODES_BINARY) {
            return;
        }
        if (!s_gl.rx_buf) {
            return;
        }
        /* Reassemble fragmented frames — drop and warn if frame > buffer */
        if ((size_t)ev->payload_len >= GL_RX_BUF_SIZE) {
            if (ev->payload_offset == 0) {
                ESP_LOGW(TAG, "Frame too large (%d bytes), dropping", ev->payload_len);
            }
            return;
        }
        if (ev->payload_offset + ev->data_len <= GL_RX_BUF_SIZE - 1) {
            memcpy(s_gl.rx_buf + ev->payload_offset, ev->data_ptr, ev->data_len);
        }
        if (ev->payload_offset + ev->data_len >= (size_t)ev->payload_len) {
            s_gl.rx_buf[ev->payload_len] = '\0';
            if (strstr(s_gl.rx_buf, "\"setupComplete\"") ||
                strstr(s_gl.rx_buf, "\"error\"") ||
                strstr(s_gl.rx_buf, "\"goAway\"")) {
                ESP_LOGI(TAG, "WS RX (%d B): %.200s", ev->payload_len, s_gl.rx_buf);
            } else {
                ESP_LOGD(TAG, "WS RX (%d B): %.200s", ev->payload_len, s_gl.rx_buf);
            }
            /* Copy the completed frame to a right-sized PSRAM block and queue it,
             * so a slow (blocking-playback) consumer cannot lose frames to rx_buf
             * being overwritten by the next arrival. At the byte cap, WAIT for
             * the consumer instead of dropping (see GL_RX_BACKPRESSURE_MAX_MS):
             * the parked WS task is TCP backpressure on the server, so a long
             * reply throttles to realtime instead of losing audio. Drop only on
             * a wedged consumer (deadline) or during stop/teardown. */
            if (s_gl.rx_queue) {
                size_t flen = (size_t)ev->payload_len + 1;
                /* Byte-cap the raw queue (P2.2/F9): bound the PSRAM the queue
                 * can pin instead of letting a burst eat the budget. */
                uint32_t waited_ms = 0;
                /* Wait while EITHER limit binds: the byte cap, or queue depth
                 * (small frames can fill all GL_RX_QUEUE_DEPTH slots below the
                 * byte cap — without this the depth path would still drop). */
                while ((atomic_load(&s_rx_queue_bytes) + flen > GL_RX_QUEUE_BYTE_CAP ||
                        uxQueueSpacesAvailable(s_gl.rx_queue) == 0) &&
                       s_gl.session_active && !s_gl.stop_requested &&
                       waited_ms < GL_RX_BACKPRESSURE_MAX_MS) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    waited_ms += 10;
                }
                if (atomic_load(&s_rx_queue_bytes) + flen > GL_RX_QUEUE_BYTE_CAP) {
                    /* Stopping: the frame is moot (teardown drains the queue) —
                     * discard silently. Otherwise the consumer made zero
                     * progress for the whole deadline: count the drop. */
                    if (s_gl.session_active && !s_gl.stop_requested &&
                        (s_gl.rx_drops++ % 8) == 0) {
                        ESP_LOGW(TAG, "rx queue byte cap held %u ms, dropped frame (total %u)",
                                 (unsigned)waited_ms, (unsigned)s_gl.rx_drops);
                    }
                } else {
                    char *frame = heap_caps_malloc(flen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (frame) {
                        memcpy(frame, s_gl.rx_buf, flen);
                        if (xQueueSend(s_gl.rx_queue, &frame, 0) != pdTRUE) {
                            heap_caps_free(frame);
                            if ((s_gl.rx_drops++ % 8) == 0) {
                                ESP_LOGW(TAG, "rx queue full, dropped frame (total %u)",
                                         (unsigned)s_gl.rx_drops);
                            }
                        } else {
                            atomic_fetch_add(&s_rx_queue_bytes, (uint32_t)flen);
                        }
                    } else {
                        /* PSRAM allocation failure loses the frame too — count
                         * it so the drops diag stays an honest audio-loss
                         * proxy (was a silent loss before). */
                        if ((s_gl.rx_drops++ % 8) == 0) {
                            ESP_LOGW(TAG, "rx frame alloc failed (%u B), dropped frame (total %u)",
                                     (unsigned)flen, (unsigned)s_gl.rx_drops);
                        }
                    }
                }
            }
        }
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "WS error");
    } else if (event_id == WEBSOCKET_EVENT_CLOSED) {
        ESP_LOGW(TAG, "WS closed by server");
        s_gl.ws_connected = false;
    }
}

/* ---- JarvisMCP tool bridge ------------------------------------------------ */

/* Read a string arg from a functionCall's args object ("" if absent). */
static const char *gl_arg(cJSON *args, const char *key)
{
    const char *v = args ? cJSON_GetStringValue(cJSON_GetObjectItem(args, key)) : NULL;
    return v ? v : "";
}

/* Escape src for embedding inside a JS single-quoted string literal (no
 * surrounding quotes written). Backslash and apostrophe are escaped; control
 * chars dropped; output truncated to fit dst. Defends the generated /act JS
 * against model-supplied free text containing quotes. */
static void gl_js_str_escape(char *dst, size_t dst_sz, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20) {
            continue;
        }
        if (c == '\\' || c == '\'') {
            dst[j++] = '\\';
        }
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

/* POST {"code":<js>} to the JarvisMCP /act gateway with the bearer token; copy
 * the response body into `out`. Returns the HTTP status (200 ok) or -1 on a
 * transport error. Blocking up to 30 s — TOOL WORKER TASK ONLY (P2.1/F8),
 * never the session task. */
static int gl_act_call(const char *code, char *out, size_t out_sz)
{
    if (!s_gl.mcp_key[0] || !s_gl.mcp_url[0]) {
        ESP_LOGW(TAG, "JarvisMCP not configured (set jarvis_mcp_url + jarvis_mcp_key via /api/config)");
        return -1;
    }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "code", code);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body) {
        return -1;
    }

    esp_http_client_config_t cfg = {
        .url               = s_gl.mcp_url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(body);
        return -1;
    }
    char auth[sizeof(s_gl.mcp_key) + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", s_gl.mcp_key);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Authorization", auth);

    int    status  = -1;
    size_t blen    = strlen(body);
    int64_t t0     = esp_timer_get_time();
    esp_err_t err  = esp_http_client_open(cli, blen);
    if (err == ESP_OK) {
        if (esp_http_client_write(cli, body, blen) >= 0) {
            esp_http_client_fetch_headers(cli);
            status = esp_http_client_get_status_code(cli);
            int rd = esp_http_client_read_response(cli, out, (int)out_sz - 1);
            out[(rd > 0) ? rd : 0] = '\0';
        }
    } else {
        ESP_LOGW(TAG, "act open failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "act call: status=%d %lldms code=%.80s", status,
             (esp_timer_get_time() - t0) / 1000, code);
    esp_http_client_cleanup(cli);
    free(body);
    return status;
}

/* ask_user — show a tappable choice arc on the round display and wait (bounded)
 * for the user to tap an answer. TOOL WORKER TASK ONLY (runs off the session
 * task, so the blocking poll never gaps audio — same contract as gl_act_call).
 *
 * Writes the chosen label into `response` (key "answer") on success, or an error
 * on bad args / timeout. Returns true if a selection completed. The poll budget
 * (GL_ASK_USER_TIMEOUT_MS) is held by the tool-inflight watchdog hold in
 * gl_maybe_resume_speaking_watchdog, so the 20 s no-reply watchdog will not fire
 * while we wait. ui_layer copies the strings internally. */
#define GL_ASK_USER_TIMEOUT_MS  30000
#define GL_ASK_USER_POLL_MS     100
static bool gl_ask_user(cJSON *args, cJSON *response)
{
    cJSON *qj = args ? cJSON_GetObjectItem(args, "question") : NULL;
    cJSON *oj = args ? cJSON_GetObjectItem(args, "options") : NULL;
    const char *question = (qj && cJSON_IsString(qj)) ? qj->valuestring : "";

    if (!oj || !cJSON_IsArray(oj)) {
        cJSON_AddStringToObject(response, "error", "options must be an array of 2..6 strings");
        return false;
    }
    int total = cJSON_GetArraySize(oj);
    if (total < 2) {
        cJSON_AddStringToObject(response, "error", "need at least 2 options");
        return false;
    }
    int n = total > 6 ? 6 : total; /* UI_LAYER_MAX_OPTIONS */

    /* Stable storage for the option labels passed to ui_layer (it copies, but the
     * const char* array must stay valid for the duration of the call). */
    char store[6][48] = {{0}};
    const char *opts[6] = {0};
    for (int i = 0; i < n; ++i) {
        cJSON *it = cJSON_GetArrayItem(oj, i);
        const char *s = (it && cJSON_IsString(it)) ? it->valuestring : "";
        strlcpy(store[i], s, sizeof(store[i]));
        opts[i] = store[i];
    }

    esp_err_t err = ui_layer_show_choice(question, opts, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ask_user: show_choice failed: %s", esp_err_to_name(err));
        cJSON_AddStringToObject(response, "error", "could not show choices on screen");
        return false;
    }
    ESP_LOGI(TAG, "ask_user: \"%s\" (%d options), waiting for tap", question, n);

    int waited = 0;
    int index = -1;
    bool done = false;
    while (waited < GL_ASK_USER_TIMEOUT_MS) {
        /* Bail out if the session ended under us — don't keep a dead UI up. */
        if (!s_gl.session_active) {
            ESP_LOGW(TAG, "ask_user: session ended while waiting; dismissing");
            ui_layer_dismiss();
            cJSON_AddStringToObject(response, "error", "session ended before answer");
            return false;
        }
        ui_layer_get_result(&index, &done);
        if (done) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(GL_ASK_USER_POLL_MS));
        waited += GL_ASK_USER_POLL_MS;
    }

    if (!done) {
        ESP_LOGW(TAG, "ask_user: timed out after %d ms; dismissing", waited);
        ui_layer_dismiss(); /* on_tap self-dismisses; this covers the timeout path */
        cJSON_AddStringToObject(response, "error", "user did not tap an answer in time");
        return false;
    }

    if (index < 0 || index >= n) {
        cJSON_AddStringToObject(response, "error", "invalid selection");
        return false;
    }
    ESP_LOGI(TAG, "ask_user: user tapped option %d (\"%s\")", index, store[index]);
    cJSON_AddStringToObject(response, "answer", store[index]);
    cJSON_AddNumberToObject(response, "index", index);
    return true;
}

/* Execute a toolCall's functions via JarvisMCP and send the toolResponse.
 * TOOL WORKER TASK ONLY — gl_act_call blocks up to 30 s per function, which
 * must never gap audio (P2.1/F8). The WS send is race-safe: gl_ws_send_text
 * snapshots the client handle under ws_mutex (P1.3). */
static void gl_run_tool_call(cJSON *toolCall)
{
    cJSON *fcs = cJSON_GetObjectItemCaseSensitive(toolCall, "functionCalls");
    cJSON *resp_root = cJSON_CreateObject();
    cJSON *tr  = cJSON_AddObjectToObject(resp_root, "toolResponse");
    cJSON *frs = cJSON_AddArrayToObject(tr, "functionResponses");

    cJSON *fc;
    cJSON_ArrayForEach(fc, fcs) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fc, "name"));
        cJSON *idj  = cJSON_GetObjectItem(fc, "id");
        cJSON *args = cJSON_GetObjectItem(fc, "args");

        cJSON *fr = cJSON_CreateObject();
        if (idj && cJSON_IsString(idj)) {
            cJSON_AddStringToObject(fr, "id", idj->valuestring);
        }
        cJSON_AddStringToObject(fr, "name", name ? name : "");
        cJSON *response = cJSON_AddObjectToObject(fr, "response");

        /* ask_user is handled locally (show choice arc + wait for tap) — it does
         * NOT route through the /act JS gateway like the other tools, so it short
         * -circuits the code-building + gl_act_call path below via `handled`. */
        if (name && strcmp(name, "ask_user") == 0) {
            gl_ask_user(args, response);
            cJSON_AddItemToArray(frs, fr);
            continue;
        }

        /* Build the JS for this function. Args are sanitised into the template. */
        char code[512] = {0};
        if (name && strcmp(name, "crypto_price") == 0) {
            const char *symbol = args ? cJSON_GetStringValue(cJSON_GetObjectItem(args, "symbol")) : NULL;
            char sym[40];
            int j = 0;
            for (int i = 0; symbol && symbol[i] && j < (int)sizeof(sym) - 1; i++) {
                char c = symbol[i];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == ' ') {
                    sym[j++] = c;
                }
            }
            sym[j] = '\0';
            snprintf(code, sizeof(code), "return await jarvis.crypto('%s')", sym[0] ? sym : "bitcoin");
        } else if (name && strcmp(name, "recall_memory") == 0) {
            char q[256];
            gl_js_str_escape(q, sizeof(q), gl_arg(args, "query"));
            snprintf(code, sizeof(code),
                     "return await jarvis.memory.search({query:'%s', area:'all'})", q);
        } else if (name && strcmp(name, "remember") == 0) {
            char q[300];
            gl_js_str_escape(q, sizeof(q), gl_arg(args, "note"));
            snprintf(code, sizeof(code), "return await jarvis.memory.capture('%s')", q);
        } else if (name && strcmp(name, "wikipedia") == 0) {
            char q[200];
            gl_js_str_escape(q, sizeof(q), gl_arg(args, "topic"));
            snprintf(code, sizeof(code), "return await jarvis.wiki('%s')", q);
        } else if (name && strcmp(name, "country_info") == 0) {
            char q[120];
            gl_js_str_escape(q, sizeof(q), gl_arg(args, "country"));
            snprintf(code, sizeof(code), "return await jarvis.country('%s')", q);
        } else if (name && strcmp(name, "current_time") == 0) {
            char tz[80];
            gl_js_str_escape(tz, sizeof(tz), gl_arg(args, "timezone"));
            if (tz[0]) {
                snprintf(code, sizeof(code), "return await jarvis.time('%s')", tz);
            } else {
                snprintf(code, sizeof(code), "return await jarvis.time()");
            }
        } else if (name && strcmp(name, "ask_jarvis") == 0) {
            /* Escape hatch: the model supplies the JS itself; the /act gateway
             * sandboxes execution. Pass through verbatim (truncated to buffer). */
            snprintf(code, sizeof(code), "%s", gl_arg(args, "code"));
        }

        if (code[0]) {
            char *rbuf = malloc(GL_ACT_RESP_MAX);
            if (rbuf) {
                int st = gl_act_call(code, rbuf, GL_ACT_RESP_MAX);
                cJSON *actj = (st == 200) ? cJSON_Parse(rbuf) : NULL;
                cJSON *result = actj ? cJSON_GetObjectItem(actj, "result") : NULL;
                if (result) {
                    cJSON_AddItemToObject(response, "result", cJSON_Duplicate(result, 1));
                } else {
                    cJSON_AddStringToObject(response, "error",
                                            st == 200 ? "no result" : "jarvis call failed");
                }
                if (actj) {
                    cJSON_Delete(actj);
                }
                free(rbuf);
            } else {
                cJSON_AddStringToObject(response, "error", "out of memory");
            }
        } else {
            cJSON_AddStringToObject(response, "error", "unknown function");
        }
        cJSON_AddItemToArray(frs, fr);
    }

    char *out = cJSON_PrintUnformatted(resp_root);
    cJSON_Delete(resp_root);
    if (out) {
        gl_ws_send_text(out);
        free(out);
    }
}

/* Persistent tool worker (P2.1/F8). Created once at gateway start, never
 * deleted — a parked task costs one PSRAM stack and removes every teardown
 * race. Stale jobs (from a session that ended while the HTTPS call ran) are
 * dropped by the generation check. */
static void gl_tool_worker_task(void *arg)
{
    (void)arg;
    gl_tool_job_t job;
    for (;;) {
        if (xQueueReceive(s_gl.tool_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.gen == s_gl.session_gen && s_gl.session_active) {
            gl_run_tool_call(job.tool_call);
        } else {
            ESP_LOGW(TAG, "tool job dropped (stale session gen %u != %u)",
                     (unsigned)job.gen, (unsigned)s_gl.session_gen);
        }
        cJSON_Delete(job.tool_call);
        atomic_fetch_sub(&s_tool_inflight, 1);
    }
}

/* SESSION TASK ONLY. Takes ownership of tool_call (deleted on failure too). */
static void gl_queue_tool_call(cJSON *tool_call)
{
    if (!s_gl.tool_queue || !s_gl.tool_task) {
        ESP_LOGE(TAG, "toolCall dropped: no tool worker");
        cJSON_Delete(tool_call);
        return;
    }
    gl_tool_job_t job = {
        .gen       = s_gl.session_gen,
        .tool_call = tool_call,
    };
    atomic_fetch_add(&s_tool_inflight, 1);
    if (xQueueSend(s_gl.tool_queue, &job, 0) != pdTRUE) {
        atomic_fetch_sub(&s_tool_inflight, 1);
        ESP_LOGW(TAG, "tool queue full, dropping toolCall");
        cJSON_Delete(tool_call);
    }
}

static void gl_maybe_resume_speaking_watchdog(void)
{
    /* Playback in progress = not stalled. The burst is decoded long before it
     * finishes playing (P2.1), so last_audio_us (a decode-time stamp) goes
     * stale while the ring drains. Refresh it while audio demonstrably moves. */
    if (s_gl.state == GL_STATE_SPEAKING && gl_playback_pending()) {
        s_gl.last_audio_us = esp_timer_get_time();
    }

    /* A JarvisMCP tool call may legitimately take ~30 s and the model cannot
     * answer before the toolResponse. Hold the watchdog while a job is in
     * flight and restart the no-reply window when the last one completes
     * (64-bit stamps stay session-task-only — the worker never writes them). */
    static uint32_t s_prev_tool_inflight;
    uint32_t tool_inflight = atomic_load(&s_tool_inflight);
    if (tool_inflight > 0) {
        s_prev_tool_inflight = tool_inflight;
        return;
    }
    if (s_prev_tool_inflight > 0) {
        s_prev_tool_inflight = 0;
        int64_t now = esp_timer_get_time();
        if (s_gl.last_input_end_us) {
            s_gl.last_input_end_us = now;
        }
        if (s_gl.last_audio_us) {
            s_gl.last_audio_us = now;
        }
    }

    if (s_gl.state == GL_STATE_THINKING && !s_gl.last_audio_us && s_gl.last_input_end_us) {
        int64_t now = esp_timer_get_time();
        if ((uint64_t)(now - s_gl.last_input_end_us) >= GL_SPEAK_WATCHDOG_MS * 1000ULL) {
            s_gl.watchdog_resume_count++;
            ESP_LOGW(TAG, "watchdog: no response for %lu ms after activityEnd",
                     (unsigned long)GL_SPEAK_WATCHDOG_MS);
            s_gl.last_input_end_us = 0;
            gl_resume_listening("input watchdog");
        }
        return;
    }

    if (!s_gl.waiting_terminal) {
        return;
    }
    if (!(s_gl.state == GL_STATE_SPEAKING || s_gl.state == GL_STATE_THINKING)) {
        s_gl.waiting_terminal = false;
        return;
    }
    if (!s_gl.last_audio_us) {
        return;
    }
    if (s_gl.rx_queue && uxQueueMessagesWaiting(s_gl.rx_queue) > 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if ((uint64_t)(now - s_gl.last_audio_us) < GL_SPEAK_WATCHDOG_MS * 1000ULL) {
        return;
    }

    s_gl.watchdog_resume_count++;
    ESP_LOGW(TAG, "watchdog: no terminal signal for %lu ms while %s", (unsigned long)GL_SPEAK_WATCHDOG_MS,
             gl_state_name(s_gl.state));
    gl_resume_listening("speaking watchdog");
}

/* ---- Frame dispatch ------------------------------------------------------- */

static void gl_dispatch_frame(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Parse failed: %.80s", json);
        return;
    }

    s_gl.rx_frames++;
    s_gl.last_frame_us = esp_timer_get_time();

    /* If an interrupt flushes playback while THIS frame is being processed
     * (tap mid-burst via the cmd queue, or this very frame's `interrupted`
     * flag), the epoch moves and the remaining audio parts of the frame are
     * skipped instead of restarting the cancelled reply. */
    uint32_t play_epoch = s_gl.pcm_ring_epoch;

    /* setupComplete */
    cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "setupComplete");
    if (sc) {
        ESP_LOGI(TAG, "setupComplete received");
        gl_set_state(GL_STATE_READY, "Setup complete");
        xEventGroupSetBits(s_gl.ev, GL_BIT_SETUP_OK);
        cJSON_Delete(root);
        return;
    }

    /* serverContent */
    cJSON *svc = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (svc) {
        /* interrupted */
        cJSON *intr = cJSON_GetObjectItemCaseSensitive(svc, "interrupted");
        if (cJSON_IsTrue(intr)) {
            /* P3.2: flush queued audio instead of draining the backlog — the
             * server already decided the reply is dead. Counter increments on
             * every receipt. */
            ESP_LOGI(TAG, "Model interrupted");
            s_gl.interrupted_hits++;
            gl_interrupt_playback("interrupted");
        }

        /* inputTranscription.finished=true → VAD just decided the user's turn
         * ended. Audio reply is ~1-3 s away. Flip the face to THINKING so the
         * panel doesn't read as frozen during that gap. Keep gl state as
         * LISTENING so capture continues (VAD may also flip back to user mid-
         * gap if it changes its mind). gl_enter_speaking will overwrite the
         * face when audio arrives. */
        cJSON *itr = cJSON_GetObjectItemCaseSensitive(svc, "inputTranscription");
        if (itr) {
            cJSON *fin = cJSON_GetObjectItemCaseSensitive(itr, "finished");
            bool   flip = cJSON_IsTrue(fin) && s_gl.state == GL_STATE_LISTENING;
            /* AUTO-VAD gate (P3.4 — design open question): the transcription
             * stream is UNORDERED relative to other server messages, and with
             * the continuous uplink a late `finished` for the PREVIOUS turn
             * can land while that turn's reply is still draining (state
             * LISTENING via deferred resume) or right after a barge flush.
             * Require "no reply in flight" so a stale marker cannot flash
             * THINKING over live playback. Field re-validation of the flip
             * itself is owed in the Phase-5 hardware pass. */
            if (GL_USE_SERVER_VAD) {
                flip = flip && !s_gl.waiting_terminal && !gl_playback_pending();
            }
            if (flip) {
                ESP_LOGI(TAG, "inputTranscription finished — face=THINKING (await audio)");
                emote_set_thinking();
            }
        }

        /* modelTurn parts */
        cJSON *turn = cJSON_GetObjectItemCaseSensitive(svc, "modelTurn");
        if (turn) {
            cJSON *parts = cJSON_GetObjectItemCaseSensitive(turn, "parts");
            if (cJSON_IsArray(parts)) {
                cJSON *part;
                cJSON_ArrayForEach(part, parts) {
                    /* Text part — print to console (Phase 2) */
                    cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
                    if (cJSON_IsString(text)) {
                        s_gl.text_part_hits++;
                        printf("[Gemini] %s\n", text->valuestring);
                    }

                    /* Audio part (Phase 3) */
                    cJSON *id = gl_get_object_compat(part, "inlineData", "inline_data");
                    {
                        /* Debug: log what fields this part contains */
                        char part_keys[128] = {0};
                        int pk = 0;
                        cJSON *c = part ? part->child : NULL;
                        while (c && pk < (int)sizeof(part_keys) - 2) {
                            int n = snprintf(part_keys + pk, sizeof(part_keys) - pk - 1, "%s ", c->string ? c->string : "?");
                            pk += (n > 0 ? n : 0);
                            c = c->next;
                        }
                        ESP_LOGD(TAG, "part keys=[%s] id=%s", part_keys, id ? "found" : "null");
                    }
                    if (s_gl.pcm_ring_epoch == play_epoch &&
                        gl_play_model_audio_from_json(id, GL_RX_SAMPLE_RATE)) {
                        /* audio queued (inlineData / inline_data) */
                    }

                    cJSON *chunks = cJSON_GetObjectItemCaseSensitive(part, "mediaChunks");
                    if (cJSON_IsArray(chunks)) {
                        cJSON *chunk_item;
                        cJSON_ArrayForEach(chunk_item, chunks) {
                            if (s_gl.pcm_ring_epoch != play_epoch) {
                                break;   /* interrupted mid-frame */
                            }
                            if (gl_play_model_audio_from_json(chunk_item, GL_RX_SAMPLE_RATE)) {
                                /* audio queued (mediaChunks array item) */
                            }
                        }
                    } else if (s_gl.pcm_ring_epoch == play_epoch &&
                               gl_play_model_audio_from_json(chunks, GL_RX_SAMPLE_RATE)) {
                        /* audio queued (mediaChunks object) */
                    }
                }
            }
        }

        /* turnComplete */
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(svc, "turnComplete");
        const bool tc_true = cJSON_IsTrue(tc);
        if (tc_true) {
            s_gl.turn_complete_hits++;
        }

        /* generationComplete */
        cJSON *gc = cJSON_GetObjectItemCaseSensitive(svc, "generationComplete");
        const bool gc_true = cJSON_IsTrue(gc);
        if (gc_true) {
            s_gl.generation_complete_hits++;
        }
        if (tc_true || gc_true) {
            const char *why = tc_true ? "turn complete" : "generation complete";
            if (s_gl.state == GL_STATE_SPEAKING && gl_playback_pending()) {
                /* The reply is still draining out of the PCM ring (the model
                 * bursts faster than realtime, P2.1). Defer the resume until
                 * the feeder runs dry — cutting over now would clip the
                 * reply. The session loop completes it. */
                strlcpy(s_gl.pending_resume_reason, why,
                        sizeof(s_gl.pending_resume_reason));
                s_gl.pending_resume = true;
                ESP_LOGI(TAG, "%s: deferred behind playback (%u B buffered)",
                         why, (unsigned)s_gl.pcm_ring_bytes);
            } else {
                gl_resume_listening(why);
            }
        }
        cJSON_Delete(root);
        return;
    }

    /* sessionResumptionUpdate — protocol heartbeat for resumable sessions. Harmless,
     * but log occasionally so we can confirm the frame shape without warning spam. */
    cJSON *sru = cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate");
    if (sru) {
        static uint32_t sru_hits;
        cJSON *new_handle = cJSON_GetObjectItemCaseSensitive(sru, "newHandle");
        cJSON *resumable  = cJSON_GetObjectItemCaseSensitive(sru, "resumable");
        sru_hits++;
        if ((sru_hits % 32) == 1) {
            ESP_LOGI(TAG, "sessionResumptionUpdate: handle=%s resumable=%s",
                     cJSON_IsString(new_handle) ? new_handle->valuestring : "n/a",
                     cJSON_IsTrue(resumable) ? "true" : "false");
        }
        cJSON_Delete(root);
        return;
    }

    /* Function calling — hand the toolCall to the worker task (P2.1/F8): the
     * 30 s HTTPS POST must never run on this task, where it would gap audio
     * and stall the rx drain. The worker sends the toolResponse itself. */
    cJSON *tool_call = cJSON_DetachItemFromObjectCaseSensitive(root, "toolCall");
    if (tool_call) {
        s_gl.tool_call_hits++;
        gl_queue_tool_call(tool_call);   /* takes ownership */
        cJSON_Delete(root);
        return;
    }

    /* toolCallCancellation (P3.4): a barge-in cancelled generation while tool
     * calls were pending — the server has already discarded them and names
     * the ids. An in-flight HTTPS call cannot be aborted mid-request; the
     * worker's session-generation check plus the server having moved on make
     * a late toolResponse harmless (ignored upstream). Count + log so the
     * Phase-5 field test can see cancellations actually happen. */
    cJSON *tcc = gl_get_object_compat(root, "toolCallCancellation",
                                      "tool_call_cancellation");
    if (tcc) {
        s_gl.tool_cancel_hits++;
        cJSON *ids = cJSON_GetObjectItemCaseSensitive(tcc, "ids");
        ESP_LOGI(TAG, "toolCallCancellation: %d id(s) cancelled by server (total %u)",
                 cJSON_IsArray(ids) ? cJSON_GetArraySize(ids) : 0,
                 (unsigned)s_gl.tool_cancel_hits);
        cJSON_Delete(root);
        return;
    }

    cJSON *ga = cJSON_GetObjectItemCaseSensitive(root, "goAway");
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (ga || err) {
        cJSON *code = ga ? cJSON_GetObjectItemCaseSensitive(ga, "code")
                         : cJSON_GetObjectItemCaseSensitive(err, "code");
        cJSON *msg = ga ? cJSON_GetObjectItemCaseSensitive(ga, "message")
                        : cJSON_GetObjectItemCaseSensitive(err, "message");
        ESP_LOGW(TAG, "%s: code=%s msg=%s",
                 ga ? "goAway" : "error",
                 cJSON_IsString(code) ? code->valuestring : "n/a",
                 cJSON_IsString(msg) ? msg->valuestring : "n/a");
        gl_resume_listening("server signal");
        cJSON_Delete(root);
        return;
    }

    /* Unhandled top-level frame — surface it instead of silently dropping.
     * Upstream protocol drift happens. Visible logs win over silent ignorance. */
    ESP_LOGW(TAG, "Unhandled server frame: %.200s", json);
    s_gl.unhandled_hits++;
    cJSON_Delete(root);
}

/* Pop one queued server frame (waiting up to `wait`), dispatch it, free it.
 * Returns true if a frame was processed. Used by both the setup-wait and the
 * main receive loops so neither path can lose frames. */
static bool gl_process_rx_queue(TickType_t wait)
{
    char *frame = NULL;
    if (!s_gl.rx_queue || xQueueReceive(s_gl.rx_queue, &frame, wait) != pdTRUE) {
        return false;
    }
    if (!frame) {
        return false;
    }
    atomic_fetch_sub(&s_rx_queue_bytes, (uint32_t)(strlen(frame) + 1));
    UBaseType_t depth = uxQueueMessagesWaiting(s_gl.rx_queue);
    if (depth > 3) {
        ESP_LOGD(TAG, "rx queue depth=%u", (unsigned)depth);
    }
    gl_dispatch_frame(frame);
    heap_caps_free(frame);
    return true;
}

/* Drop and free any frames left in the queue (teardown / interrupt flush). */
static void gl_drain_rx_queue(void)
{
    char *frame = NULL;
    if (!s_gl.rx_queue) {
        return;
    }
    while (xQueueReceive(s_gl.rx_queue, &frame, 0) == pdTRUE) {
        if (frame) {
            atomic_fetch_sub(&s_rx_queue_bytes, (uint32_t)(strlen(frame) + 1));
            heap_caps_free(frame);
        }
    }
}

/* ---- Session task --------------------------------------------------------- */

static void gl_ws_cleanup_task(void *arg)
{
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)arg;
    if (client) {
        ESP_LOGI(TAG, "WS cleanup: stop/destroy begin");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        ESP_LOGI(TAG, "WS cleanup: stop/destroy done");
    }
    /* Signal the session task that the old client is fully gone — it must not
     * create a new client (sharing rx_buf + the event handler) before this
     * (F6 two-clients window). */
    if (s_gl.ev) {
        xEventGroupSetBits(s_gl.ev, GL_BIT_WS_CLEANED);
    }
    vTaskDelete(NULL);
}

/* In-session WS resume after a transport write-0 abort (root-cause #1).
 *
 * A benign TCP would-block makes esp_websocket_client abort the connection and
 * fire WEBSOCKET_EVENT_DISCONNECTED; the handler clears ws_connected. Rather
 * than tear the whole session down to IDLE, restart the SAME client handle and
 * re-do the setup handshake while the audio path (PCM ring, converters, feeder,
 * TX task) stays open. Returns true if the socket is live and setupComplete was
 * re-received; false to fall through to session_cleanup.
 *
 * Safety: we reuse s_gl.ws_client (never destroy/re-init it here), so the
 * registered event handler + shared rx_buf are unchanged — there is no
 * two-clients window (F6) and no need to join the async cleanup task. While the
 * resume runs ws_connected is false, so gl_ws_send_text_to (which snapshots the
 * handle under ws_mutex and gates on ws_connected) emits nothing into the
 * restarting socket. */
static bool gl_try_ws_resume(void)
{
    esp_websocket_client_handle_t client = s_gl.ws_client;
    if (!client || s_gl.stop_requested || !s_gl.session_active) {
        return false;
    }

    /* stop() the aborted client, then start() the same handle. stop() can block
     * up to network_timeout_ms; that is acceptable here — we are already torn
     * out of realtime by the drop. */
    esp_websocket_client_stop(client);
    if (s_gl.stop_requested || !s_gl.session_active) {
        return false;
    }
    s_gl.ws_connected = false;
    xEventGroupClearBits(s_gl.ev, GL_BIT_SETUP_OK);

    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS resume: start failed (%s)", esp_err_to_name(err));
        return false;
    }

    /* Wait for the new TCP+WS handshake (CONNECTED sets ws_connected). */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(GL_WS_RESUME_CONNECT_MS);
    while (!s_gl.ws_connected && !s_gl.stop_requested && s_gl.session_active) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGW(TAG, "WS resume: reconnect timeout");
            return false;
        }
        gl_process_cmd_queue();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_gl.ws_connected || s_gl.stop_requested || !s_gl.session_active) {
        return false;
    }

    /* Re-send setup and re-wait setupComplete (same poll+dispatch as the initial
     * handshake — the main rx loop is not running during resume). */
    if (!gl_send_setup()) {
        ESP_LOGW(TAG, "WS resume: setup send failed");
        return false;
    }
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(GL_WS_RESUME_SETUP_MS);
    while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK) &&
           !s_gl.stop_requested && s_gl.session_active && s_gl.ws_connected) {
        if (xTaskGetTickCount() > deadline) {
            break;
        }
        gl_process_rx_queue(pdMS_TO_TICKS(100));
        gl_process_cmd_queue();
    }
    if (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK)) {
        ESP_LOGW(TAG, "WS resume: setupComplete timeout");
        return false;
    }

    /* The resumed socket is a fresh server session (new setupComplete) with no
     * memory of the pre-drop turn, so the input activity must be re-sent. Force
     * a clean LISTENING turn first: a drop mid-SPEAKING would otherwise leave
     * state == GL_STATE_SPEAKING, and the TX gate (mic frames uplink only when
     * state == GL_STATE_LISTENING) would drop every frame until the ~20 s
     * speaking watchdog rescued it — the "ws_resume_count++ but audio_parts==0
     * next turn" failure mode. The audio path itself (PCM ring, feeder, TX task)
     * was never torn down. */
    s_gl.waiting_terminal = false;
    s_gl.pending_resume   = false;
    atomic_store(&s_playback_kill, false);   /* a drop mid-barge must not carry the fast-kill into the resumed session */
    if (s_gl.state != GL_STATE_LISTENING) {
        gl_dac_mute(true);
        s_gl.first_audio_pending = false;
        gl_set_state(GL_STATE_LISTENING, "Listening");
    }
    if (!s_gl.adc_open) {
        gl_open_adc(GL_CAP_SAMPLE_RATE);
    }
    /* Reset activity_open so gl_begin_audio_activity re-sends activityStart on
     * the new socket (it no-ops when activity_open is already true — needed for
     * the drop-while-LISTENING case where the old activity was open). */
    s_gl.activity_open = false;
    gl_begin_audio_activity("ws resume");
    gl_start_tx_task();   /* idempotent: no-ops if the TX task survived the drop */
    s_gl.ws_resume_count++;
    ESP_LOGI(TAG, "WS resume OK (#%u) — session preserved",
             (unsigned)s_gl.ws_resume_count);
    return true;
}

static void gl_session_task(void *arg)
{
    (void)arg;

    while (!s_gl.stop_requested) {
        /* Wait for session_active */
        EventBits_t bits = xEventGroupWaitBits(s_gl.ev, GL_BIT_SESSION_ON | GL_BIT_STOP,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(200));
        if (bits & GL_BIT_STOP) {
            break;
        }
        if (!(bits & GL_BIT_SESSION_ON) || !s_gl.session_active) {
            /* No session: requests posted in the race window around teardown
             * are stale — discard them so they cannot fire into a future
             * session. */
            gl_drain_cmd_queue();
            continue;
        }
        /* Fresh session: drop any requests left over from before activation. */
        gl_drain_cmd_queue();
        s_gl.session_gen++;            /* invalidates tool jobs from prior sessions */
        s_gl.pending_resume = false;
        s_gl.first_audio_pending = false;
        gl_log_heap_snapshot("session start");   /* P0.4 logging half */
        if (!s_gl.api_key[0]) {
            ESP_LOGE(TAG, "No Gemini API key — set via dashboard (gemini_key)");
            xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
            s_gl.session_active = false;
            gl_set_state(GL_STATE_IDLE, "");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        char ws_path[512];
        snprintf(ws_path, sizeof(ws_path), "%s?key=%s", GEMINI_WS_PATH, s_gl.api_key);

        /* Join any in-flight async WS cleanup before creating a new client —
         * two clients alive at once would both feed the shared rx_buf via the
         * event handler (F6). The bit is set at init, cleared when a cleanup
         * task is spawned, re-set when it finishes. */
        EventBits_t cleaned = xEventGroupWaitBits(s_gl.ev, GL_BIT_WS_CLEANED,
                                                  pdFALSE, pdTRUE,
                                                  pdMS_TO_TICKS(GL_WS_CLEANUP_JOIN_MS));
        if (!(cleaned & GL_BIT_WS_CLEANED)) {
            ESP_LOGE(TAG, "WS cleanup still pending after %d ms; refusing new session",
                     (int)GL_WS_CLEANUP_JOIN_MS);
            goto session_cleanup;
        }

        ESP_LOGI(TAG, "Connecting to Gemini Live...");
        gl_set_state(GL_STATE_CONNECTING, "Connecting");

        esp_websocket_client_config_t ws_cfg = {
            .host                   = GEMINI_WS_HOST,
            .path                   = ws_path,
            .port                   = 443,
            .transport              = WEBSOCKET_TRANSPORT_OVER_SSL,
            .buffer_size            = 4096,
            .task_stack             = 8192,
            .task_prio              = 5,
            .network_timeout_ms     = 10000,
            .reconnect_timeout_ms   = 5000,
            .disable_auto_reconnect = true,
            /* Root-cause #1, Part B: client-side keepalive PING. A genuinely
             * dead socket (Wi-Fi gone, server hung) is then closed by a clean
             * PONG-timeout DISCONNECTED — which our in-session resume catches —
             * instead of waiting for the next writer to hit a write-0 abort
             * during silence. Auto-reconnect stays DISABLED: the explicit
             * gl_try_ws_resume path owns reconnection so the shared event
             * handler + rx_buf invariants (F6 two-clients window) are never
             * violated by the client reconnecting on its own. */
            .ping_interval_sec      = 20,
            .pingpong_timeout_sec   = 20,
            .keep_alive_enable      = true,
            .crt_bundle_attach      = esp_crt_bundle_attach,
        };
        ESP_LOGI(TAG, "WS init host=%s path=%s", GEMINI_WS_HOST, GEMINI_WS_PATH);
        s_gl.ws_client = esp_websocket_client_init(&ws_cfg);
        if (!s_gl.ws_client) {
            ESP_LOGE(TAG, "WS init failed");
            goto session_cleanup;
        }
        esp_websocket_register_events(s_gl.ws_client, WEBSOCKET_EVENT_ANY,
                                      gl_ws_event_handler, NULL);
        ESP_LOGI(TAG, "WS heap before start: internal=%u largest_internal=%u dma=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        esp_err_t ws_start = esp_websocket_client_start(s_gl.ws_client);
        ESP_LOGI(TAG, "WS start returned %s", esp_err_to_name(ws_start));
        if (ws_start != ESP_OK) {
            goto session_cleanup;
        }

        /* Wait for connection */
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
        while (!s_gl.ws_connected && !s_gl.stop_requested && s_gl.session_active) {
            if (xTaskGetTickCount() > deadline) {
                ESP_LOGE(TAG, "WS connect timeout");
                goto session_cleanup;
            }
            /* Consume requests even while connecting (handlers state-check,
             * so anything invalid for CONNECTING is discarded promptly). */
            gl_process_cmd_queue();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_gl.stop_requested || !s_gl.session_active) {
            goto session_cleanup;
        }

        /* Send setup */
        if (!gl_send_setup()) {
            ESP_LOGE(TAG, "Failed to send setup");
            goto session_cleanup;
        }

        /* Wait for setupComplete — drain rx_buf ourselves (gl-setup-poll).
         * The WS handler only xTaskNotifyGive()s us; it does NOT set the event
         * group, and the main receive loop that dispatches frames hasn't started
         * yet. A pure xEventGroupWaitBits here would never see setupComplete (it
         * arrives in rx_buf, undispatched) and time out at 10s. So we poll +
         * dispatch here, mirroring cap_gemini_live_test(). */
        xEventGroupClearBits(s_gl.ev, GL_BIT_SETUP_OK);
        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
        while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK) &&
               !s_gl.stop_requested && s_gl.session_active && s_gl.ws_connected) {
            if (xTaskGetTickCount() > deadline) {
                break;
            }
            /* Drain queued frames until setupComplete lands (sets GL_BIT_SETUP_OK). */
            gl_process_rx_queue(pdMS_TO_TICKS(100));
            gl_process_cmd_queue();
        }
        if (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK)) {
            ESP_LOGE(TAG, "setupComplete timeout (see WS RX logs for server reply)");
            goto session_cleanup;
        }

        /* Phase 1 verified: WSS + setup handshake works. */
        ESP_LOGI(TAG, "Gemini Live session ready");

        /* P2.4 (F11): open BOTH converters once for the whole session on the
         * shared 16 kHz I2S clock (intentional design — commit 66413b1).
         * Turn transitions only pause/resume capture and mute/unmute the
         * DAC; no esp_codec_dev_close/open per turn. */
        if (gl_open_adc(GL_CAP_SAMPLE_RATE) != ESP_CODEC_DEV_OK) {
            gl_set_audio_error("session: initial ADC open failed");
            ESP_LOGE(TAG, "Listening: ADC open failed, ending session");
            goto session_cleanup;
        }
        if (gl_open_dac(gl_resolve_playback_rate(GL_RX_SAMPLE_RATE)) == ESP_CODEC_DEV_OK) {
            gl_dac_mute(true);    /* silent until the first model turn */
        } else {
            ESP_LOGW(TAG, "Session: DAC pre-open failed; will retry at first audio");
        }

        /* P2.2 (F9): decoded-PCM ring + feeder. Allocation degrades by
         * halving and playback falls back to synchronous — never a crash. */
        gl_pcm_ring_alloc();
        gl_start_feeder_task();

        gl_set_state(GL_STATE_LISTENING, "Listening");
        gl_begin_audio_activity("initial listen");
        gl_start_tx_task();

        /* Main receive loop */
        while (s_gl.ws_connected && !s_gl.stop_requested && s_gl.session_active) {
            /* Drain ALL queued frames before re-checking loop conditions, so a
             * burst of audio chunks plays back-to-back without gaps. Requests
             * (tap-commit, HTTP send_text/end_input, local-VAD commit) are
             * consumed between frames so they act promptly even mid-burst. */
        while (gl_process_rx_queue(pdMS_TO_TICKS(200))) {
            gl_process_cmd_queue();
            if (s_gl.stop_requested || !s_gl.session_active) {
                break;
            }
        }
        gl_process_cmd_queue();
        /* Deferred turn-end (P2.1): a terminal frame arrived while the ring
         * was still draining; complete the resume once the feeder runs dry. */
        if (s_gl.pending_resume && !gl_playback_pending()) {
            gl_resume_listening(s_gl.pending_resume_reason);
        }
        gl_maybe_resume_speaking_watchdog();
        gl_ensure_listening_capture();
        /* WS dropped: a transport write-0 abort (root-cause #1) — most often a
         * benign TCP would-block, not a dead socket. Try to resume the SAME
         * client in place (re-handshake, audio path untouched) before tearing
         * the session down to IDLE. Only after every attempt fails do we break
         * to session_cleanup. */
        if (!s_gl.ws_connected) {
            bool resumed = false;
            for (int attempt = 1;
                 attempt <= GL_WS_RESUME_ATTEMPTS &&
                 !s_gl.stop_requested && s_gl.session_active;
                 ++attempt) {
                ESP_LOGW(TAG, "WS dropped; in-session resume attempt %d/%d",
                         attempt, GL_WS_RESUME_ATTEMPTS);
                if (gl_try_ws_resume()) {
                    resumed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(GL_WS_RESUME_BACKOFF_MS));
            }
            if (!resumed) {
                ESP_LOGW(TAG, "WS dropped, cleaning up session");
                break;
            }
        }
        }

session_cleanup:
        /* Stop audio TX and close codecs from this task only. The TX worker
         * never closes codec handles, avoiding cross-task I2S mutex deadlocks. */
        ESP_LOGI(TAG, "Teardown: stopping tx task");
        {
            bool tx_stopped = gl_stop_tx_task();
            for (int retry = 0; !tx_stopped && retry < 2; ++retry) {
                ESP_LOGE(TAG, "Teardown: TX task still parked; retrying stop wait (%d/2)",
                         retry + 1);
                tx_stopped = gl_stop_tx_task();
            }
            ESP_LOGI(TAG, "Teardown: tx task %s", tx_stopped ? "stopped" : "STILL RUNNING");
            if (tx_stopped) {
                ESP_LOGI(TAG, "Teardown: closing adc");
                gl_close_adc();
            } else {
                /* Never close the ADC under a live reader — that is the
                 * close-during-read crash this rework retires (F3). The task
                 * will exit on its own (bounded send timeouts) and the next
                 * session recovers via gl_ensure_listening_capture. */
                ESP_LOGE(TAG, "Teardown: skipping ADC close (TX task alive)");
            }
        }
        /* Flush first so the feeder isn't left draining seconds of backlog,
         * then park it BEFORE touching the DAC or the ring memory. If it
         * refuses to park, leave both alone (never close/free under a live
         * writer) — the next session reuses the surviving ring. */
        gl_pcm_ring_flush();
        ESP_LOGI(TAG, "Teardown: stopping feeder");
        if (gl_stop_feeder_task()) {
            ESP_LOGI(TAG, "Teardown: closing dac");
            gl_close_dac();
            /* P2.2 (F9): retain a full-size ring across sessions. Emote rwave
             * clip swaps (~0.7-0.9 MB each) fragment PSRAM while a session
             * runs; once a freed 1 MB ring's hole is carved up, the largest
             * free block drops to ~430 KB and every later session degrades to
             * a 256 KB (~8 s) ring — long replies then overflow the ring and
             * the raw-queue byte cap (observed: 53 drops in one story turn).
             * Only release a degraded ring so the next session start can
             * retry the full-size allocation. */
            if (s_gl.pcm_ring && s_gl.pcm_ring_cap < GL_PCM_RING_BYTES) {
                gl_pcm_ring_free();
            }
        } else {
            ESP_LOGE(TAG, "Teardown: skipping DAC close + ring free (feeder alive)");
        }

        ESP_LOGI(TAG, "Teardown: closing websocket");
        ESP_LOGI(TAG, "Session cleanup");
        gl_drain_rx_queue();
        gl_drain_cmd_queue();
        /* Detach the client under ws_mutex: an in-flight sender holds the
         * mutex through its send, so by the time we own the mutex no sender
         * can still be using the old handle, and any later sender snapshots
         * NULL (F2). Destruction itself happens off-mutex in the cleanup
         * task — the handle is already unreachable. */
        esp_websocket_client_handle_t ws_client = NULL;
        xSemaphoreTake(s_gl.ws_mutex, portMAX_DELAY);
        ws_client = s_gl.ws_client;
        s_gl.ws_client = NULL;
        s_gl.ws_connected = false;
        xSemaphoreGive(s_gl.ws_mutex);
        s_gl.session_active = false;
        s_gl.activity_open = false;
        s_gl.pending_resume = false;
        s_gl.first_audio_pending = false;
        s_gl.state = GL_STATE_IDLE;
        atomic_store(&s_out_rms, 0);
        emote_set_voice_idle();
        xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
        if (ws_client) {
            xEventGroupClearBits(s_gl.ev, GL_BIT_WS_CLEANED);
            if (xTaskCreate(gl_ws_cleanup_task, "gl_ws_cleanup", 4096, ws_client, 3, NULL) != pdPASS) {
                ESP_LOGW(TAG, "WS cleanup task create failed; destroying inline");
                esp_websocket_client_destroy(ws_client);
                xEventGroupSetBits(s_gl.ev, GL_BIT_WS_CLEANED);
            }
        }
        gl_log_heap_snapshot("session stop");   /* P0.4 logging half */
    }

    ESP_LOGI(TAG, "Session task exiting");
    s_gl.stop_requested = false;
    s_gl.session_active = false;
    s_gl.ws_connected = false;
    if (s_gl.session_task == xTaskGetCurrentTaskHandle()) {
        s_gl.session_task = NULL;
    }
    if (s_gl.ev) {
        xEventGroupSetBits(s_gl.ev, GL_BIT_SESSION_DONE);
    }
    claw_task_delete(NULL);
}

/* Touch input is handled by touch_monitor_task (main app layer), which calls
 * cap_gemini_live_toggle() — a request-poster. The old in-component
 * gl_touch_task (Phase 5) was dead code that mutated session state outside
 * the session task; removed in the P1 lifecycle rework. */

/* ---- Lifecycle ------------------------------------------------------------ */

/* Start/stop are guarded by s_gl_lifecycle_mutex so the check-then-create on
 * s_gl.session_task is atomic: a concurrent tap + HTTP action=start can never
 * spawn two gl_session tasks sharing one s_gl (F5 / P1.4). */
static esp_err_t gl_gateway_start(void)
{
    esp_err_t err = ESP_OK;

    gl_lifecycle_lock();
    if (s_gl.session_task) {
        if (s_gl.stop_requested) {
            ESP_LOGW(TAG, "Gemini Live gateway stop still in progress");
            err = ESP_ERR_INVALID_STATE;
        }
        goto out;
    }
    if (!s_gl.rx_buf) {
        s_gl.rx_buf = heap_caps_malloc(GL_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_gl.rx_buf) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    s_gl.rx_buf[0] = '\0';
    if (!s_gl.rx_queue) {
        s_gl.rx_queue = xQueueCreate(GL_RX_QUEUE_DEPTH, sizeof(char *));
        if (!s_gl.rx_queue) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    if (!s_gl.cmd_queue) {
        s_gl.cmd_queue = xQueueCreate(GL_CMD_QUEUE_DEPTH, sizeof(gl_cmd_t));
        if (!s_gl.cmd_queue) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    if (!s_gl.tx_frame_queue) {
        /* By-value 32 ms mic frames (P2.3): 16 × 1024 B, one-time allocation. */
        s_gl.tx_frame_queue = xQueueCreate(GL_TX_FRAME_QUEUE_DEPTH, GL_TX_PCM_BYTES);
        if (!s_gl.tx_frame_queue) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    if (!s_gl.tool_queue) {
        s_gl.tool_queue = xQueueCreate(GL_TOOL_QUEUE_DEPTH, sizeof(gl_tool_job_t));
        if (!s_gl.tool_queue) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    if (!s_gl.ring_mutex) {
        s_gl.ring_mutex = xSemaphoreCreateMutex();
        if (!s_gl.ring_mutex) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    s_gl.stop_requested = false;
    gl_reset_diag_counters();
    s_gl.dac_codec_failed = false;
    s_gl.adc_codec_failed = false;

    if (!s_gl.ev) {
        s_gl.ev = xEventGroupCreate();
        if (!s_gl.ev) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        /* Fresh event group: no WS cleanup can be in flight. */
        xEventGroupSetBits(s_gl.ev, GL_BIT_WS_CLEANED);
    }
    if (!s_gl.ws_mutex) {
        s_gl.ws_mutex = xSemaphoreCreateMutex();
        if (!s_gl.ws_mutex) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
    }

    /* Clear stop/done bits left over from any previous gateway run.
     * GL_BIT_WS_CLEANED is deliberately NOT touched: it tracks the async WS
     * cleanup task, which can outlive a gateway run. Feeder bits are managed
     * by gl_start_feeder_task per session. */
    xEventGroupClearBits(s_gl.ev,
                         GL_BIT_STOP | GL_BIT_TX_STOP | GL_BIT_TX_DONE |
                         GL_BIT_TXS_DONE | GL_BIT_SESSION_DONE);

    gl_acquire_codec_handles();

    /* Persistent tool worker (P2.1/F8): outlives sessions, never deleted —
     * no teardown races. Stale jobs die on the generation check. */
    if (!s_gl.tool_task) {
        static const claw_task_config_t tool_cfg = {
            .name         = "gl_tool_worker",
            .stack_size   = 12288,          /* TLS HTTPS client runs here */
            .priority     = 4,
            .core_id      = tskNO_AFFINITY,
            .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
        };
        if (claw_task_create(&tool_cfg, gl_tool_worker_task, NULL, &s_gl.tool_task) != pdPASS) {
            s_gl.tool_task = NULL;
            ESP_LOGE(TAG, "tool worker create failed; toolCalls will be dropped");
        }
    }

    static const claw_task_config_t sess_cfg = {
        .name         = "gl_session",
        .stack_size   = 12288,
        .priority     = 5,
        .core_id      = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    if (claw_task_create(&sess_cfg, gl_session_task, NULL, &s_gl.session_task) != pdPASS) {
        s_gl.session_task = NULL;
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    /* Touch toggle is driven by touch_monitor_task in main.c (app_claw layer),
     * which calls cap_gemini_live_toggle() on rising-edge tap. Do not start a
     * second touch-polling task here — two tasks on the same I2C bus race the
     * controller and cause phantom double-taps + I2S codec churn. */

    ESP_LOGI(TAG, "Gemini Live gateway started");

out:
    gl_lifecycle_unlock();
    return err;
}

static esp_err_t gl_gateway_stop(void)
{
    /* Request-poster only: flags + event bits. The session task performs the
     * actual codec/TX/WS teardown. Never block here — stop may be called from
     * the HTTP server or the touch monitor. */
    gl_lifecycle_lock();
    s_gl.stop_requested = true;
    s_gl.session_active = false;

    if (!s_gl.ev) {
        gl_lifecycle_unlock();
        return ESP_OK;
    }

    TaskHandle_t session_task = s_gl.session_task;
    xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
    xEventGroupSetBits(s_gl.ev, GL_BIT_STOP | GL_BIT_TX_STOP);
    gl_lifecycle_unlock();

    if (session_task == xTaskGetCurrentTaskHandle()) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Gateway stop requested");
    return ESP_OK;
}

/* ---- Public API ---------------------------------------------------------- */

esp_err_t cap_gemini_live_set_api_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_gl.api_key, api_key, sizeof(s_gl.api_key));
    return ESP_OK;
}

esp_err_t cap_gemini_live_set_mcp_key(const char *mcp_key)
{
    if (!mcp_key) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_gl.mcp_key, mcp_key, sizeof(s_gl.mcp_key));
    return ESP_OK;
}

esp_err_t cap_gemini_live_set_mcp_url(const char *mcp_url)
{
    if (!mcp_url) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_gl.mcp_url, mcp_url, sizeof(s_gl.mcp_url));
    return ESP_OK;
}

/* ---- Audio-level hooks for the reactive waveform (display layer) ----------
 * Normalised 0.0..1.0 (RMS of the int16 PCM block / 32768). The display layer's
 * amplitude-source adapter (patch 0032) reads these and scales to its 0..1000.
 * Lock-free (plain atomics) — safe to poll from the display task. */

float cap_gemini_live_get_mic_level(void)
{
    return (float)atomic_load(&s_mic_rms) / 32768.0f;
}

float cap_gemini_live_get_output_level(void)
{
    return (float)atomic_load(&s_out_rms) / 32768.0f;
}

void cap_gemini_live_print_diagnostics(void)
{
    EventBits_t bits = s_gl.ev ? xEventGroupGetBits(s_gl.ev) : 0;
    UBaseType_t rx_depth = s_gl.rx_queue ? uxQueueMessagesWaiting(s_gl.rx_queue) : 0;
    int64_t now_us = esp_timer_get_time();
    int64_t frame_age = s_gl.last_frame_us ? (now_us - s_gl.last_frame_us) / 1000 : -1;
    int64_t audio_age = s_gl.last_audio_us ? (now_us - s_gl.last_audio_us) / 1000 : -1;
    int64_t resume_age = s_gl.last_resume_us ? (now_us - s_gl.last_resume_us) / 1000 : -1;

    printf("Gemini Live diagnostics:\n");
    printf("  state=%s listening=%d connected=%d session_active=%d stop_requested=%d\n",
           gl_state_name(s_gl.state),
           (int)(s_gl.state == GL_STATE_LISTENING || s_gl.state == GL_STATE_READY),
           (int)s_gl.ws_connected,
           (int)s_gl.session_active,
           (int)s_gl.stop_requested);
    printf("  capture: tx_task=%d dac_open=%d adc_open=%d dac=%p raw_dac=%p adc=%p raw_adc=%p\n",
           (int)(s_gl.tx_task != NULL),
           (int)s_gl.dac_open,
           (int)s_gl.adc_open,
           (void *)s_gl.dac,
           (void *)s_gl.dac_chan,
           (void *)s_gl.adc,
           (void *)s_gl.adc_chan);
    printf("  raw cfg rates: dac=%u adc=%u\n",
           (unsigned)s_gl.dac_raw_rate_hz,
           (unsigned)s_gl.adc_raw_rate_hz);
    printf("  tx: frames_sent=%u send_failures=%u read_failures=%u codec_reads=%u raw_reads=%u\n",
           (unsigned)s_gl.tx_frames_sent,
           (unsigned)s_gl.tx_send_failures,
           (unsigned)s_gl.tx_read_failures,
           (unsigned)s_gl.tx_codec_reads,
           (unsigned)s_gl.tx_raw_reads);
    printf("  active dac rate=%u last_audio_mime_rate=%u rate_mismatch_chunks=%u\n",
           (unsigned)s_gl.dac_rate,
           (unsigned)s_gl.last_audio_mime_rate,
           (unsigned)s_gl.rate_mismatch_chunks);
    printf("  audio path state: dac_codec_failed=%d adc_codec_failed=%d\n",
           (int)s_gl.dac_codec_failed,
           (int)s_gl.adc_codec_failed);
    printf("  audio path: errors=%s\n", s_gl.last_audio_error[0] ? s_gl.last_audio_error : "-");
    printf("  bits=0x%08x queue_depth=%u drops=%u frames=%u text_parts=%u audio_parts=%u\n",
           (unsigned)bits, (unsigned)rx_depth,
           (unsigned)s_gl.rx_drops, (unsigned)s_gl.rx_frames,
           (unsigned)s_gl.text_part_hits, (unsigned)s_gl.audio_part_hits);
    printf("  turn_complete=%u generation_complete=%u interrupted=%u tool_calls=%u unhandled=%u\n",
           (unsigned)s_gl.turn_complete_hits,
           (unsigned)s_gl.generation_complete_hits,
           (unsigned)s_gl.interrupted_hits,
           (unsigned)s_gl.tool_call_hits,
           (unsigned)s_gl.unhandled_hits);
    printf("  barge: server_vad=%d hits=%u rms_thr=%u stream_ends=%u tool_cancels=%u\n",
           (int)GL_USE_SERVER_VAD,
           (unsigned)s_gl.barge_hits,
           (unsigned)atomic_load(&s_barge_rms),
           (unsigned)s_gl.audio_stream_end_hits,
           (unsigned)s_gl.tool_cancel_hits);
    printf("  pipeline: ring=%u/%u B ring_drop=%u rx_q_bytes=%u tx_q_depth=%u tool_inflight=%u first_audio_ms=%u\n",
           (unsigned)s_gl.pcm_ring_bytes,
           (unsigned)s_gl.pcm_ring_cap,
           (unsigned)s_gl.pcm_ring_drop_bytes,
           (unsigned)atomic_load(&s_rx_queue_bytes),
           (unsigned)(s_gl.tx_frame_queue ? uxQueueMessagesWaiting(s_gl.tx_frame_queue) : 0),
           (unsigned)atomic_load(&s_tool_inflight),
           (unsigned)s_gl.last_first_audio_ms);
#if GL_USE_AEC
    printf("  aec: enabled=%d frames=%u cost_p50_us=%u cost_p95_us=%u atten_db10=%d\n",
           (int)atomic_load(&s_aec_enabled),
           (unsigned)atomic_load(&s_aec_frames),
           (unsigned)atomic_load(&s_aec_cost_p50_us),
           (unsigned)atomic_load(&s_aec_cost_p95_us),
           (int)atomic_load(&s_aec_atten_db10));
#endif
    printf("  tasks: tx=%d sender=%d feeder=%d tool=%d pending_resume=%d\n",
           (int)(s_gl.tx_task != NULL),
           (int)(s_gl.tx_sender_task != NULL),
           (int)(s_gl.feeder_task != NULL),
           (int)(s_gl.tool_task != NULL),
           (int)s_gl.pending_resume);
    printf("  resumes=%u watchdog_resumes=%u last_resume=%s (%lld ms ago) waiting_terminal=%d\n",
           (unsigned)s_gl.resume_count,
           (unsigned)s_gl.watchdog_resume_count,
           s_gl.last_resume_reason[0] ? s_gl.last_resume_reason : "-",
           (long long)resume_age,
           (int)s_gl.waiting_terminal);
    printf("  frame_age_ms=%lld audio_age_ms=%lld\n",
           (long long)frame_age, (long long)audio_age);
}

esp_err_t cap_gemini_live_get_diagnostics_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    EventBits_t bits = s_gl.ev ? xEventGroupGetBits(s_gl.ev) : 0;
    UBaseType_t rx_depth = s_gl.rx_queue ? uxQueueMessagesWaiting(s_gl.rx_queue) : 0;
    int64_t now_us = esp_timer_get_time();
    int64_t frame_age = s_gl.last_frame_us ? (now_us - s_gl.last_frame_us) / 1000 : -1;
    int64_t audio_age = s_gl.last_audio_us ? (now_us - s_gl.last_audio_us) / 1000 : -1;
    int64_t resume_age = s_gl.last_resume_us ? (now_us - s_gl.last_resume_us) / 1000 : -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "state", gl_state_name(s_gl.state));
    cJSON_AddBoolToObject(root, "listening", s_gl.state == GL_STATE_LISTENING || s_gl.state == GL_STATE_READY);
    cJSON_AddBoolToObject(root, "connected", s_gl.ws_connected);
    cJSON_AddBoolToObject(root, "session_active", s_gl.session_active);
    cJSON_AddBoolToObject(root, "stop_requested", s_gl.stop_requested);
    cJSON_AddBoolToObject(root, "tx_task", s_gl.tx_task != NULL);
    cJSON_AddBoolToObject(root, "activity_open", s_gl.activity_open);
    cJSON_AddBoolToObject(root, "dac_open", s_gl.dac_open);
    cJSON_AddBoolToObject(root, "adc_open", s_gl.adc_open);
    cJSON_AddBoolToObject(root, "dac_raw", s_gl.dac_raw);
    cJSON_AddBoolToObject(root, "adc_raw", s_gl.adc_raw);
    cJSON_AddNumberToObject(root, "dac_rate", (double)s_gl.dac_rate);
    cJSON_AddNumberToObject(root, "dac_raw_rate_hz", (double)s_gl.dac_raw_rate_hz);
    cJSON_AddNumberToObject(root, "adc_raw_rate_hz", (double)s_gl.adc_raw_rate_hz);
    cJSON_AddNumberToObject(root, "last_audio_mime_rate", (double)s_gl.last_audio_mime_rate);
    cJSON_AddNumberToObject(root, "rate_mismatch_chunks", (double)s_gl.rate_mismatch_chunks);
    cJSON_AddNumberToObject(root, "tx_frames_sent", (double)s_gl.tx_frames_sent);
    cJSON_AddNumberToObject(root, "tx_send_failures", (double)s_gl.tx_send_failures);
    cJSON_AddNumberToObject(root, "ws_resume_count", (double)s_gl.ws_resume_count);
    cJSON_AddNumberToObject(root, "tx_read_failures", (double)s_gl.tx_read_failures);
    cJSON_AddNumberToObject(root, "tx_codec_reads", (double)s_gl.tx_codec_reads);
    cJSON_AddNumberToObject(root, "tx_raw_reads", (double)s_gl.tx_raw_reads);
    cJSON_AddBoolToObject(root, "dac_codec_failed", s_gl.dac_codec_failed);
    cJSON_AddBoolToObject(root, "adc_codec_failed", s_gl.adc_codec_failed);
    cJSON_AddStringToObject(root, "audio_error", s_gl.last_audio_error[0] ? s_gl.last_audio_error : "-");
    cJSON_AddNumberToObject(root, "bits", (double)bits);
    cJSON_AddNumberToObject(root, "queue_depth", (double)rx_depth);
    cJSON_AddNumberToObject(root, "drops", (double)s_gl.rx_drops);
    cJSON_AddNumberToObject(root, "frames", (double)s_gl.rx_frames);
    cJSON_AddNumberToObject(root, "text_parts", (double)s_gl.text_part_hits);
    cJSON_AddNumberToObject(root, "audio_parts", (double)s_gl.audio_part_hits);
    cJSON_AddNumberToObject(root, "turn_complete", (double)s_gl.turn_complete_hits);
    cJSON_AddNumberToObject(root, "generation_complete", (double)s_gl.generation_complete_hits);
    cJSON_AddNumberToObject(root, "interrupted", (double)s_gl.interrupted_hits);
    /* P3.4 barge-in telemetry: server_vad = turn-taking mode; barge_hits =
     * local detector fires; barge_rms_threshold = live calibration value;
     * audio_stream_ends = auto-VAD pause flushes; tool_cancellations =
     * server-side cancellations after a barge. */
    cJSON_AddBoolToObject(root, "server_vad", GL_USE_SERVER_VAD != 0);
    /* Self-interrupt cure (2026-06-13): how long the THINKING watchdog waits
     * before resuming, and whether the server honors client activityStart as a
     * barge-in. In manual mode activity_handling is NO_INTERRUPTION so a
     * resume-path activityStart can never cancel the model's pending reply;
     * speak_watchdog_ms is the no-audio timeout (20000) that only catches a
     * genuinely dead turn. Both confirm the running binary carries the fix. */
    cJSON_AddNumberToObject(root, "speak_watchdog_ms", (double)GL_SPEAK_WATCHDOG_MS);
    cJSON_AddStringToObject(root, "activity_handling",
                            (GL_USE_SERVER_VAD || atomic_load(&s_activity_interrupts))
                                ? "START_OF_ACTIVITY_INTERRUPTS"
                                : "NO_INTERRUPTION");
    cJSON_AddNumberToObject(root, "barge_hits", (double)s_gl.barge_hits);
    cJSON_AddNumberToObject(root, "barge_rms_threshold", (double)atomic_load(&s_barge_rms));
    cJSON_AddNumberToObject(root, "barge_guard_ms", (double)atomic_load(&s_barge_guard_ms));
    cJSON_AddNumberToObject(root, "barge_ratio_pct", (double)atomic_load(&s_barge_ratio_pct));
    cJSON_AddNumberToObject(root, "vad_speech_rms", (double)atomic_load(&s_vad_speech_rms));
    cJSON_AddNumberToObject(root, "vad_silence_rms", (double)atomic_load(&s_vad_silence_rms));
    cJSON_AddNumberToObject(root, "mic_pga_db", (double)atomic_load(&s_mic_pga_db));
    cJSON_AddNumberToObject(root, "speak_pga_db", (double)atomic_load(&s_mic_pga_speak_db));
    cJSON_AddNumberToObject(root, "ref_pga_db", (double)atomic_load(&s_ref_pga_db));
    cJSON_AddNumberToObject(root, "out_vol", (double)atomic_load(&s_out_vol));
    cJSON_AddNumberToObject(root, "audio_stream_ends", (double)s_gl.audio_stream_end_hits);
    cJSON_AddNumberToObject(root, "tool_cancellations", (double)s_gl.tool_cancel_hits);
    cJSON_AddNumberToObject(root, "tool_calls", (double)s_gl.tool_call_hits);
    cJSON_AddNumberToObject(root, "unhandled", (double)s_gl.unhandled_hits);
    cJSON_AddNumberToObject(root, "pcm_ring_bytes", (double)s_gl.pcm_ring_bytes);
    cJSON_AddNumberToObject(root, "pcm_ring_cap", (double)s_gl.pcm_ring_cap);
    cJSON_AddNumberToObject(root, "pcm_ring_drop_bytes", (double)s_gl.pcm_ring_drop_bytes);
    cJSON_AddNumberToObject(root, "rx_queue_bytes", (double)atomic_load(&s_rx_queue_bytes));
    cJSON_AddNumberToObject(root, "tx_queue_depth",
                            (double)(s_gl.tx_frame_queue ? uxQueueMessagesWaiting(s_gl.tx_frame_queue) : 0));
    cJSON_AddNumberToObject(root, "tool_inflight", (double)atomic_load(&s_tool_inflight));
    cJSON_AddNumberToObject(root, "first_audio_ms", (double)s_gl.last_first_audio_ms);
    cJSON_AddBoolToObject(root, "feeder_task", s_gl.feeder_task != NULL);
    cJSON_AddBoolToObject(root, "tx_sender_task", s_gl.tx_sender_task != NULL);
    cJSON_AddBoolToObject(root, "pending_resume", s_gl.pending_resume);
    cJSON_AddNumberToObject(root, "resumes", (double)s_gl.resume_count);
    cJSON_AddNumberToObject(root, "watchdog_resumes", (double)s_gl.watchdog_resume_count);
    cJSON_AddNumberToObject(root, "frame_age_ms", (double)frame_age);
    cJSON_AddNumberToObject(root, "audio_age_ms", (double)audio_age);
    cJSON_AddNumberToObject(root, "last_resume_ms", (double)resume_age);
    cJSON_AddStringToObject(root, "last_resume_reason",
                            s_gl.last_resume_reason[0] ? s_gl.last_resume_reason : "-");
    cJSON_AddNumberToObject(root, "mic_level", (double)cap_gemini_live_get_mic_level());
    cJSON_AddNumberToObject(root, "output_level", (double)cap_gemini_live_get_output_level());
#if GL_USE_AEC
    /* P3.3 instrumentation: AEC health — engine state, per-frame cost
     * percentiles (last ~10 s window, us) and the playback echo-attenuation
     * estimate (raw-mic vs clean-mic RMS, dB; >0 means echo removed). */
    cJSON_AddBoolToObject(root, "aec_enabled", atomic_load(&s_aec_enabled));
    cJSON_AddNumberToObject(root, "aec_frames", (double)atomic_load(&s_aec_frames));
    cJSON_AddNumberToObject(root, "aec_cost_p50_us", (double)atomic_load(&s_aec_cost_p50_us));
    cJSON_AddNumberToObject(root, "aec_cost_p95_us", (double)atomic_load(&s_aec_cost_p95_us));
    cJSON_AddNumberToObject(root, "aec_atten_db", (double)atomic_load(&s_aec_atten_db10) / 10.0);
#endif
#if GL_LANE_DIAG
    /* Raw per-lane capture levels, last 1 s window (aec_atten cross-check). */
    {
        cJSON *lr = cJSON_AddArrayToObject(root, "lane_rms");
        cJSON *lp = cJSON_AddArrayToObject(root, "lane_peak");
        for (int l = 0; l < GL_CAPTURE_CHANNELS; ++l) {
            cJSON_AddItemToArray(lr, cJSON_CreateNumber((double)atomic_load(&s_lane_rms[l])));
            cJSON_AddItemToArray(lp, cJSON_CreateNumber((double)atomic_load(&s_lane_peak[l])));
        }
    }
#endif

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (strlen(json) + 1 > out_size) {
        free(json);
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(out, json, out_size);
    free(json);
    return ESP_OK;
}

/* Runtime calibration for the local barge detector (P3.4): RMS threshold on
 * the AEC-cleaned post-gain mic during SPEAKING. 0 disables the detector
 * (tap interrupt still works). Lock-free; takes effect on the next captured
 * frame. Exposed via `gemini-live --barge-rms <n>` for on-device tuning. */
void cap_gemini_live_set_barge_rms(uint16_t rms)
{
    atomic_store(&s_barge_rms, rms);
    ESP_LOGI(TAG, "barge RMS threshold set to %u%s", (unsigned)rms,
             rms == 0 ? " (barge detector disabled)" : "");
}

/* Runtime toggle for server-side barge-in (manual mode). 1 =
 * START_OF_ACTIVITY_INTERRUPTS (barge stops her), 0 = NO_INTERRUPTION (replies
 * never cancelled, no barge). Read at session setup, so this takes effect on the
 * NEXT session — restart voice (or it applies on the next WS (re)connect). */
void cap_gemini_live_set_activity_interrupts(int enable)
{
    atomic_store(&s_activity_interrupts, enable ? 1 : 0);
    ESP_LOGI(TAG, "activityHandling -> %s (applies next session)",
             enable ? "START_OF_ACTIVITY_INTERRUPTS" : "NO_INTERRUPTION");
}

/* Runtime AEC gain staging for hardware calibration (2026-06-12). Any arg < 0
 * is left unchanged. Stores the value and, if a session is live, applies it
 * immediately (I2C control writes, off the I2S read/write path). Exposed via
 * GET /api/debug/gain?mic=&ref=&vol= so the echo-vs-reference levels can be
 * swept without reflashing — read back the result from aec_atten_db/lane_rms. */
void cap_gemini_live_set_in_gains(int mic_db, int ref_db, int out_vol)
{
    if (mic_db >= 0)  atomic_store(&s_mic_pga_db, mic_db);
    if (ref_db >= 0)  atomic_store(&s_ref_pga_db, ref_db);
    if (out_vol >= 0) atomic_store(&s_out_vol, out_vol);
    if (s_gl.adc && !s_gl.adc_codec_failed && s_gl.adc_open) {
        gl_apply_in_gains();
    }
    if (out_vol >= 0 && s_gl.dac && !s_gl.dac_codec_failed && s_gl.dac_open) {
        esp_codec_dev_set_out_vol(s_gl.dac, atomic_load(&s_out_vol));
    }
    ESP_LOGI(TAG, "in-gains set: mic=%d dB ref=%d dB out_vol=%d",
             atomic_load(&s_mic_pga_db), atomic_load(&s_ref_pga_db), atomic_load(&s_out_vol));
}

/* During-SPEAKING mic gain (barge calibration). Raising it lifts a real barge
 * above the post-AEC residual echo so the local detector can catch it; too high
 * re-clips the echo into the AEC. Applies immediately (re-runs gl_apply_in_gains)
 * so it can be swept mid-reply via /api/debug/gain?speak=N. <0 = unchanged. */
void cap_gemini_live_set_speak_gain(int db)
{
    if (db < 0) {
        return;
    }
    atomic_store(&s_mic_pga_speak_db, db);
    if (s_gl.adc && !s_gl.adc_codec_failed && s_gl.adc_open) {
        gl_apply_in_gains();   /* takes effect now if currently SPEAKING */
    }
    ESP_LOGI(TAG, "speak mic gain set to %d dB (barge calibration)", db);
}

/* Runtime local-VAD turn-commit thresholds (manual mode). Lets hands-free turn
 * detection be matched to the user's mic level live: speech = RMS to start a
 * turn, silence = RMS (below) to end it. <0 = leave unchanged. Lock-free; the
 * capture task picks them up on the next frame. */
void cap_gemini_live_set_vad(int speech_rms, int silence_rms)
{
    if (speech_rms >= 0)  atomic_store(&s_vad_speech_rms, speech_rms);
    if (silence_rms >= 0) atomic_store(&s_vad_silence_rms, silence_rms);
    ESP_LOGI(TAG, "VAD thresholds set: speech=%d silence=%d",
             atomic_load(&s_vad_speech_rms), atomic_load(&s_vad_silence_rms));
}

void cap_gemini_live_set_barge_guard_ms(int ms)
{
    if (ms < 0) return;
    atomic_store(&s_barge_guard_ms, ms);
    ESP_LOGI(TAG, "Barge guard window set: %d ms", ms);
}

void cap_gemini_live_set_barge_ratio_pct(int pct)
{
    if (pct < 0) return;
    atomic_store(&s_barge_ratio_pct, pct);
    ESP_LOGI(TAG, "Barge adaptive ratio set: %d%% of playback", pct);
}

/* Optional: drive the levels with no live session (own audio-path testing). */
void cap_gemini_live_set_synthetic_levels(float mic, float out)
{
    if (mic < 0.0f) mic = 0.0f; else if (mic > 1.0f) mic = 1.0f;
    if (out < 0.0f) out = 0.0f; else if (out > 1.0f) out = 1.0f;
    atomic_store(&s_mic_rms, (uint16_t)(mic * 32767.0f));
    atomic_store(&s_out_rms, (uint16_t)(out * 32767.0f));
}

esp_err_t cap_gemini_live_start(void)
{
    esp_err_t err = gl_gateway_start();
    if (err != ESP_OK) {
        return err;
    }
    /* Activate the first session immediately. Under the lifecycle mutex so a
     * racing stop cannot interleave between activation flag and event bit. */
    gl_lifecycle_lock();
    s_gl.session_active = true;
    xEventGroupSetBits(s_gl.ev, GL_BIT_SESSION_ON);
    gl_lifecycle_unlock();
    return ESP_OK;
}

esp_err_t cap_gemini_live_stop(void)
{
    return gl_gateway_stop();
}

/* Request-poster (httpd / CLI context): the session task performs the actual
 * TX stop + codec close + WS send (F3 — codec lifecycle was previously run
 * directly on the httpd task here). ESP_OK means "queued", not "sent". */
esp_err_t cap_gemini_live_send_text(const char *text)
{
    if (!text || !s_gl.session_active ||
        (s_gl.state != GL_STATE_READY && s_gl.state != GL_STATE_LISTENING && s_gl.state != GL_STATE_SPEAKING)) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = strlen(text) + 1;
    char *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = malloc(len);
    }
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, text, len);
    esp_err_t err = gl_post_cmd(GL_CMD_SEND_TEXT, copy);
    if (err != ESP_OK) {
        free(copy);
    }
    return err;
}

/* Request-poster (httpd / touch context): see gl_do_end_input for the actual
 * lifecycle work (F3/F4 — this used to run codec teardown + a TLS WS send on
 * the 4 KB touch_mon stack). */
esp_err_t cap_gemini_live_end_input(void)
{
    if (!s_gl.session_active ||
        (s_gl.state != GL_STATE_READY && s_gl.state != GL_STATE_LISTENING)) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Server VAD owns turn end; manual end_input would race the server and
     * leave the model wedged in THINKING with no serverContent. Treat as
     * idempotent OK so external callers (HTTP /api/gemini/live action=end_input)
     * stay backward-compatible. */
    if (GL_USE_SERVER_VAD) {
        return ESP_OK;
    }
    return gl_post_cmd(GL_CMD_END_INPUT, NULL);
}

bool cap_gemini_live_is_active(void)
{
    return s_gl.session_active && s_gl.state != GL_STATE_IDLE;
}

void cap_gemini_live_toggle(void)
{
    /* Debounce rapid taps so an in-flight connect is not torn down mid-handshake. */
    static int64_t last_toggle_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_toggle_us < (int64_t)GL_TOGGLE_COOLDOWN_MS * 1000) {
        ESP_LOGW(TAG, "Tap ignored (debounce %d ms)", GL_TOGGLE_COOLDOWN_MS);
        return;
    }
    last_toggle_us = now_us;

    /* Don't tear down a session that's mid-handshake — ignore the tap and let it
     * reach READY (or time out). Prevents half-connected sessions and audio-path
     * churn when the user taps impatiently during connect. */
    if (s_gl.session_active && s_gl.state == GL_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Tap ignored (session still connecting)");
        return;
    }

    if (!s_gl.session_task) {
        cap_gemini_live_start();
        return;
    }
    if (s_gl.session_active) {
        /* Tap during SPEAKING = barge-in (P3.1): the session task flushes the
         * PCM ring + rx queue, sends activityStart, and returns to LISTENING.
         * The session stays up — her voice stops, her ears open. */
        if (s_gl.state == GL_STATE_SPEAKING) {
            ESP_LOGI(TAG, "Tap: interrupting playback");
            if (gl_post_cmd(GL_CMD_INTERRUPT, NULL) != ESP_OK) {
                ESP_LOGW(TAG, "Tap: interrupt request dropped (cmd queue full)");
            }
            return;
        }
        if (!GL_USE_SERVER_VAD &&
            (s_gl.state == GL_STATE_READY || s_gl.state == GL_STATE_LISTENING)) {
            /* Posts GL_CMD_END_INPUT; the session task commits the turn. */
            ESP_LOGI(TAG, "Tap: ending input stream");
            cap_gemini_live_end_input();
            return;
        }
        /* THINKING = a reply is being generated. Tearing the session down here
         * is the worst outcome — the user (who is usually tapping because the
         * reply feels slow) kills their own answer and reads it as "it never
         * replied" (seen live: 16 s grounding delay → three taps → dead turn).
         * Ignore taps for the first 10 s of THINKING; after that the user has
         * waited long enough that "get me out" is the honest intent. */
        if (s_gl.state == GL_STATE_THINKING &&
            (now_us - s_gl.thinking_since_us) < 10 * 1000 * 1000LL) {
            ESP_LOGI(TAG, "Tap ignored: reply pending (THINKING %lld ms) — taps stop it after 10 s",
                     (long long)((now_us - s_gl.thinking_since_us) / 1000));
            return;
        }
        ESP_LOGI(TAG, "Tap: stopping session");
        cap_gemini_live_stop();
    } else {
        /* Gateway up, session off — reactivate through the same locked path
         * as a cold start (atomic with a racing stop). */
        ESP_LOGI(TAG, "Tap: starting session");
        cap_gemini_live_start();
    }
}

/* Phase 1 test: connect → setup → wait setupComplete → disconnect */
esp_err_t cap_gemini_live_test(void)
{
    if (!s_gl.api_key[0]) {
        ESP_LOGE(TAG, "No API key — set via dashboard (gemini_key NVS field)");
        return ESP_ERR_INVALID_STATE;
    }
    /* The test shares s_gl.ws_client / rx_buf / the WS event handler with the
     * session path. Running it while the gateway is up would put a second WS
     * client on the same state from the CLI task — exactly the cross-task
     * lifecycle access P1 retires. Refuse instead. */
    if (s_gl.session_task) {
        ESP_LOGE(TAG, "gemini-live test refused: gateway is running (stop it first)");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure event group exists */
    if (!s_gl.ev) {
        s_gl.ev = xEventGroupCreate();
        if (!s_gl.ev) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_gl.ws_mutex) {
        s_gl.ws_mutex = xSemaphoreCreateMutex();
        if (!s_gl.ws_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_gl.rx_buf) {
        s_gl.rx_buf = heap_caps_malloc(GL_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_gl.rx_buf) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_gl.rx_buf[0] = '\0';

    char ws_path[512];
    snprintf(ws_path, sizeof(ws_path), "%s?key=%s", GEMINI_WS_PATH, s_gl.api_key);

    printf("[gemini-live test] Connecting to %s%s\n", GEMINI_WS_HOST, GEMINI_WS_PATH);

    esp_websocket_client_config_t ws_cfg = {
        .host                   = GEMINI_WS_HOST,
        .path                   = ws_path,
        .port                   = 443,
        .transport              = WEBSOCKET_TRANSPORT_OVER_SSL,
        .buffer_size            = 4096,
        .task_stack             = 8192,
        .task_prio              = 5,
        .network_timeout_ms     = 10000,
        .reconnect_timeout_ms   = 5000,
        .disable_auto_reconnect = true,
        .crt_bundle_attach      = esp_crt_bundle_attach,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        printf("[gemini-live test] FAIL: WS init failed\n");
        return ESP_FAIL;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, gl_ws_event_handler, NULL);
    esp_err_t ws_start = esp_websocket_client_start(client);
    if (ws_start != ESP_OK) {
        printf("[gemini-live test] FAIL: WS start failed: %s\n", esp_err_to_name(ws_start));
        esp_websocket_client_destroy(client);
        return ESP_FAIL;
    }

    /* Wait for connect */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (!s_gl.ws_connected && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_gl.ws_connected) {
        printf("[gemini-live test] FAIL: WS connect timeout\n");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        s_gl.ws_client = NULL;
        return ESP_FAIL;
    }

    s_gl.ws_client = client;
    printf("[gemini-live test] Connected. Sending setup (model=%s)\n", GEMINI_LIVE_MODEL);

    xEventGroupClearBits(s_gl.ev, GL_BIT_SETUP_OK);
    if (!gl_send_setup()) {
        printf("[gemini-live test] FAIL: send_setup failed\n");
        goto test_cleanup;
    }

    /* Wait for setupComplete — dispatched via task notification then our event bit */
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK) && xTaskGetTickCount() < deadline) {
        /* Poll rx_buf for incoming frames (no session task running) */
        if (s_gl.rx_buf && s_gl.rx_buf[0]) {
            gl_dispatch_frame(s_gl.rx_buf);
            s_gl.rx_buf[0] = '\0';
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK)) {
        printf("[gemini-live test] FAIL: setupComplete timeout\n");
        goto test_cleanup;
    }

    printf("[gemini-live test] PASS: setupComplete received — WSS+TLS+auth OK\n");
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    s_gl.ws_client   = NULL;
    s_gl.ws_connected = false;
    s_gl.state       = GL_STATE_IDLE;
    return ESP_OK;

test_cleanup:
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    s_gl.ws_client   = NULL;
    s_gl.ws_connected = false;
    s_gl.state       = GL_STATE_IDLE;
    return ESP_FAIL;
}

/* ---- Capability descriptor ----------------------------------------------- */

static esp_err_t gl_cap_init(void)
{
    /* Boot path is single-threaded — materialise the lifecycle mutex before
     * any httpd/touch caller can race gateway start/stop. */
    if (!s_gl_lifecycle_mutex) {
        s_gl_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_gl_lifecycle_mutex_buf);
    }
    s_gl.ev       = xEventGroupCreate();
    s_gl.ws_mutex = xSemaphoreCreateMutex();
    if (!s_gl.ev || !s_gl.ws_mutex) {
        return ESP_ERR_NO_MEM;
    }
    /* No WS cleanup can be in flight at init. */
    xEventGroupSetBits(s_gl.ev, GL_BIT_WS_CLEANED);
    if (!s_gl.cmd_queue) {
        s_gl.cmd_queue = xQueueCreate(GL_CMD_QUEUE_DEPTH, sizeof(gl_cmd_t));
        if (!s_gl.cmd_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    cmd_cap_gemini_live_register();
    return ESP_OK;
}

static esp_err_t gl_cap_start(void)
{
    /* Do not auto-start tasks at boot — user must call cap_gemini_live_start()
     * or `gemini-live --start`. Touch polling would race the emote system. */
    return ESP_OK;
}

static esp_err_t gl_cap_stop(void)
{
    return gl_gateway_stop();
}

static esp_err_t gl_cap_execute(const char *input_json,
                                const claw_cap_call_context_t *ctx,
                                char *output, size_t output_size)
{
    (void)ctx;
    /* The CLI (cmd_cap_gemini_live.c) handles commands directly.
     * This execute path is a fallback for LLM-initiated calls (not exposed). */
    snprintf(output, output_size, "{\"ok\":false,\"error\":\"use gemini-live CLI\"}");
    return ESP_OK;
}

static const claw_cap_descriptor_t s_gl_descriptors[] = {
    {
        .id           = "gemini_live",
        .name         = "Gemini Live",
        .family       = "voice",
        .description  = "Toggle-on/off voice conversation via Gemini Live API",
        .kind         = CLAW_CAP_KIND_HYBRID,
        .cap_flags    = CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = NULL,
        .init         = gl_cap_init,
        .start        = gl_cap_start,
        .stop         = gl_cap_stop,
        .execute      = gl_cap_execute,
    },
};

static const claw_cap_group_t s_gl_group = {
    .group_id         = "cap_gemini_live",
    .plugin_name      = "cap_gemini_live",
    .version          = "1.0.0",
    .descriptors      = s_gl_descriptors,
    .descriptor_count = sizeof(s_gl_descriptors) / sizeof(s_gl_descriptors[0]),
};

esp_err_t cap_gemini_live_register_group(void)
{
    if (claw_cap_group_exists(s_gl_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_gl_group);
}
