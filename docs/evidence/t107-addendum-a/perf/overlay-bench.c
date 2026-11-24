/* Actual production overlay, with HUD in an opaque separate translation unit.
 * No LTO; every frame starts with fresh art and all final pixels are consumed. */
#define hud_overlay_frame bench_overlay_frame
#include "jr_display.c"
#undef hud_overlay_frame
#include "idf_stubs.inc"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint16_t frame[HUD_W * HUD_H];
static uint16_t art[HUD_W * HUD_H];
static uint16_t reference[HUD_W * HUD_H];
#ifndef COUNT_VISITS
static unsigned long long sum;
#endif
extern void hud_overlay_frame(uint16_t *, int, int, uint32_t, bool, const hud_env_t *);
static void (*overlay)(uint16_t *, int, int, uint32_t, bool, const hud_env_t *) = hud_overlay_frame;
void bench_overlay_frame(uint16_t *dst, int y, int n, uint32_t ms, bool swap,
                         const hud_env_t *env)
{
    overlay(dst, y, n, ms, swap, env);
}
static void fixture(int style, int batt, bool charging, bool swap, int mode, int k)
{
    memset(&s_display, 0, sizeof s_display);
    s_display.task = (TaskHandle_t)1;
    s_display.gfx = (gfx_handle_t)1;
    s_display.anim = (gfx_obj_t *)1;
    s_display.board.width = HUD_W;
    s_display.board.height = HUD_H;
    s_display.board.swap_color_bytes = swap;
    s_display.applied_face = s_display.shown_face = JR_FACE_MUTED;
    s_display.requested_word = (uint32_t)JR_FACE_MUTED << JR_DISPLAY_CMD_FACE_SHIFT;
    s_display.requested_amplitude = 0U;
    s_hud_enabled = 1U;
    s_hud_env_word = (uint32_t)batt | (charging ? 1U << 8 : 0U) | (1U << 9);
    if (mode != 0) s_hud_env_word |= ((uint32_t)(uint8_t)(k % 31 - 15) << 16) |
                                  ((uint32_t)(uint8_t)(k % 25 - 12) << 24);
    s_fake_us = (123456U + (uint32_t)k * 167U) * 1000LL;
    s_nav_word = JR_DISPLAY_SPACE_WATCH;
    jr_display_watch_style_set((jr_watch_style_t)style);
    s_space_from = s_space_to = JR_DISPLAY_SPACE_WATCH;
    s_space_on = true;
    s_space_prog = s_space_ease = s_space_veil = 256;
    s_shade_ease = s_detail_ease = 0;
    s_clock_ease = 256;
    s_caption_ease = s_ask_ease = s_canvas_ease = 0;
    s_caption_on = 0;
    s_canvas_until_ms = 0U;
    s_surface_active = false;
    s_choice_n = s_ask_shown_n = 0;
    s_commit_pct = 0U;
    s_ripple_start_ms = s_bloom_start_ms = 0U;
    s_ota_word = JR_DISPLAY_OTA_IDLE;
    s_power_word = (uint32_t)batt;
    s_panel_off = 0U;
    atomic_store(&s_caption_pinned, false);
    s_brightness_want = mode == 1 ? 2U : mode == 2 ? 100U : 14U;
    atomic_store(&s_brightness_have, s_brightness_want);
    jr_display_clock_set(true, k % 24, (k * 17) % 60, (k * 13) % 60);
    s_clock_shown_word = s_clock_word;
    const jr_face_t dial = watch_dial_face();
    if (dial != JR_FACE_COUNT) s_display.shown_face = dial;
    s_dial_frame = art;
    s_dial_frame_face = dial;
    s_dial_blit = dial != JR_FACE_COUNT;
    if (mode == 1) {
        s_space_from = JR_DISPLAY_SPACE_JARVIS;
        s_space_prog = s_space_ease = s_clock_ease = 128;
    } else if (mode == 2) {
        s_shade_ease = 128;
        s_nav_word |= (uint32_t)JR_DISPLAY_OVERLAY_SHADE << NAV_OVL_SHIFT;
    } else if (mode == 3) {
        static const char *const labels[] = {"ALLOW", "DENY"};
        jr_display_present_choices("Consent?", labels, 2);
        s_ask_ease = 128;
    } else if (mode == 4) {
        s_nav_word = JR_DISPLAY_SPACE_JARVIS;
        s_space_on = false;
        s_display.applied_face = s_display.shown_face = JR_FACE_RESTING;
        s_display.requested_word = (uint32_t)JR_FACE_RESTING << JR_DISPLAY_CMD_FACE_SHIFT;
        s_hud_env_word |= JR_DISPLAY_ENV_DREAM;
    } else if (mode == 5) {
        s_nav_word = JR_DISPLAY_SPACE_JARVIS;
        s_space_on = false;
        s_clock_ease = 0;
    }
    sp_compose();
    watch_compose();
}
static void paint(int style, int batt, bool charging, bool swap, int mode, int k)
{
    fixture(style, batt, charging, swap, mode, k);
    for (int i = 0; i < HUD_W * HUD_H; ++i) frame[i] = (uint16_t)(i * 73 + k * 31);
    for (int y = 0; y < HUD_H; y += 12) {
        const int end = y + 12 < HUD_H ? y + 12 : HUD_H;
        apply_dial(&s_display, 0, y, HUD_W, end, frame + (size_t)y * HUD_W);
        apply_canvas(&s_display, 0, y, HUD_W, end, frame + (size_t)y * HUD_W);
        apply_test_pattern(&s_display, 0, y, HUD_W, end, frame + (size_t)y * HUD_W);
        apply_hud_overlay(&s_display, 0, y, HUD_W, end, frame + (size_t)y * HUD_W);
        apply_shell_overlay(&s_display, 0, y, HUD_W, end, frame + (size_t)y * HUD_W);
        apply_surface_overlay(&s_display, 0, y, HUD_W, end, frame + (size_t)y * HUD_W);
    }
}
static unsigned long long hash(void)
{
    unsigned long long h = 1469598103934665603ull;
    for (int i = 0; i < HUD_W * HUD_H; ++i) h = (h ^ frame[i]) * 1099511628211ull;
    return h;
}
static double now(clockid_t clock)
{
    struct timespec t;
    clock_gettime(clock, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
#ifdef COUNT_VISITS
extern unsigned long long rim_visits(void);
extern void rim_visits_reset(void);
#endif
int main(int argc, char **argv)
{
    for (int i = 0; i < HUD_W * HUD_H; ++i) art[i] = (uint16_t)(i * 73 + 1203);
    if (argc > 2 && strcmp(argv[1], "compare") == 0) {
        void *library = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (!library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
        void (*baseline)(uint16_t *, int, int, uint32_t, bool, const hud_env_t *) =
            dlsym(library, "hud_overlay_frame");
        if (!baseline) return 2;
        unsigned cases = 0;
        for (int style = 0; style < 6; ++style)
        for (int sw = 0; sw < 2; ++sw)
        for (int batt = 0; batt <= 101; ++batt)
        for (int ch = 0; ch < 2; ++ch)
        for (int mode = 0; mode < 6; ++mode) {
            const int k = batt * 19 + mode * 43;
            overlay = baseline;
            paint(style, batt, ch, sw, mode, k);
            memcpy(reference, frame, sizeof frame);
            overlay = hud_overlay_frame;
            paint(style, batt, ch, sw, mode, k);
            if (memcmp(reference, frame, sizeof frame)) {
                printf("FAIL overlay style%d order%d batt%d charge%d mode%d\n", style, sw, batt, ch, mode);
                return 1;
            }
            ++cases;
        }
        printf("Exact full software overlay comparisons: %u frames\n", cases);
        cases = 0;
        static const int selected[] = {0, 1, 19, 20, 50, 69, 99, 100, 255};
        for (int sw = 0; sw < 2; ++sw)
        for (int pos = 0; pos < 3; ++pos)
        for (int b = 0; b < (int)(sizeof selected / sizeof selected[0]); ++b)
        for (int head = 0; head <= 256; ++head) {
            const hud_env_t env = { .face = HUD_FACE_IDLE, .batt_pct = selected[b],
                .charging = true, .privacy_muted = true, .ox = (pos - 1) * 15,
                .oy = (1 - pos) * 12 };
            const uint32_t ms = (uint32_t)head * 12U;
            for (int i = 0; i < HUD_W * HUD_H; ++i) reference[i] = frame[i] = art[i];
            for (int y = 0; y < HUD_H; y += 12) {
                const int n = y + 12 < HUD_H ? 12 : HUD_H - y;
                baseline(reference + (size_t)y * HUD_W, y, n, ms, sw, &env);
                hud_overlay_frame(frame + (size_t)y * HUD_W, y, n, ms, sw, &env);
            }
            if (memcmp(reference, frame, sizeof frame)) {
                printf("FAIL rim order%d offset%d batt%d head%d\n", sw, pos, selected[b], head);
                return 1;
            }
            ++cases;
        }
        printf("Exact charging/absent/offset comparisons: %u frames\n", cases);
        dlclose(library);
        return 0;
    }
#ifndef COUNT_VISITS
    if (argc > 2 && strcmp(argv[1], "paired") == 0) {
        void *library = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (!library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
        void (*baseline)(uint16_t *, int, int, uint32_t, bool, const hud_env_t *) =
            dlsym(library, "hud_overlay_frame");
        if (!baseline) return 2;
        for (int version = 0; version < 2; ++version) {
            overlay = version ? hud_overlay_frame : baseline;
            for (int k = 0; k < 100; ++k) paint(JR_WATCH_FUTURE, 69, false, false, 0, k);
        }
        for (int run = 0; run < 7; ++run) {
            double cpu[2] = {0}, wall[2] = {0};
            unsigned long long digest[2] = {0};
            for (int batch = 0; batch < 50; ++batch) {
                for (int leg = 0; leg < 4; ++leg) {
                    const int v = ((leg == 1 || leg == 2) ? 1 : 0) ^ (batch & 1);
                    overlay = v ? hud_overlay_frame : baseline;
                    const double start_cpu = now(CLOCK_PROCESS_CPUTIME_ID);
                    const double start_wall = now(CLOCK_MONOTONIC);
                    for (int k = batch * 100; k < (batch + 1) * 100; ++k) {
                        paint(JR_WATCH_FUTURE, 69, false, false, 0, k);
                        digest[v] += frame[(k * 431) % (HUD_W * HUD_H)];
                    }
                    cpu[v] += now(CLOCK_PROCESS_CPUTIME_ID) - start_cpu;
                    wall[v] += now(CLOCK_MONOTONIC) - start_wall;
                    digest[v] ^= hash();
                }
            }
            printf("paired%d frames10000 before_cpu%.9f after_cpu%.9f gain_pct%.5f before_wall%.9f after_wall%.9f digest%llu/%llu\n",
                run, cpu[0], cpu[1], 100.0 * (cpu[0] - cpu[1]) / cpu[0],
                wall[0], wall[1], digest[0], digest[1]);
            fflush(stdout);
            if (digest[0] != digest[1]) return 1;
        }
        dlclose(library);
        return 0;
    }
#endif
    if (argc > 1 && strcmp(argv[1], "pixels") == 0) {
        for (int style = 0; style < 6; ++style)
        for (int sw = 0; sw < 2; ++sw)
        for (int batt = 0; batt <= 101; ++batt)
        for (int ch = 0; ch < 2; ++ch)
        for (int mode = 0; mode < 6; ++mode) {
            paint(style, batt, ch, sw, mode, batt * 19 + mode * 43);
            printf("%d %d %d %d %d %016llx\n", style, sw, batt, ch, mode, hash());
        }
        return 0;
    }
#ifdef COUNT_VISITS
    rim_visits_reset();
    paint(JR_WATCH_FUTURE, 69, false, false, 0, 1);
    printf("FUTURE69 base+comet calls/frame=%llu hash=%016llx\n", rim_visits(), hash());
    if (argc > 1 && strcmp(argv[1], "bound") == 0) {
        if (rim_visits() > 400U) { puts("FAIL bounded FUTURE69 work exceeds 400 visits"); return 1; }
        puts("PASS bounded polar work <=400 visits");
        return 0;
    }
    for (int batt = 0; batt <= 100; batt += 10) {
        rim_visits_reset();
        paint(JR_WATCH_FUTURE, batt, true, false, 0, 231);
        printf("charging%d base+comet calls/frame=%llu\n", batt, rim_visits());
    }
#else
    const int frames = argc > 1 ? atoi(argv[1]) : 10000;
    for (int k = 0; k < 100; ++k) paint(JR_WATCH_FUTURE, 69, false, false, 0, k);
    for (int run = 0; run < 5; ++run) {
        const double wall = now(CLOCK_MONOTONIC), cpu = now(CLOCK_PROCESS_CPUTIME_ID);
        for (int k = 0; k < frames; ++k) {
            paint(JR_WATCH_FUTURE, 69, false, false, 0, k);
            sum += frame[(k * 431) % (HUD_W * HUD_H)];
        }
        const double elapsed_cpu = now(CLOCK_PROCESS_CPUTIME_ID) - cpu;
        const double elapsed_wall = now(CLOCK_MONOTONIC) - wall;
        sum ^= hash();
        printf("run%d frames%d cpu%.9f wall%.9f checksum%llu\n", run, frames,
               elapsed_cpu, elapsed_wall, sum);
    }
#endif
    return 0;
}
