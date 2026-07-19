/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_transport.c — Phase-1 Run-3 host suite for the L2 Gemini transport.
 *
 * Proves, on a laptop with NO ESP-IDF / NO socket / NO Gemini quota:
 *   - the SETUP/message BUILDERS (model pin, nested thinkingLevel, manual PTT,
 *     tools coexistence, and the NO-audioStreamEnd-in-manual guarantee);
 *   - the would-block-as-backpressure FRAMER (connection survives would-block /
 *     partial / soft-fail; bounded memory; drop-newest; a real close is death);
 *   - the inbound PARSER (server audio, turn signals, the nasty sequences:
 *     interrupted-without-generationComplete, toolCallCancellation, goAway
 *     Duration, sessionResumptionUpdate mid-stream);
 *   - the independent uplink/downlink liveness feed (split-brain detectable).
 *
 * Registered from test_core.c's main() via transport_tests_run() so the whole
 * suite is one ./build/jr_host_tests binary.
 */
#include "unity.h"
#include "jr_transport/gemini_live.h"
#include "jr_core/monitors.h"
#include "fake_ws_transport.h"
#include "fake_clock.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

/* ---- small helpers ---- */
static jr_gemini_config_t manual_cfg(void)
{
    jr_gemini_config_t c;
    memset(&c, 0, sizeof(c));
    c.vad_mode = JR_VAD_MANUAL_LOCAL_RMS;
    return c;
}

/* first event of a single-frame parse */
static jr_gemini_event_kind_t first_kind(jr_gemini_parse_t *p)
{
    return p->count ? p->events[0].kind : JR_GEV_UNKNOWN;
}

/* ===================================================================== *
 *  BUILDERS                                                             *
 * ===================================================================== */
static void test_setup_model_pinned(void)
{
    jr_gemini_config_t cfg = manual_cfg();
    char *j = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = cJSON_Parse(j);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *model = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "model");
    TEST_ASSERT_TRUE(cJSON_IsString(model));
    TEST_ASSERT_EQUAL_STRING(JR_GEMINI_MODEL_PRIMARY, model->valuestring);
    TEST_ASSERT_EQUAL_STRING("models/gemini-3.1-flash-live-preview", model->valuestring);
    cJSON_Delete(root);
    free(j);
}

/* thinkingLevel MUST nest under generationConfig.thinkingConfig, NOT flat. */
static void test_setup_thinkingLevel_nested_not_flat(void)
{
    jr_gemini_config_t cfg = manual_cfg();
    cfg.thinking_level = "minimal";
    char *j = jr_gemini_build_setup(&cfg);
    cJSON *root = cJSON_Parse(j);
    cJSON *gc = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "generationConfig");
    TEST_ASSERT_NOT_NULL(gc);
    cJSON *tcfg = cJSON_GetObjectItem(gc, "thinkingConfig");
    TEST_ASSERT_NOT_NULL(tcfg);                                   /* nested exists */
    cJSON *tl = cJSON_GetObjectItem(tcfg, "thinkingLevel");
    TEST_ASSERT_TRUE(cJSON_IsString(tl));
    TEST_ASSERT_EQUAL_STRING("minimal", tl->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(gc, "thinkingLevel"));   /* NOT flat */
    /* responseModalities == [AUDIO] */
    cJSON *rm = cJSON_GetObjectItem(gc, "responseModalities");
    TEST_ASSERT_TRUE(cJSON_IsArray(rm));
    TEST_ASSERT_EQUAL_STRING("AUDIO", cJSON_GetArrayItem(rm, 0)->valuestring);
    cJSON_Delete(root);
    free(j);
}

static void test_setup_manual_ptt_config(void)
{
    jr_gemini_config_t cfg = manual_cfg();
    char *j = jr_gemini_build_setup(&cfg);
    cJSON *root = cJSON_Parse(j);
    cJSON *ric = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "realtimeInputConfig");
    cJSON *aad = cJSON_GetObjectItem(ric, "automaticActivityDetection");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(aad, "disabled")));
    TEST_ASSERT_EQUAL_STRING("NO_INTERRUPTION",
                             cJSON_GetObjectItem(ric, "activityHandling")->valuestring);
    /* setup NEVER contains audioStreamEnd */
    TEST_ASSERT_NULL(strstr(j, "audioStreamEnd"));
    cJSON_Delete(root);
    free(j);
}

static void test_setup_auto_vad_config(void)
{
    jr_gemini_config_t cfg = manual_cfg();
    cfg.vad_mode = JR_VAD_SERVER;
    char *j = jr_gemini_build_setup(&cfg);
    cJSON *root = cJSON_Parse(j);
    cJSON *ric = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "realtimeInputConfig");
    cJSON *aad = cJSON_GetObjectItem(ric, "automaticActivityDetection");
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(aad, "disabled")));
    TEST_ASSERT_EQUAL_STRING("START_OF_ACTIVITY_INTERRUPTS",
                             cJSON_GetObjectItem(ric, "activityHandling")->valuestring);
    cJSON_Delete(root);
    free(j);
}

/* functionDeclarations + googleSearch COEXIST in tools. */
static void test_setup_tools_coexist(void)
{
    jr_gemini_fn_decl_t fns[] = {
        { "crypto_price", "price of a coin", "symbol", "coin symbol" },
    };
    jr_gemini_config_t cfg = manual_cfg();
    cfg.google_search = true;
    cfg.fns = fns;
    cfg.fn_count = 1;
    char *j = jr_gemini_build_setup(&cfg);
    cJSON *root = cJSON_Parse(j);
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "tools");
    TEST_ASSERT_TRUE(cJSON_IsArray(tools));
    int saw_gs = 0, saw_fd = 0;
    cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        if (cJSON_GetObjectItem(t, "googleSearch")) { saw_gs = 1; }
        if (cJSON_GetObjectItem(t, "functionDeclarations")) { saw_fd = 1; }
    }
    TEST_ASSERT_TRUE(saw_gs);
    TEST_ASSERT_TRUE(saw_fd);
    cJSON_Delete(root);
    free(j);
}

static void test_build_control_frames(void)
{
    char *as = jr_gemini_build_activity_start();
    char *ae = jr_gemini_build_activity_end();
    char *se = jr_gemini_build_audio_stream_end();
    TEST_ASSERT_EQUAL_STRING("{\"realtimeInput\":{\"activityStart\":{}}}", as);
    TEST_ASSERT_EQUAL_STRING("{\"realtimeInput\":{\"activityEnd\":{}}}", ae);
    TEST_ASSERT_NOT_NULL(strstr(se, "audioStreamEnd"));
    free(as); free(ae); free(se);
}

/* Build an uplink audio chunk, re-embed its base64 in a downlink inlineData
 * frame, parse it back: proves PCM16 encode + decode round-trips and the parser
 * decodes server audio + the mimeType rate. */
static void test_build_audio_chunk_roundtrip(void)
{
    jr_pcm_t in[16];
    for (int i = 0; i < 16; ++i) { in[i] = (jr_pcm_t)((i * 4099) - 20000); }
    char *chunk = jr_gemini_build_audio_chunk(in, 16, JR_GEMINI_TX_RATE);
    TEST_ASSERT_NOT_NULL(chunk);
    cJSON *root = cJSON_Parse(chunk);
    cJSON *audio = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "realtimeInput"), "audio");
    TEST_ASSERT_NOT_NULL(strstr(cJSON_GetObjectItem(audio, "mimeType")->valuestring, "rate=16000"));
    const char *b64 = cJSON_GetObjectItem(audio, "data")->valuestring;

    char frame[512];
    snprintf(frame, sizeof(frame),
             "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":"
             "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"%s\"}}]}}}", b64);
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(frame, &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, first_kind(&p));
    TEST_ASSERT_EQUAL_size_t(16, p.events[0].pcm_len);
    TEST_ASSERT_EQUAL_UINT32(24000, p.events[0].sample_rate);
    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_INT16(in[i], p.events[0].pcm[i]);
    }
    jr_gemini_parse_free(&p);
    cJSON_Delete(root);
    free(chunk);
}

static void test_build_tool_response_preserves_original_id(void)
{
    char *j = jr_gemini_build_tool_response(
        "call_original_42", "current_time", "{\"result\":\"12:34\"}");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = cJSON_Parse(j);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *responses = cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "toolResponse"), "functionResponses");
    cJSON *fr = cJSON_GetArrayItem(responses, 0);
    TEST_ASSERT_EQUAL_STRING("call_original_42",
                             cJSON_GetObjectItem(fr, "id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("current_time",
                             cJSON_GetObjectItem(fr, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("12:34",
        cJSON_GetObjectItem(cJSON_GetObjectItem(fr, "response"), "result")->valuestring);
    cJSON_Delete(root);
    free(j);

    j = jr_gemini_build_tool_response("id", "tool", "not-json");
    root = cJSON_Parse(j);
    fr = cJSON_GetArrayItem(cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "toolResponse"), "functionResponses"), 0);
    TEST_ASSERT_EQUAL_STRING("not-json",
        cJSON_GetObjectItem(cJSON_GetObjectItem(fr, "response"), "error")->valuestring);
    cJSON_Delete(root);
    free(j);
}

/* ===================================================================== *
 *  FRAMER — would-block as backpressure (the load-bearing win)          *
 * ===================================================================== */

/* A socket that ALWAYS would-blocks: EVERY send is backpressure, the connection
 * NEVER tears down, memory stays bounded, and drop-newest kicks in past cap. */
static void test_framer_wouldblock_never_aborts(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_WOULD_BLOCK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    for (int i = 0; i < 200; ++i) {
        jr_err_t r = jr_gemini_send_frame(&c, "audioframe", 10);
        TEST_ASSERT_EQUAL_INT(JR_ERR_WOULD_BLOCK, r);        /* backpressure */
        TEST_ASSERT_TRUE(jr_err_is_backpressure(r));
        TEST_ASSERT_TRUE(jr_gemini_txq_depth(&c) <= JR_GEMINI_TXQ_DEPTH); /* bounded */
    }
    TEST_ASSERT_EQUAL_size_t(JR_GEMINI_TXQ_DEPTH, jr_gemini_txq_depth(&c));
    TEST_ASSERT_TRUE(c.live.tx_drops > 0);                    /* drop-newest fired */
    TEST_ASSERT_EQUAL_UINT(0, f.close_calls);                /* NEVER aborted */
    TEST_ASSERT_EQUAL_INT(JR_WS_OPEN, f.state);              /* connection kept */
}

/* Bounded, drop-NEWEST, oldest-retained: overfill while blocked, then drain and
 * assert the first JR_GEMINI_TXQ_DEPTH frames survived in order, the newest were
 * dropped, and nothing crashed. */
static void test_framer_drop_newest_oldest_retained(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_WOULD_BLOCK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    const int N = JR_GEMINI_TXQ_DEPTH + 3;
    char tag[8];
    for (int i = 0; i < N; ++i) {
        snprintf(tag, sizeof(tag), "F%02d", i);              /* distinct 3-byte frames */
        jr_gemini_send_frame(&c, tag, 3);
    }
    TEST_ASSERT_EQUAL_size_t(JR_GEMINI_TXQ_DEPTH, jr_gemini_txq_depth(&c));
    TEST_ASSERT_EQUAL_UINT32(3, c.live.tx_drops);            /* the 3 newest dropped */

    f.send_mode = FWS_OK;                                    /* socket drains */
    TEST_ASSERT_EQUAL_INT(JR_OK, jr_gemini_flush(&c));
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_txq_depth(&c));
    /* oldest retained, in order; the dropped newest are absent */
    for (int i = 0; i < JR_GEMINI_TXQ_DEPTH; ++i) {
        snprintf(tag, sizeof(tag), "F%02d", i);
        TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, tag));
    }
    snprintf(tag, sizeof(tag), "F%02d", JR_GEMINI_TXQ_DEPTH);
    TEST_ASSERT_FALSE(fake_ws_sent_contains(&f, tag));       /* newest dropped */
}

/* A partial write buffers the remainder; a later flush completes the frame with
 * no loss and no teardown. */
static void test_framer_partial_then_flush_completes(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN;
    f.send_mode = FWS_PARTIAL; f.partial_bytes = 4;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    const char *frame = "HELLO-WORLD";                       /* 11 bytes */
    jr_err_t r = jr_gemini_send_frame(&c, frame, strlen(frame));
    TEST_ASSERT_EQUAL_INT(JR_ERR_WOULD_BLOCK, r);            /* remainder buffered */
    TEST_ASSERT_EQUAL_size_t(1, jr_gemini_txq_depth(&c));

    f.send_mode = FWS_OK;
    TEST_ASSERT_EQUAL_INT(JR_OK, jr_gemini_flush(&c));
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_txq_depth(&c));
    TEST_ASSERT_EQUAL_STRING("HELLO-WORLD", f.sent_log);     /* delivered whole */
    TEST_ASSERT_EQUAL_UINT(0, f.close_calls);
}

static void test_framer_ok_passthrough(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_OK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    TEST_ASSERT_EQUAL_INT(JR_OK, jr_gemini_send_frame(&c, "ping", 4));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, "ping"));
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_txq_depth(&c));
}

/* A real close is NOT backpressure — it routes to the death path. */
static void test_framer_real_close_is_death(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_CLOSED;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    jr_err_t r = jr_gemini_send_frame(&c, "x", 1);
    TEST_ASSERT_EQUAL_INT(JR_ERR_CLOSED, r);
    TEST_ASSERT_FALSE(jr_err_is_backpressure(r));            /* death, not backpressure */
    TEST_ASSERT_FALSE(c.live.socket_open);
}

/* A reconnect is a protocol boundary: no frame buffered for the dead socket may
 * precede Setup on the replacement socket. */
static void test_framer_session_reset_drops_stale_tx(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_WOULD_BLOCK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    TEST_ASSERT_EQUAL_INT(JR_ERR_WOULD_BLOCK,
                          jr_gemini_send_frame(&c, "stale-audio", 11));
    TEST_ASSERT_EQUAL_size_t(1, jr_gemini_txq_depth(&c));

    jr_gemini_reset_tx(&c);
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_txq_depth(&c));

    f.send_mode = FWS_OK;
    TEST_ASSERT_EQUAL_INT(JR_OK, jr_gemini_send_frame(&c, "setup", 5));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, "setup"));
    TEST_ASSERT_FALSE(fake_ws_sent_contains(&f, "stale-audio"));
}

/* ===================================================================== *
 *  RVC control guarantee — audioStreamEnd is IMPOSSIBLE in manual mode  *
 * ===================================================================== */
static void test_manual_never_emits_audioStreamEnd(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_OK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    jr_realtime_voice_client_t rvc = jr_gemini_client_as_rvc(&c);

    /* manual mode: audioStreamEnd is refused (no bytes sent) ... */
    TEST_ASSERT_EQUAL_INT(JR_OK, rvc.send_control(rvc.ctx, JR_RVC_CTRL_AUDIO_STREAM_END));
    TEST_ASSERT_FALSE(fake_ws_sent_contains(&f, "audioStreamEnd"));
    /* ... but activityEnd IS the manual turn-boundary signal */
    TEST_ASSERT_EQUAL_INT(JR_OK, rvc.send_control(rvc.ctx, JR_RVC_CTRL_ACTIVITY_END));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, "activityEnd"));

    /* auto-VAD mode DOES emit audioStreamEnd (and no-ops activityStart) */
    fake_ws_t g; fake_ws_init(&g); g.state = JR_WS_OPEN; g.send_mode = FWS_OK;
    jr_gemini_config_t acfg = manual_cfg(); acfg.vad_mode = JR_VAD_SERVER;
    jr_gemini_client_t ac;
    jr_gemini_client_init(&ac, fake_ws_make(&g), fake_clock_make(), &acfg);
    jr_realtime_voice_client_t arvc = jr_gemini_client_as_rvc(&ac);
    TEST_ASSERT_EQUAL_INT(JR_OK, arvc.send_control(arvc.ctx, JR_RVC_CTRL_AUDIO_STREAM_END));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&g, "audioStreamEnd"));
}

/* connect() opens the socket AND sends the setup frame (hidden behind the port). */
static void test_connect_sends_setup(void)
{
    fake_ws_t f; fake_ws_init(&f); f.send_mode = FWS_OK;
    jr_gemini_config_t cfg = manual_cfg();
    cfg.url = "wss://example/fake";
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    jr_realtime_voice_client_t rvc = jr_gemini_client_as_rvc(&c);
    TEST_ASSERT_EQUAL_INT(JR_OK, rvc.connect(rvc.ctx));
    TEST_ASSERT_EQUAL_INT(JR_WS_OPEN, f.state);
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, "\"setup\""));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, JR_GEMINI_MODEL_PRIMARY));
}

/* ===================================================================== *
 *  PARSER — server frame -> typed events                                *
 * ===================================================================== */
static void test_parse_setupComplete(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"setupComplete\":{}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_SETUP_COMPLETE, first_kind(&p));
    jr_gemini_parse_free(&p);
}

/* A single serverContent frame carrying audio AND turnComplete demuxes to two
 * ordered events. */
static void test_parse_audio_and_turnComplete(void)
{
    /* "AAAA" base64 = 3 zero bytes -> not int16-aligned; use "AAAAAAAA" = 6 zero
     * bytes -> 3 samples of 0. */
    const char *frame =
        "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":"
        "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"}}]},"
        "\"turnComplete\":true}}";
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(frame, &p));
    TEST_ASSERT_EQUAL_size_t(2, p.count);
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, p.events[0].kind);
    TEST_ASSERT_EQUAL_size_t(3, p.events[0].pcm_len);
    TEST_ASSERT_EQUAL_INT(JR_GEV_TURN_COMPLETE, p.events[1].kind);
    jr_gemini_parse_free(&p);
}

static void test_parse_mediaChunks_array_and_object(void)
{
    const char *array_frame =
        "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"mediaChunks\":["
        "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"},"
        "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"}]}]}}}";
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(array_frame, &p));
    TEST_ASSERT_EQUAL_size_t(2, p.count);
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, p.events[0].kind);
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, p.events[1].kind);
    jr_gemini_parse_free(&p);

    const char *object_frame =
        "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"mediaChunks\":"
        "{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"AAAAAAAA\"}}]}}}";
    TEST_ASSERT_TRUE(jr_gemini_parse(object_frame, &p));
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, p.events[0].kind);
    TEST_ASSERT_EQUAL_UINT32(16000, p.events[0].sample_rate);
    jr_gemini_parse_free(&p);
}

/* NASTY SEQUENCE: interrupted with NO generationComplete, then a bare
 * turnComplete — exactly the interrupted -> turnComplete path (generationComplete
 * is skipped on an interrupted turn). */
static void test_parse_interrupted_without_generationComplete(void)
{
    jr_gemini_parse_t a;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"serverContent\":{\"interrupted\":true}}", &a));
    TEST_ASSERT_EQUAL_size_t(1, a.count);
    TEST_ASSERT_EQUAL_INT(JR_GEV_INTERRUPTED, a.events[0].kind);
    for (size_t i = 0; i < a.count; ++i) {
        TEST_ASSERT_NOT_EQUAL(JR_GEV_GENERATION_COMPLETE, a.events[i].kind);
    }
    jr_gemini_parse_free(&a);

    jr_gemini_parse_t b;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"serverContent\":{\"turnComplete\":true}}", &b));
    TEST_ASSERT_EQUAL_INT(JR_GEV_TURN_COMPLETE, first_kind(&b));
    jr_gemini_parse_free(&b);
}

/* goAway.timeLeft is a protobuf Duration (string "60s", object {seconds}, or a
 * bare number) — decoded to whole seconds, never treated as a raw field. */
static void test_parse_goAway_duration(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"goAway\":{\"timeLeft\":\"60s\"}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_GO_AWAY, first_kind(&p));
    TEST_ASSERT_EQUAL_UINT32(60, p.events[0].go_away_seconds);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse("{\"goAway\":{\"timeLeft\":{\"seconds\":30}}}", &p));
    TEST_ASSERT_EQUAL_UINT32(30, p.events[0].go_away_seconds);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse("{\"goAway\":{\"timeLeft\":10}}", &p));
    TEST_ASSERT_EQUAL_UINT32(10, p.events[0].go_away_seconds);
    jr_gemini_parse_free(&p);
}

/* sessionResumptionUpdate mid-stream: a usable (resumable) handle yields a
 * nonzero token; a non-resumable update yields token 0 (only usable handles
 * are stored). Both are heartbeats. */
static void test_parse_sessionResumption_resumable_gate(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"sessionResumptionUpdate\":{\"newHandle\":\"H1\",\"resumable\":true}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_RESUMPTION_UPDATE, first_kind(&p));
    TEST_ASSERT_TRUE(p.events[0].resumable);
    TEST_ASSERT_NOT_EQUAL(0, p.events[0].resumption_token);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"sessionResumptionUpdate\":{\"newHandle\":\"\",\"resumable\":false}}", &p));
    TEST_ASSERT_FALSE(p.events[0].resumable);
    TEST_ASSERT_EQUAL_UINT32(0, p.events[0].resumption_token);   /* unusable => 0 */
    jr_gemini_parse_free(&p);
}

/* toolCall dispatch + toolCallCancellation: the SAME id string maps to the SAME
 * uint32 call_id, so a cancellation matches its call. */
static void test_parse_toolCall_and_cancellation(void)
{
    jr_gemini_parse_t call;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"get_weather\","
        "\"id\":\"call_42\",\"args\":{\"city\":\"Paris\"}}]}}", &call));
    TEST_ASSERT_EQUAL_INT(JR_GEV_TOOL_CALL, first_kind(&call));
    TEST_ASSERT_EQUAL_STRING("get_weather", call.events[0].tool_name);
    TEST_ASSERT_EQUAL_STRING("call_42", call.events[0].call_id_text);
    TEST_ASSERT_NOT_NULL(strstr(call.events[0].tool_args, "Paris"));
    uint32_t id = call.events[0].call_id;
    TEST_ASSERT_NOT_EQUAL(0, id);
    jr_gemini_parse_free(&call);

    jr_gemini_parse_t cancel;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCallCancellation\":{\"ids\":[\"call_42\"]}}", &cancel));
    TEST_ASSERT_EQUAL_INT(JR_GEV_TOOL_CANCEL, first_kind(&cancel));
    TEST_ASSERT_EQUAL_UINT32(id, cancel.events[0].call_id);      /* matches the call */
    TEST_ASSERT_EQUAL_STRING("call_42", cancel.events[0].call_id_text);
    jr_gemini_parse_free(&cancel);
}

/* error frames classify a kind; quota is detected by code AND by message. */
static void test_parse_error_quota_detection(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"error\":{\"code\":429,\"message\":\"Too many requests\"}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_ERROR, first_kind(&p));
    TEST_ASSERT_EQUAL_INT(JR_GEMINI_ERRK_QUOTA, p.events[0].error_kind);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"error\":{\"message\":\"RESOURCE_EXHAUSTED: quota\"}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEMINI_ERRK_QUOTA, p.events[0].error_kind);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"error\":{\"code\":503,\"message\":\"backend unavailable\"}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEMINI_ERRK_TRANSIENT, p.events[0].error_kind);
    jr_gemini_parse_free(&p);
}

/* An unknown top-level frame is a valid parse with one UNKNOWN event; invalid
 * JSON is a parse failure (never a crash). */
static void test_parse_unknown_and_invalid(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"somethingNew\":{}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_UNKNOWN, first_kind(&p));
    jr_gemini_parse_free(&p);

    jr_gemini_parse_t bad;
    TEST_ASSERT_FALSE(jr_gemini_parse("{not valid json", &bad));
    jr_gemini_parse_free(&bad);   /* safe on a failed parse */
}

/* ===================================================================== *
 *  PUMP + independent liveness (split-brain)                            *
 * ===================================================================== */

/* The pump: recv -> parse -> emit both the rich and the neutral callbacks. */
static int g_rich_audio, g_neutral_audio;
static void rich_sink(void *u, const jr_gemini_event_t *e)
{
    (void)u;
    if (e->kind == JR_GEV_AUDIO_CHUNK) { g_rich_audio++; }
}
static void neutral_sink(void *u, const jr_rvc_event_t *e)
{
    (void)u;
    if (e->kind == JR_RVC_EV_AUDIO_CHUNK) { g_neutral_audio++; }
}
static void test_pump_emits_rich_and_neutral(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN;
    fake_ws_push_inbox(&f,
        "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":"
        "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"}}]}}}");
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    g_rich_audio = g_neutral_audio = 0;
    jr_gemini_client_set_event_cb(&c, rich_sink, NULL);
    jr_realtime_voice_client_t rvc = jr_gemini_client_as_rvc(&c);
    rvc.set_event_cb(rvc.ctx, neutral_sink, NULL);

    TEST_ASSERT_TRUE(jr_gemini_pump_rx(&c));
    TEST_ASSERT_EQUAL_INT(1, g_rich_audio);
    TEST_ASSERT_EQUAL_INT(1, g_neutral_audio);
    TEST_ASSERT_FALSE(jr_gemini_pump_rx(&c));   /* inbox drained */
}

/* SPLIT-BRAIN: a dead uplink (soft-fails) while the downlink is alive
 * (heartbeats). The DeadUplinkMonitor fires at 25; the KeepaliveMonitor does NOT
 * (its clock is re-armed by every inbound frame) — the two liveness signals are
 * independent, so the split-brain is observable. */
static void test_split_brain_uplink_dead_downlink_alive(void)
{
    fake_clock_reset();
    jr_clock_t clk = fake_clock_make();
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_SOFT_FAIL;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), clk, &cfg);

    jr_keepalive_monitor_t ka; memset(&ka, 0, sizeof ka);
    jr_keepalive_arm(&ka, jr_clock_now_ms(&clk), JR_LIVE_DEADLINE_MS);
    jr_dead_uplink_monitor_t du; memset(&du, 0, sizeof du);
    jr_dead_uplink_arm(&du, JR_TX_FAIL_RESUME);
    jr_gemini_client_set_monitors(&c, &ka, &du);

    for (unsigned i = 0; i < JR_TX_FAIL_RESUME; ++i) {   /* 25 */
        fake_ws_push_inbox(&f,
            "{\"sessionResumptionUpdate\":{\"newHandle\":\"h\",\"resumable\":true}}");
        TEST_ASSERT_EQUAL_INT(JR_ERR_WOULD_BLOCK,               /* soft-fail != death */
                              jr_gemini_send_frame(&c, "mic", 3));
        jr_gemini_pump_rx(&c);                                  /* downlink heartbeat */
    }

    TEST_ASSERT_EQUAL_UINT32(25, c.live.consecutive_tx_failures);
    TEST_ASSERT_TRUE(jr_dead_uplink_poll(&du).fired);          /* uplink dead */
    TEST_ASSERT_FALSE(jr_keepalive_poll(&ka, jr_clock_now_ms(&clk)).fired); /* downlink alive */
    TEST_ASSERT_EQUAL_UINT(0, f.close_calls);                  /* never torn down here */
}

/* ---- registration (called from test_core.c main, inside UNITY_BEGIN/END) ---- */
void transport_tests_run(void)
{
    /* builders */
    RUN_TEST(test_setup_model_pinned);
    RUN_TEST(test_setup_thinkingLevel_nested_not_flat);
    RUN_TEST(test_setup_manual_ptt_config);
    RUN_TEST(test_setup_auto_vad_config);
    RUN_TEST(test_setup_tools_coexist);
    RUN_TEST(test_build_control_frames);
    RUN_TEST(test_build_audio_chunk_roundtrip);
    RUN_TEST(test_build_tool_response_preserves_original_id);
    /* framer */
    RUN_TEST(test_framer_wouldblock_never_aborts);
    RUN_TEST(test_framer_drop_newest_oldest_retained);
    RUN_TEST(test_framer_partial_then_flush_completes);
    RUN_TEST(test_framer_ok_passthrough);
    RUN_TEST(test_framer_real_close_is_death);
    RUN_TEST(test_framer_session_reset_drops_stale_tx);
    /* rvc control guarantee + connect */
    RUN_TEST(test_manual_never_emits_audioStreamEnd);
    RUN_TEST(test_connect_sends_setup);
    /* parser */
    RUN_TEST(test_parse_setupComplete);
    RUN_TEST(test_parse_audio_and_turnComplete);
    RUN_TEST(test_parse_mediaChunks_array_and_object);
    RUN_TEST(test_parse_interrupted_without_generationComplete);
    RUN_TEST(test_parse_goAway_duration);
    RUN_TEST(test_parse_sessionResumption_resumable_gate);
    RUN_TEST(test_parse_toolCall_and_cancellation);
    RUN_TEST(test_parse_error_quota_detection);
    RUN_TEST(test_parse_unknown_and_invalid);
    /* pump + liveness */
    RUN_TEST(test_pump_emits_rich_and_neutral);
    RUN_TEST(test_split_brain_uplink_dead_downlink_alive);
}
