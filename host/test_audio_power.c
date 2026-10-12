/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include "jr_dsp/audio_power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char trace[32];
    size_t used;
    char fail;
    unsigned reads, waits, wait_ms;
    bool allowed, mute_during_read, read_fails;
} fake_t;

static bool event(fake_t *f, char c)
{
    f->trace[f->used++] = c;
    f->trace[f->used] = 0;
    return f->fail != c;
}
static bool open_adc(void *p) { return event(p, 'A'); }
static bool close_adc(void *p) { return event(p, 'a'); }
static bool open_dac(void *p) { return event(p, 'D'); }
static bool close_dac(void *p) { return event(p, 'd'); }
static void pa(void *p, bool on) { (void)event(p, on ? 'P' : 'p'); }
static jr_audio_power_ops_t ops(fake_t *f)
{
    return (jr_audio_power_ops_t){f, open_adc, close_adc, open_dac, close_dac, pa};
}
static bool read_hw(void *p)
{
    fake_t *f = p;
    ++f->reads;
    if (f->mute_during_read) f->allowed = false;
    return !f->read_fails;
}
static void wait_frame(void *p, uint32_t ms)
{
    fake_t *f = p;
    ++f->waits;
    f->wait_ms = ms;
}
static bool allowed(void *p) { return ((fake_t *)p)->allowed; }
static jr_audio_capture_ops_t capture_ops(fake_t *f)
{
    return (jr_audio_capture_ops_t){f, read_hw, wait_frame, allowed};
}

static void test_muted_idle_closes_pair(void)
{
    fake_t f = {0}; jr_audio_power_ops_t o = ops(&f);
    jr_audio_power_t p = {true, true, true};
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, false, false, false, &o));
    TEST_ASSERT_EQUAL_STRING("adp", f.trace);
    TEST_ASSERT_FALSE(p.adc_open); TEST_ASSERT_FALSE(p.dac_open); TEST_ASSERT_FALSE(p.pa_on);
}
static void test_privacy_keeps_playback_until_tail_drains(void)
{
    fake_t f = {0}; jr_audio_power_ops_t o = ops(&f);
    jr_audio_power_t p = {true, true, true};
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, false, true, false, &o));
    TEST_ASSERT_EQUAL_STRING("a", f.trace);
    TEST_ASSERT_FALSE(p.adc_open); TEST_ASSERT_TRUE(p.dac_open); TEST_ASSERT_TRUE(p.pa_on);
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, false, false, false, &o));
    TEST_ASSERT_EQUAL_STRING("adp", f.trace);
}
static void test_unmuted_rest_keeps_clock_peer_without_pa(void)
{
    fake_t f = {0}; jr_audio_power_ops_t o = ops(&f); jr_audio_power_t p = {0};
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, true, false, false, &o));
    TEST_ASSERT_EQUAL_STRING("ADp", f.trace);
    TEST_ASSERT_TRUE(p.adc_open); TEST_ASSERT_TRUE(p.dac_open); TEST_ASSERT_FALSE(p.pa_on);
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, true, false, false, &o));
    TEST_ASSERT_EQUAL_STRING("ADp", f.trace);
}
static void test_muted_playback_opens_only_dac(void)
{
    fake_t f = {0}; jr_audio_power_ops_t o = ops(&f); jr_audio_power_t p = {0};
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, false, true, false, &o));
    TEST_ASSERT_EQUAL_STRING("DP", f.trace);
    TEST_ASSERT_FALSE(p.adc_open); TEST_ASSERT_TRUE(p.dac_open); TEST_ASSERT_TRUE(p.pa_on);
}
static void test_fast_kill_latch_reasserted_on_playback(void)
{
    fake_t f = {0}; jr_audio_power_ops_t o = ops(&f);
    jr_audio_power_t p = {false, true, false}; /* synchronized electrical kill */
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, false, true, false, &o));
    TEST_ASSERT_EQUAL_STRING("P", f.trace); TEST_ASSERT_TRUE(p.pa_on);
}
static void test_sleep_forces_pair_closed(void)
{
    fake_t f = {0}; jr_audio_power_ops_t o = ops(&f); jr_audio_power_t p = {true,true,true};
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, true, true, true, &o));
    TEST_ASSERT_EQUAL_STRING("adp", f.trace);
    TEST_ASSERT_FALSE(p.adc_open); TEST_ASSERT_FALSE(p.dac_open); TEST_ASSERT_FALSE(p.pa_on);
}
static void test_failed_close_is_truthful_and_retried(void)
{
    fake_t f = {.fail='a'}; jr_audio_power_ops_t o = ops(&f); jr_audio_power_t p = {true,true,true};
    TEST_ASSERT_FALSE(jr_audio_power_reconcile(&p, false, false, false, &o));
    TEST_ASSERT_TRUE(p.adc_open); TEST_ASSERT_FALSE(p.dac_open);
    f.fail=0;
    TEST_ASSERT_TRUE(jr_audio_power_reconcile(&p, false, false, false, &o));
    TEST_ASSERT_FALSE(p.adc_open); TEST_ASSERT_EQUAL_STRING("adpa", f.trace);
}
static void test_failed_dac_open_never_asserts_pa(void)
{
    fake_t f = {.fail='D'}; jr_audio_power_ops_t o = ops(&f); jr_audio_power_t p = {0};
    TEST_ASSERT_FALSE(jr_audio_power_reconcile(&p, false, true, false, &o));
    TEST_ASSERT_EQUAL_STRING("D", f.trace); TEST_ASSERT_FALSE(p.pa_on);
}
static void test_muted_capture_paces_512_zeros_without_hardware(void)
{
    fake_t f = {0}; jr_audio_capture_ops_t o = capture_ops(&f); int16_t pcm[512];
    memset(pcm, 0x7f, sizeof pcm);
    TEST_ASSERT_EQUAL(JR_AUDIO_CAPTURE_SILENT,jr_audio_power_capture(false,true,pcm,512,&o));
    for (size_t i=0;i<512;++i) TEST_ASSERT_EQUAL_INT16(0,pcm[i]);
    TEST_ASSERT_EQUAL_UINT(0,f.reads); TEST_ASSERT_EQUAL_UINT(1,f.waits);
    TEST_ASSERT_EQUAL_UINT(32,f.wait_ms);
}
static void test_live_capture_uses_hardware(void)
{
    fake_t f = {.allowed=true}; jr_audio_capture_ops_t o = capture_ops(&f); int16_t pcm[512];
    TEST_ASSERT_EQUAL(JR_AUDIO_CAPTURE_LIVE,jr_audio_power_capture(true,true,pcm,512,&o));
    TEST_ASSERT_EQUAL_UINT(1,f.reads); TEST_ASSERT_EQUAL_UINT(0,f.waits);
    TEST_ASSERT_EQUAL(JR_AUDIO_CAPTURE_NONE,jr_audio_power_capture(true,false,pcm,512,&o));
    TEST_ASSERT_EQUAL_UINT(1,f.reads);
}
static void test_privacy_during_read_discards_before_publication(void)
{
    fake_t f = {.allowed=true,.mute_during_read=true}; jr_audio_capture_ops_t o=capture_ops(&f);
    int16_t pcm[512]; memset(pcm,0x7f,sizeof pcm);
    TEST_ASSERT_EQUAL(JR_AUDIO_CAPTURE_SILENT,jr_audio_power_capture(true,true,pcm,512,&o));
    for(size_t i=0;i<512;++i) TEST_ASSERT_EQUAL_INT16(0,pcm[i]);
    TEST_ASSERT_EQUAL_UINT(1,f.reads); TEST_ASSERT_EQUAL_UINT(0,f.waits);
}

typedef struct {
    jr_audio_uplink_batch_t state;
    int16_t pcm[1024], sent[1024];
    size_t sent_samples;
    unsigned sends;
} fake_uplink_t;

/* The existing owner appends one 512-sample frame and sends at two frames or
 * speech end. The production guard owns invalidation, not this fake sender. */
static void uplink_frame(fake_uplink_t *u, int16_t sample,
                         bool upload_enabled, bool mic_enabled, bool flush)
{
    jr_audio_uplink_batch_guard(&u->state, u->pcm, 1024U,
                                upload_enabled, mic_enabled);
    if (!upload_enabled) return;
    for (size_t i = 0; i < 512U; ++i)
        u->pcm[u->state.filled + i] = mic_enabled ? sample : 0;
    u->state.filled += 512U;
    if (u->state.filled == 1024U || flush) {
        memcpy(u->sent, u->pcm, u->state.filled * sizeof(*u->pcm));
        u->sent_samples = u->state.filled;
        ++u->sends;
        jr_audio_uplink_batch_reset(&u->state, u->pcm, 1024U);
    }
}
static void test_listening_half_cannot_reach_muted_text_upload(void)
{
    fake_uplink_t u = {0};
    uplink_frame(&u, 1234, true, true, false);
    TEST_ASSERT_EQUAL_UINT(512U, u.state.filled);
    /* DISARM / privacy: no source read happens during these owner ticks. */
    jr_audio_uplink_batch_guard(&u.state, u.pcm, 1024U, false, false);
    TEST_ASSERT_EQUAL_UINT(0U, u.state.filled);
    for (size_t i = 0; i < 1024U; ++i) TEST_ASSERT_EQUAL_INT16(0, u.pcm[i]);
    uplink_frame(&u, 0, true, false, false); /* muted text opens capture */
    TEST_ASSERT_EQUAL_UINT(0U, u.sends);
    uplink_frame(&u, 0, true, false, false);
    TEST_ASSERT_EQUAL_UINT(1U, u.sends);
    TEST_ASSERT_EQUAL_UINT(1024U, u.sent_samples);
    for (size_t i = 0; i < 1024U; ++i) TEST_ASSERT_EQUAL_INT16(0, u.sent[i]);
}
static void test_batch_privacy_edge_during_capture_drops_live_half(void)
{
    fake_uplink_t u = {0};
    uplink_frame(&u, 1234, true, true, false);
    /* The current read is now silent, without an intervening idle tick. */
    uplink_frame(&u, 0, true, false, false);
    TEST_ASSERT_EQUAL_UINT(0U, u.sends);
    TEST_ASSERT_EQUAL_UINT(512U, u.state.filled);
    uplink_frame(&u, 0, true, false, false);
    TEST_ASSERT_EQUAL_UINT(1U, u.sends);
    for (size_t i = 0; i < 1024U; ++i) TEST_ASSERT_EQUAL_INT16(0, u.sent[i]);
    /* A privacy edge after batching but before send discards the whole batch. */
    u.state.filled = 1024U; u.state.microphone_enabled = true;
    memset(u.pcm, 0x7f, sizeof u.pcm);
    jr_audio_uplink_batch_guard(&u.state, u.pcm, 1024U, true, false);
    TEST_ASSERT_EQUAL_UINT(0U, u.state.filled);
}
static void test_batch_capture_and_transport_gaps_reset_partial(void)
{
    fake_uplink_t u = {0};
    uplink_frame(&u, 111, true, true, false);
    jr_audio_uplink_batch_guard(&u.state, u.pcm, 1024U, false, true);
    TEST_ASSERT_EQUAL_UINT(0U, u.state.filled);
    uplink_frame(&u, 222, true, true, false);
    TEST_ASSERT_EQUAL_UINT(0U, u.sends);
    uplink_frame(&u, 333, true, true, false);
    TEST_ASSERT_EQUAL_UINT(1U, u.sends);
    for (size_t i = 0; i < 512U; ++i) {
        TEST_ASSERT_EQUAL_INT16(222, u.sent[i]);
        TEST_ASSERT_EQUAL_INT16(333, u.sent[512U + i]);
    }
}
static void test_batch_retains_two_frame_cadence_and_speech_end_flush(void)
{
    fake_uplink_t u = {0};
    uplink_frame(&u, 111, true, true, false);
    TEST_ASSERT_EQUAL_UINT(0U, u.sends);
    uplink_frame(&u, 222, true, true, false);
    TEST_ASSERT_EQUAL_UINT(1U, u.sends);
    TEST_ASSERT_EQUAL_UINT(1024U, u.sent_samples);
    uplink_frame(&u, 333, true, true, true);
    TEST_ASSERT_EQUAL_UINT(2U, u.sends);
    TEST_ASSERT_EQUAL_UINT(512U, u.sent_samples);
    for (size_t i = 0; i < 512U; ++i) TEST_ASSERT_EQUAL_INT16(333, u.sent[i]);
    /* A physical unmute also starts a new batch, without mixing silence. */
    uplink_frame(&u, 0, true, false, false);
    uplink_frame(&u, 444, true, true, false);
    TEST_ASSERT_EQUAL_UINT(2U, u.sends);
    TEST_ASSERT_EQUAL_UINT(512U, u.state.filled);
}
static void test_privacy_history_clear_preserves_absolute_cursor(void)
{
    size_t head = 512U, count = 512U;
    uint64_t total = UINT64_C(4294967808);
    const uint64_t held_cursor = total;
    jr_audio_tap_history_clear(&head, &count, &total, false);
    TEST_ASSERT_EQUAL_UINT(0U, head); TEST_ASSERT_EQUAL_UINT(0U, count);
    TEST_ASSERT_EQUAL_UINT64(held_cursor, total);
    /* The surviving lease resumes from its previous absolute cursor. */
    total += 512U; count = 512U;
    TEST_ASSERT_TRUE(held_cursor <= total);
    TEST_ASSERT_EQUAL_UINT64(held_cursor, total - count);
    jr_audio_tap_history_clear(&head, &count, &total, true);
    TEST_ASSERT_EQUAL_UINT64(0U, total);
}
typedef struct {
    bool privacy, flip, microphone;
    bool mute_after_copy, flip_after_copy, close_after_copy, mute_before_send;
    unsigned copies, sends;
} fake_export_t;
static void copied_mic_response(fake_export_t *f)
{
    ++f->copies;
    if (f->mute_after_copy) f->privacy = true;
    if (f->flip_after_copy) f->flip = true;
    if (f->close_after_copy) f->microphone = false;
    if (!jr_audio_microphone_export_allowed(f->privacy, f->flip, f->microphone))
        return;
    /* Response headers/codec gain restoration may intervene before send. */
    if (f->mute_before_send) f->privacy = true;
    if (!jr_audio_microphone_export_allowed(f->privacy, f->flip, f->microphone))
        return;
    ++f->sends;
}
static void test_copied_companion_pcm_obeys_privacy_before_send(void)
{
    fake_export_t live = {.microphone = true};
    copied_mic_response(&live);
    TEST_ASSERT_EQUAL_UINT(1U, live.sends);
    fake_export_t edges[] = {
        {.microphone = true, .mute_after_copy = true},
        {.microphone = true, .flip_after_copy = true},
        {.microphone = true, .close_after_copy = true},
        {.microphone = true, .mute_before_send = true},
    };
    for (size_t i = 0; i < sizeof edges / sizeof edges[0]; ++i) {
        copied_mic_response(&edges[i]);
        TEST_ASSERT_EQUAL_UINT(1U, edges[i].copies);
        TEST_ASSERT_EQUAL_UINT(0U, edges[i].sends);
    }
}

/* Narrow production wiring assertions supplement the executing callback
 * controller above: removing the real call/gate must fail this same suite. */
static char *source_section(const char *path,const char *begin,const char *end)
{
    char full[1024]; snprintf(full,sizeof full,"%s/%s",JR_TEST_REPO_ROOT,path);
    FILE *fp=fopen(full,"rb"); TEST_ASSERT_NOT_NULL(fp);
    fseek(fp,0,SEEK_END); long n=ftell(fp); rewind(fp);
    char *all=malloc((size_t)n+1U); TEST_ASSERT_NOT_NULL(all);
    TEST_ASSERT_EQUAL_UINT((unsigned)n,(unsigned)fread(all,1,(size_t)n,fp)); fclose(fp);all[n]=0;
    char *a=strstr(all,begin);TEST_ASSERT_NOT_NULL(a);
    char *b=strstr(a+strlen(begin),end);TEST_ASSERT_NOT_NULL(b);*b=0;
    char *section=malloc(strlen(a)+1U);TEST_ASSERT_NOT_NULL(section);strcpy(section,a);free(all);
    return section;
}
static void test_production_source_boundary_and_pa_latch(void)
{
    char *s=source_section("components/jr_audio/src/jr_audio.c","static int src_read(","float jr_audio_clean_rms");
    char *gate=strstr(s,"jr_audio_power_capture(");char *dsp=strstr(s,"downsample_24to16_4lane(");
    TEST_ASSERT_NOT_NULL(gate);TEST_ASSERT_NOT_NULL(dsp);TEST_ASSERT_TRUE(gate<dsp);
    TEST_ASSERT_NOT_NULL(strstr(s,"capture != JR_AUDIO_CAPTURE_LIVE"));
    TEST_ASSERT_NOT_NULL(strstr(s,"xSemaphoreTake(s_adc_guard"));free(s);
    s=source_section("components/jr_audio/src/jr_audio.c","static bool audio_reconcile(","static bool capture_read_hardware");
    TEST_ASSERT_NOT_NULL(strstr(s,"s_power.pa_on = atomic_load(&s_pa_on)"));
    TEST_ASSERT_NOT_NULL(strstr(s,"jr_audio_power_reconcile(&s_power"));free(s);
    s=source_section("components/jr_audio/src/jr_audio.c","static void power_set_pa(","static const jr_audio_power_ops_t");
    TEST_ASSERT_NOT_NULL(strstr(s,"portENTER_CRITICAL(&s_pa_lock)"));
    TEST_ASSERT_NOT_NULL(strstr(s,"atomic_store(&s_pa_on, on)"));
    TEST_ASSERT_NOT_NULL(strstr(s,"!atomic_load(&s_muted)"));free(s);
}
static void test_production_main_privacy_and_dream_wiring(void)
{
    char *s=source_section("main/main.c","/* 4) mic uplink", "bool phase_allows_capture");
    char *set=strstr(s,"jr_audio_set_microphone_enabled("); TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_NOT_NULL(strstr(set,"!atomic_load(&s_voice_privacy_paused) && !s_flip_muted"));free(s);
    s=source_section("main/main.c","/* The source supplies paced zeros", "jr_capture_pause_on_capture(");
    char *zero=strstr(s,"if (mic_gated) memset(mic_frame");
    char *diag=strstr(s,"if (audio_diag_active || companion_listen)");
    TEST_ASSERT_NOT_NULL(zero);TEST_ASSERT_NOT_NULL(diag);TEST_ASSERT_TRUE(zero<diag);free(s);
    s=source_section("main/main.c","const bool dream_glass =", "const bool realtime_power");
    TEST_ASSERT_NOT_NULL(strstr(s,"atomic_load(&s_ota_active) || image_in_probation()"));
    free(s);
    s=source_section("main/main.c","static bool companion_update(", "static void companion_boot_seed(");
    TEST_ASSERT_NOT_NULL(strstr(s,"const bool dream = jr_mood_dream_glass(mood, busy, usb,"));
    TEST_ASSERT_NOT_NULL(strstr(s,"jr_display_rest_set(dream ? JR_DISPLAY_REST_DREAM : JR_DISPLAY_REST_NONE,"));free(s);
    s=source_section("main/main.c","if (peek || watch_space", "/* v4 barge fix");
    TEST_ASSERT_NOT_NULL(strstr(s,"|| dream_glass"));free(s);
}
static void test_production_http_export_and_bounded_health(void)
{
    char *s=source_section("main/http_routes.c","static esp_err_t audio_tap_wav_handler(","static const char *touch_kind_name");
    char *guard=strstr(s,"kind != JR_AUDIO_TAP_PLAYBACK &&");char *copy=strstr(s,"jr_audio_diag_copy(");
    TEST_ASSERT_NOT_NULL(guard);TEST_ASSERT_NOT_NULL(copy);TEST_ASSERT_TRUE(guard<copy);
    TEST_ASSERT_NOT_NULL(strstr(guard,"atomic_load(&s_voice_privacy_paused) || s_flip_muted"));
    TEST_ASSERT_NOT_NULL(strstr(copy,"403 Forbidden"));free(s);
    s=source_section("main/http_routes.c","jr_audio_power_stats_t audio_power", "static esp_err_t voice_control_handler");
    TEST_ASSERT_NOT_NULL(strstr(s,"char body[1536]"));
    TEST_ASSERT_NOT_NULL(strstr(s,"n < 0 || (size_t)n >= sizeof body"));
    TEST_ASSERT_NOT_NULL(strstr(s,"audio_power.hardware_reads"));free(s);
}
static void test_production_feeder_ownership_and_reopen_settings(void)
{
    char *s=source_section("components/jr_audio/src/jr_audio.c","static void feeder_task(","static int aligned16");
    char *claim=strstr(s,"atomic_store(&s_feeder_writing, true)");char *dequeue=strstr(s,"s_pb_head =");
    TEST_ASSERT_NOT_NULL(claim);TEST_ASSERT_NOT_NULL(dequeue);TEST_ASSERT_TRUE(claim<dequeue);
    char *tail=strstr(s,"atomic_store(&s_playback_tail_until_ms,");char *release=strstr(s,"atomic_store(&s_feeder_writing, false)");
    TEST_ASSERT_NOT_NULL(tail);TEST_ASSERT_NOT_NULL(release);TEST_ASSERT_TRUE(tail<release);free(s);
    s=source_section("components/jr_audio/src/jr_audio.c","static esp_err_t open_adc(","static bool power_open_adc");
    TEST_ASSERT_NOT_NULL(strstr(s,"fs.sample_rate     = GL_CAP_SAMPLE_RATE"));
    TEST_ASSERT_NOT_NULL(strstr(s,"apply_in_gains()"));
    TEST_ASSERT_NOT_NULL(strstr(s,"esp_codec_dev_set_out_vol(s_dac, atomic_load(&s_out_vol))"));free(s);
}
static void test_production_privacy_taps_and_real_codec_close(void)
{
    char *s=source_section("components/jr_audio/src/jr_audio.c","static void tap_write(","static bool diag_taps_init");
    char *gate=strstr(s,"kind != JR_AUDIO_TAP_PLAYBACK && !atomic_load(&s_microphone_enabled)");
    char *copy=strstr(s,"memcpy(tap->samples");
    TEST_ASSERT_NOT_NULL(gate);TEST_ASSERT_NOT_NULL(copy);TEST_ASSERT_TRUE(gate<copy);free(s);
    s=source_section("components/jr_audio/src/jr_audio.c","esp_err_t jr_audio_diag_copy(","/* ---- earcons:");
    TEST_ASSERT_NOT_NULL(strstr(s,"kind != JR_AUDIO_TAP_PLAYBACK && !atomic_load(&s_microphone_enabled)"));free(s);
    s=source_section("components/jr_audio/src/jr_audio.c","static bool power_close_adc(","static void power_set_pa");
    TEST_ASSERT_NOT_NULL(strstr(s,"esp_codec_dev_close(s_adc)"));
    TEST_ASSERT_NOT_NULL(strstr(s,"esp_codec_dev_close(s_dac)"));free(s);
    s=source_section("components/jr_audio/src/jr_audio.c","esp_err_t jr_audio_prepare_sleep(","void jr_audio_power_stats");
    TEST_ASSERT_NOT_NULL(strstr(s,"audio_reconcile(pdMS_TO_TICKS(200))"));
    TEST_ASSERT_NOT_NULL(strstr(s,"if (!was_muted) jr_audio_dac_unmute()"));free(s);
}
static void test_production_batch_cursor_and_companion_privacy_wiring(void)
{
    char *s = source_section("main/main.c", "const bool voice_uplink_capture =",
                            "/* Wake watch (Phase 5)");
    char *guard = strstr(s, "jr_audio_uplink_batch_guard(");
    char *read = strstr(s, "jr_audio_source_read(");
    char *send = strstr(s, "s_app.rvc.send_audio(");
    TEST_ASSERT_NOT_NULL(guard); TEST_ASSERT_NOT_NULL(read); TEST_ASSERT_NOT_NULL(send);
    TEST_ASSERT_TRUE(guard < read);
    TEST_ASSERT_NOT_NULL(strstr(s, "!audio_diag_active && !companion_listen"));
    char *before_send = strstr(read, "/* Recheck after VAD and buffer work");
    TEST_ASSERT_NOT_NULL(before_send);
    char *final_guard = strstr(before_send, "jr_audio_uplink_batch_guard(");
    TEST_ASSERT_NOT_NULL(final_guard); TEST_ASSERT_TRUE(final_guard < send);
    TEST_ASSERT_NOT_NULL(strstr(before_send, "s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN"));
    TEST_ASSERT_NOT_NULL(strstr(before_send, "!atomic_load(&s_voice_privacy_paused) && !s_flip_muted"));
    TEST_ASSERT_NOT_NULL(strstr(s, "if (upload_microphone_enabled)"));
    TEST_ASSERT_NOT_NULL(strstr(s, "memset(mic_batch + mic_batch_state.filled, 0")); free(s);
    s = source_section("main/main.c", "voice_control_request_t control =", "int requested_volume =");
    TEST_ASSERT_NOT_NULL(strstr(s, "if (control != VOICE_CONTROL_NONE)"));
    TEST_ASSERT_NOT_NULL(strstr(s, "jr_audio_uplink_batch_reset(")); free(s);
    s = source_section("components/jr_audio/src/jr_audio.c", "void jr_audio_set_microphone_enabled(",
                       "esp_err_t jr_audio_diag_get_info(");
    TEST_ASSERT_NOT_NULL(strstr(s, "tap_account_skips_locked(tap)"));
    TEST_ASSERT_NOT_NULL(strstr(s, "&tap->total, false)"));
    TEST_ASSERT_NULL(strstr(s, "tap->total = 0")); free(s);
    s = source_section("main/http_routes.c", "static bool companion_microphone_privacy_rejected(",
                       "static uint32_t companion_audio_next_seq(");
    TEST_ASSERT_NOT_NULL(strstr(s, "jr_audio_microphone_export_allowed("));
    TEST_ASSERT_NOT_NULL(strstr(s, "power.microphone_enabled"));
    char *copy = strstr(s, "esp_err_t err = jr_audio_diag_copy_since(");
    TEST_ASSERT_NOT_NULL(copy);
    char *post_copy = strstr(copy, "if (companion_microphone_privacy_rejected(req))");
    char *lease_check = strstr(copy, "const uint32_t response_now =");
    TEST_ASSERT_NOT_NULL(post_copy); TEST_ASSERT_NOT_NULL(lease_check);
    TEST_ASSERT_TRUE(post_copy < lease_check);
    char *binary = strstr(copy, "httpd_resp_set_type(req, \"application/octet-stream\")");
    TEST_ASSERT_NOT_NULL(binary);
    char *post_headers = strstr(binary, "if (companion_microphone_privacy_rejected(req))");
    char *pcm_send = strstr(binary, "err = httpd_resp_send(req, (const char *)pcm");
    TEST_ASSERT_NOT_NULL(post_headers); TEST_ASSERT_NOT_NULL(pcm_send);
    TEST_ASSERT_TRUE(post_headers < pcm_send); free(s);
}
void audio_power_tests_run(void)
{
    RUN_TEST(test_muted_idle_closes_pair);
    RUN_TEST(test_privacy_keeps_playback_until_tail_drains);
    RUN_TEST(test_unmuted_rest_keeps_clock_peer_without_pa);
    RUN_TEST(test_muted_playback_opens_only_dac);
    RUN_TEST(test_fast_kill_latch_reasserted_on_playback);
    RUN_TEST(test_sleep_forces_pair_closed);
    RUN_TEST(test_failed_close_is_truthful_and_retried);
    RUN_TEST(test_failed_dac_open_never_asserts_pa);
    RUN_TEST(test_muted_capture_paces_512_zeros_without_hardware);
    RUN_TEST(test_live_capture_uses_hardware);
    RUN_TEST(test_privacy_during_read_discards_before_publication);
    RUN_TEST(test_listening_half_cannot_reach_muted_text_upload);
    RUN_TEST(test_batch_privacy_edge_during_capture_drops_live_half);
    RUN_TEST(test_batch_capture_and_transport_gaps_reset_partial);
    RUN_TEST(test_batch_retains_two_frame_cadence_and_speech_end_flush);
    RUN_TEST(test_privacy_history_clear_preserves_absolute_cursor);
    RUN_TEST(test_copied_companion_pcm_obeys_privacy_before_send);
    RUN_TEST(test_production_source_boundary_and_pa_latch);
    RUN_TEST(test_production_main_privacy_and_dream_wiring);
    RUN_TEST(test_production_http_export_and_bounded_health);
    RUN_TEST(test_production_feeder_ownership_and_reopen_settings);
    RUN_TEST(test_production_privacy_taps_and_real_codec_close);
    RUN_TEST(test_production_batch_cursor_and_companion_privacy_wiring);
}
