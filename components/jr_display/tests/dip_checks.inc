/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * dip_checks.inc — the panel dip (perf knob bit 0), included by test_frame_identity.c.
 *
 * WHAT IS PROVEN. With JR_KNOB_PANEL_DIP the compositor draws the old screen and then
 * the new one at full strength and the panel's brightness is ramped by the display's
 * millisecond clock. Each case plays one transition through the production state
 * machine with the panel pumped between strips as panel_flush does (mp_frame with
 * mp_frame_ms), at a frame time that is not the render cadence, and holds it to:
 *
 *   - every composed frame is the settled old screen or the new screen in its entry
 *     motion, built here by the same machinery on knob 0 (no part-strength frame, no
 *     half-swapped one), old first, new after;
 *   - the dip falls to 0, holds, and rises to 256, monotonically each way, on the
 *     clock (several writes inside one slow frame) and at no more than one write per
 *     16 ms, each write the gamma of the dip times the brightness the mood asked for;
 *   - the dark ends exactly when the first whole frame of the new screen is flushed,
 *     or at the cap when it never is;
 *   - a transition cut in on, in each phase, never lights an unlit screen, never jumps
 *     to full brightness and ends at 256 with the sent value back where it was;
 *   - on knob 0 nothing of this runs: no write, no dip, the fade is still in the pixels.
 */
#define DP_MAX_FRAMES 100

typedef struct { const char *name; const char *scene; mp_op_t op; int arg; int total_ms; int cut; } dp_case_t;

static const dp_case_t DP_CASES[] = {
    { "slide weather > status",  "weather",     MP_NEXT,  0, JR_DISPLAY_SPACE_MS, TR_CUT_SLIDE },
    { "enter a drawn watch",     "home",        MP_NEXT,  0, JR_DISPLAY_SPACE_MS, TR_CUT_SLIDE },
    { "leave a baked dial",      "watch-diver", MP_PREV,  0, JR_DISPLAY_SPACE_MS, TR_CUT_SLIDE },
    { "style dial > dial",       "watch-diver", MP_STYLE, 1, SP_STYLE_MS,          TR_CUT_STYLE },
    { "style dial > drawn",      "watch-pilot", MP_STYLE, 1, SP_STYLE_MS,          TR_CUT_STYLE },
};
#define DP_NCASES ((int)(sizeof DP_CASES / sizeof DP_CASES[0]))

/* One played transition: what each frame was, and the log of the panel and the state it ended in. */
typedef struct {
    int n, frame_ms;
    int64_t t0_us;
    int64_t t_us[DP_MAX_FRAMES];
    uint32_t entry[DP_MAX_FRAMES];
    bool ready[DP_MAX_FRAMES];
    uint64_t hash[DP_MAX_FRAMES];
    int nwrites, nsamples;
    mp_write_t writes[MP_MAX_LOG];
    mp_sample_t samples[MP_MAX_LOG];
    int dip, phase, panel, have, shown;
    bool tail;
    int mode;
    unsigned dip_writes, hold_ms, calls;
} dp_run_t;

#define DP_T0_US 20000000LL

static void dp_apply(mp_op_t op, int arg)
{
    switch (op) {
    case MP_NEXT:  jr_display_nav_next(); break;
    case MP_PREV:  jr_display_nav_prev(); break;
    case MP_HOME:  jr_display_nav_home(); break;
    case MP_STYLE: jr_display_watch_style_step(arg < 0 ? -1 : 1); break;
    default: break;
    }
}

/* The scene settled on `knob` with the mood at `want` percent, its last frame at end_us. */
static void dp_start(const char *scene, uint32_t knob, int want, int64_t end_us)
{
    static uint16_t fb[466 * 466];
    fc_knob_extra = knob;
    mp_frame_ms = 0;
    mp_entry_override = -1;
    mp_unbound = false;
    fc_setup(find_scene(scene));
    s_space_serial_seen = (uint8_t)(s_nav_word >> NAV_SERIAL_SHIFT);   /* fc_setup resets the word, not the edge detector */
    s_fake_us = 0;
    s_fade_prev_ms = (uint32_t)((end_us - (int64_t)MP_PRE * 1000000LL / MP_FPS) / 1000);
    jr_display_set_brightness((uint8_t)want);
    atomic_store(&s_brightness_shown, want);
    atomic_store(&s_brightness_have, (uint8_t)want);
    s_stub_panel_brightness = (uint8_t)want;
    s_stub_brightness_calls = 0U;
    s_space_hold_ms = 0U;
    mp_nwrites = mp_nsamples = 0;
    for (int i = MP_PRE; i > 0; i--) {
        mp_frame(end_us - (int64_t)i * 1000000LL / MP_FPS, fb);
    }
}

/* Play the case's op now and `op2` when frame `interrupt` begins (-1 = never), `frame_ms` a frame. */
static void dp_transition(const dp_case_t *c, uint32_t knob, int want, int frame_ms, int interrupt,
                          mp_op_t op2, int arg2, bool unbound, dp_run_t *r)
{
    static uint16_t fb[466 * 466];
    dp_start(c->scene, knob, want, DP_T0_US);
    mp_frame_ms = frame_ms;
    mp_unbound = unbound;
    r->frame_ms = frame_ms;
    r->t0_us = DP_T0_US;
    r->n = DP_MAX_FRAMES * 40 / frame_ms;
    if (r->n > DP_MAX_FRAMES) r->n = DP_MAX_FRAMES;
    dp_apply(c->op, c->arg);
    for (int i = 0; i < r->n; i++) {
        if (i == interrupt) {
            dp_apply(op2, arg2);
        }
        r->t_us[i] = DP_T0_US + (int64_t)i * frame_ms * 1000;
        mp_frame(r->t_us[i], fb);
        r->entry[i] = mp_last_entry;
        r->ready[i] = mp_last_ready;
        r->hash[i] = fc_hash(fb);
    }
    mp_frame_ms = 0;
    mp_unbound = false;
    r->nwrites = mp_nwrites;
    r->nsamples = mp_nsamples;
    memcpy(r->writes, mp_writes, sizeof mp_writes);
    memcpy(r->samples, mp_samples, sizeof mp_samples);
    r->dip = s_dip;
    r->phase = (int)s_dip_phase;
    r->tail = s_dip_tail;
    r->mode = (int)s_dip_mode;
    r->panel = (int)s_stub_panel_brightness;
    r->have = (int)atomic_load(&s_brightness_have);
    r->shown = (int)atomic_load(&s_brightness_shown);
    r->dip_writes = (unsigned)s_dip_writes;
    r->hold_ms = (unsigned)s_dip_hold_ms;
    r->calls = s_stub_brightness_calls;
}

/* The same frames, settled, on knob 0: with 0, 1 or 2 ops applied and given time to settle
 * first; the entry clock and the rail's hold are the run's. */
static void dp_refs(const dp_case_t *c, int nops, mp_op_t op2, int arg2, const dp_run_t *r, uint64_t *out)
{
    static uint16_t fb[466 * 466];
    const int64_t settle = 4000000LL;
    const int64_t base = DP_T0_US - (nops > 0 ? settle : 0);
    dp_start(c->scene, 0u, 100, base);
    int64_t t = base;
    if (nops > 0) {
        dp_apply(c->op, c->arg);
        for (int k = 0; k < 40; k++) {
            t += 1000000LL / MP_FPS;
            mp_frame(t, fb);
        }
        if (nops > 1) {
            dp_apply(op2, arg2);
            for (int k = 0; k < 40; k++) {
                t += 1000000LL / MP_FPS;
                mp_frame(t, fb);
            }
        }
    }
    const bool rail = c->op != MP_STYLE;
    for (int i = 0; i < r->n; i++) {
        s_fade_prev_ms = (uint32_t)(r->t_us[i] / 1000) - 42u;
        s_space_hold_ms = rail ? (uint32_t)(DP_T0_US / 1000) + (uint32_t)JR_DISPLAY_SPACE_HOLD_MS : 0U;
        mp_entry_override = nops > 0 ? (int64_t)r->entry[i] : -1;
        mp_frame(r->t_us[i], fb);
        out[i] = fc_hash(fb);
    }
    mp_entry_override = -1;
}

static int dp_light_ref(int dip) { return (dip * dip) >> 8; }

/* The series every transition must show, whatever it is: the dip's samples, the writes. */
static int dp_series_check(const char *name, const dp_run_t *r, int want, int cut, int total_ms, bool expect_cap,
                           int *checks)
{
    int bad = 0;
    const int64_t t0 = r->t0_us;
    const int out_ms = total_ms * cut / 256;
    const int in_ms = total_ms - out_ms;
#define DPCHK(cond, ...) do { (*checks)++; if (!(cond)) { printf("FAIL dip %s: ", name); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)
    /* samples: 256 down to 0, 0, up to 256, each way monotone */
    int phase = 0, zeros = 0, down_mid = 0, up_mid = 0;
    int64_t first_zero = -1, in_start = -1, back_full = -1, idle_at = -1;
    int prev = 256;
    for (int i = 0; i < r->nsamples; i++) {
        const int d = r->samples[i].dip;
        if (phase == 0) {
            DPCHK(d <= prev, "sample %d: dip rose %d > %d before the dark", i, d, prev);
            if (d == 0) { phase = 1; first_zero = r->samples[i].us; }
            else if (d < 256) down_mid++;
        }
        if (phase == 1) {
            if (d == 0) zeros++;
            else phase = 2;
        }
        if (phase == 2) {
            DPCHK(d >= prev, "sample %d: dip fell %d < %d on the way up", i, d, prev);
            if (d > 0 && d < 256) up_mid++;
            if (d == 256 && back_full < 0) back_full = r->samples[i].us;
        }
        if (r->samples[i].phase == DIP_IN && in_start < 0) in_start = r->samples[i].us;
        if (in_start >= 0 && r->samples[i].phase == DIP_IDLE && idle_at < 0) idle_at = r->samples[i].us;
        prev = d;
    }
    DPCHK(first_zero >= 0 && zeros >= 1, "the dip never went dark (%d samples)", r->nsamples);
    DPCHK(prev == 256 && back_full >= 0, "the dip did not end at 256 (ends %d)", prev);
    DPCHK(down_mid >= 3 && up_mid >= 3, "the ramps are not smooth (%d samples down, %d up)", down_mid, up_mid);
    /* the clock, not the frames: out takes tr_out's time, in takes tr_in's */
    const int64_t stride = (int64_t)r->frame_ms * 1000 / 39 + 1000;
    DPCHK(first_zero >= t0 + (int64_t)out_ms * 1000 && first_zero <= t0 + (int64_t)out_ms * 1000 + stride,
          "dark at %lld us, wanted %lld..+%lld (out %d ms)", (long long)(first_zero - t0),
          (long long)out_ms * 1000, (long long)stride, out_ms);
    DPCHK(in_start >= 0 && idle_at >= in_start + (int64_t)in_ms * 1000 - 1000 &&
          idle_at <= in_start + (int64_t)in_ms * 1000 + stride && back_full <= idle_at,
          "the rise took %lld us, wanted %d ms (in)", (long long)(idle_at - in_start), in_ms);
    /* the hold: until the first whole frame of the new screen is flushed, or the cap */
    const int64_t out_end_ms = t0 / 1000 + out_ms;
    const int64_t hold_ms = in_start / 1000 - out_end_ms;
    if (expect_cap) {
        DPCHK(hold_ms >= DIP_HOLD_CAP_MS && hold_ms * 1000 <= (int64_t)DIP_HOLD_CAP_MS * 1000 + stride,
              "the cap: dark for %lld ms, wanted %d", (long long)hold_ms, DIP_HOLD_CAP_MS);
    } else {
        int ready_frame = -1;
        for (int i = 0; i < r->n && ready_frame < 0; i++) if (r->ready[i]) ready_frame = i;
        DPCHK(ready_frame >= 0, "no frame was ever the new screen, whole");
        if (ready_frame >= 0) {
            const int64_t flushed = r->t_us[ready_frame] + (int64_t)r->frame_ms * 1000;
            DPCHK(in_start >= flushed, "the dark ended at %lld us before the new screen was flushed (%lld us)",
                  (long long)(in_start - t0), (long long)(flushed - t0));
            DPCHK(in_start <= flushed + stride, "the dark outlived the first whole new frame: %lld us after it",
                  (long long)(in_start - flushed));
        }
        DPCHK(hold_ms < DIP_HOLD_CAP_MS, "the hold hit the cap (%lld ms)", (long long)hold_ms);
    }
    DPCHK((int64_t)r->hold_ms == hold_ms, "dip_hold_ms says %u, the hold was %lld ms", r->hold_ms, (long long)hold_ms);
    /* the writes: spaced, the law of the gamma, several inside a slow frame, back where they began */
    int per_frame[DP_MAX_FRAMES] = { 0 }, per_frame_max = 0;
    for (int i = 0; i < r->nwrites; i++) {
        if (i > 0) {
            DPCHK(r->writes[i].us / 1000 - r->writes[i - 1].us / 1000 >= DIP_WRITE_MS,
                  "writes %d and %d are %lld ms apart", i - 1, i, (long long)(r->writes[i].us / 1000 - r->writes[i - 1].us / 1000));
        }
        const int law = (want * dp_light_ref(r->writes[i].dip)) >> 8;
        DPCHK(r->writes[i].value == law, "write %d sent %d at dip %d, the gamma law says %d", i, r->writes[i].value,
              r->writes[i].dip, law);
        const int64_t f = (r->writes[i].us - t0) / ((int64_t)r->frame_ms * 1000);
        if (f >= 0 && f < DP_MAX_FRAMES && ++per_frame[f] > per_frame_max) per_frame_max = per_frame[f];
    }
    DPCHK(r->nwrites >= 4, "only %d writes", r->nwrites);
    DPCHK(r->frame_ms < 32 || per_frame_max >= 2, "a %d ms frame carried at most %d brightness step(s)", r->frame_ms,
          per_frame_max);
    DPCHK(r->nwrites > 0 && r->writes[r->nwrites - 1].value == want && r->panel == want,
          "the panel ends at %d, the mood asked for %d", r->panel, want);
    DPCHK(r->have == want && r->shown == want, "have %d shown %d, wanted %d", r->have, r->shown, want);
    DPCHK((int)r->dip_writes == r->nwrites, "dip_writes says %u, %d were made", r->dip_writes, r->nwrites);
    DPCHK(r->dip == 256 && r->phase == DIP_IDLE && !r->tail && r->mode == DIP_MODE_NONE,
          "not at rest: dip %d phase %d tail %d mode %d", r->dip, r->phase, (int)r->tail, r->mode);
#undef DPCHK
    return bad;
}

/* The letters of a run's frames: 'O' the old screen settled, 'N' the new one (the last one of a
 * double transition), 'M' the middle screen of a double transition, '?' none of them. */
static void dp_classify(const dp_run_t *r, const uint64_t *old, const uint64_t *n1, const uint64_t *n2, char *out)
{
    for (int i = 0; i < r->n; i++) {
        out[i] = r->hash[i] == old[i] ? 'O' : (n2 != NULL && r->hash[i] == n2[i]) ? 'N' :
                 (r->hash[i] == n1[i] ? (n2 != NULL ? 'M' : 'N') : '?');
    }
    out[r->n] = 0;
}

/* A frame composed while the panel is dark at every pump of it shows nothing: whatever it holds
 * (the new screen before its layer is bound, a screen that was never lit) is not on the glass. */
static bool dp_dark_frame(const dp_run_t *r, int i)
{
    bool any = false;
    for (int k = 0; k < r->nsamples; k++) {
        if (r->samples[k].us >= r->t_us[i] && r->samples[k].us < r->t_us[i] + (int64_t)r->frame_ms * 1000) {
            any = true;
            if (r->samples[k].dip != 0) return false;
        }
    }
    return any;
}

/* '?' frames that are lit: the ones that would be a half-faded or half-swapped picture on the glass. */
static int dp_lit_odd(const dp_run_t *r, const char *cls)
{
    int q = 0;
    for (int i = 0; i < r->n; i++) q += cls[i] == '?' && !dp_dark_frame(r, i);
    return q;
}

static int dp_check(int *checks)
{
    int bad = 0;
    mp_build_dial();
    static dp_run_t run;
    static uint64_t h_old[DP_MAX_FRAMES], h_new[DP_MAX_FRAMES], h_new2[DP_MAX_FRAMES];
    static char cls[DP_MAX_FRAMES + 1];
#define DPCHK(cond, ...) do { (*checks)++; if (!(cond)) { printf("FAIL dip: "); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

    for (int ci = 0; ci < DP_NCASES; ci++) {
        const dp_case_t *c = &DP_CASES[ci];
        /* the dip on, at a frame time the render cadence does not divide into the ramp */
        dp_transition(c, JR_KNOB_PANEL_DIP, 100, 40, -1, MP_WAIT, 0, false, &run);
        dp_refs(c, 0, MP_WAIT, 0, &run, h_old);
        dp_refs(c, 1, MP_WAIT, 0, &run, h_new);
        dp_classify(&run, h_old, h_new, NULL, cls);
        int o = 0, nn = 0, swapped_at = -1;
        const int q = dp_lit_odd(&run, cls);
        for (int i = 0; i < run.n; i++) {
            o += cls[i] == 'O'; nn += cls[i] == 'N';
            if (cls[i] == 'N' && swapped_at < 0) swapped_at = i;
        }
        DPCHK(q == 0, "%s: %d lit frame(s) are neither the old screen nor the new one: %s", c->name, q, cls);
        DPCHK(o >= 3 && nn >= 3, "%s: %d old and %d new frames (%s)", c->name, o, nn, cls);
        bool order = true;
        for (int i = swapped_at; swapped_at >= 0 && i < run.n; i++) if (cls[i] == 'O') order = false;
        DPCHK(order, "%s: the old screen came back after the new one: %s", c->name, cls);
        bad += dp_series_check(c->name, &run, 100, c->cut, c->total_ms, false, checks);
        printf("dip %-26s: %s\n", c->name, cls);
    }

    /* the mood: the sent value comes back to what the mood asked for, 100, 48, 8, and 85 (the cap) */
    {
        static const int wants[] = { 100, 48, 8, 85 };
        for (unsigned w = 0; w < sizeof wants / sizeof wants[0]; w++) {
            dp_transition(&DP_CASES[0], JR_KNOB_PANEL_DIP, wants[w], 40, -1, MP_WAIT, 0, false, &run);
            char nm[48];
            snprintf(nm, sizeof nm, "mood %d", wants[w]);
            bad += dp_series_check(nm, &run, wants[w], DP_CASES[0].cut, DP_CASES[0].total_ms, false, checks);
        }
    }

    /* a slow frame (100 ms, a transition frame at its worst on the device): the ramp keeps its clock */
    dp_transition(&DP_CASES[0], JR_KNOB_PANEL_DIP, 100, 100, -1, MP_WAIT, 0, false, &run);
    bad += dp_series_check("slow frames", &run, 100, DP_CASES[0].cut, DP_CASES[0].total_ms, false, checks);

    /* the write schedule at other frame times: the spacing, and the restoring write after the rise,
     * must hold wherever the pumps happen to fall against the ramp's end */
    {
        static const int fms[] = { 33, 37, 41, 47, 53, 61 };
        for (unsigned f = 0; f < sizeof fms / sizeof fms[0]; f++) {
            char nm[40];
            snprintf(nm, sizeof nm, "%d ms frames", fms[f]);
            dp_transition(&DP_CASES[0], JR_KNOB_PANEL_DIP, 100, fms[f], -1, MP_WAIT, 0, false, &run);
            bad += dp_series_check(nm, &run, 100, DP_CASES[0].cut, DP_CASES[0].total_ms, false, checks);
        }
    }

    /* the layer never binds: the dark waits for the cap and no longer */
    dp_transition(&DP_CASES[3], JR_KNOB_PANEL_DIP, 100, 40, -1, MP_WAIT, 0, true, &run);
    bad += dp_series_check("cap, the dial never binds", &run, 100, DP_CASES[3].cut, DP_CASES[3].total_ms, true, checks);

    /* a transition cut in on by another, in each phase */
    {
        static const struct { const char *name; int at; } cuts[] = {
            { "in the fall", 2 }, { "in the dark", 5 }, { "in the rise", 8 } };
        const dp_case_t *c = &DP_CASES[0];       /* weather > status, then status > the next screen */
        for (unsigned k = 0; k < sizeof cuts / sizeof cuts[0]; k++) {
            dp_transition(c, JR_KNOB_PANEL_DIP, 100, 40, cuts[k].at, MP_NEXT, 0, false, &run);
            dp_refs(c, 0, MP_WAIT, 0, &run, h_old);
            dp_refs(c, 1, MP_WAIT, 0, &run, h_new);
            dp_refs(c, 2, MP_NEXT, 0, &run, h_new2);
            dp_classify(&run, h_old, h_new, h_new2, cls);
            char nm[64];
            snprintf(nm, sizeof nm, "cut %s", cuts[k].name);
            int m = 0, nn = 0;
            const int q = dp_lit_odd(&run, cls);
            for (int i = 0; i < run.n; i++) { m += cls[i] == 'M'; nn += cls[i] == 'N'; }
            DPCHK(q == 0, "%s: %d lit frame(s) are no screen that was ever lit: %s", nm, q, cls);
            DPCHK(nn >= 3, "%s: the last screen never arrived: %s", nm, cls);
            if (cuts[k].at == 2) {
                DPCHK(m == 0, "%s: a screen that was never lit appeared: %s", nm, cls);
            }
            /* never a jump up to full brightness: after the cut the dip is at or under where it was */
            const int64_t cut_us = DP_T0_US + (int64_t)cuts[k].at * 40000;
            int level = 256, worst = 0;
            for (int i = 0; i < run.nsamples && run.samples[i].us < cut_us; i++) level = run.samples[i].dip;
            bool dark = false;
            for (int i = 0; i < run.nsamples && !dark; i++) {
                if (run.samples[i].us < cut_us) continue;
                if (run.samples[i].dip > worst) worst = run.samples[i].dip;
                dark = run.samples[i].dip == 0;
            }
            DPCHK(dark && worst <= level + 16, "%s: the dip was %d at the cut and rose to %d before the dark", nm, level, worst);
            DPCHK(run.dip == 256 && run.phase == DIP_IDLE && !run.tail && run.mode == DIP_MODE_NONE,
                  "%s: not at rest: dip %d phase %d", nm, run.dip, run.phase);
            DPCHK(run.nwrites > 0 && run.writes[run.nwrites - 1].value == 100 && run.panel == 100,
                  "%s: the panel ends at %d", nm, run.panel);
            for (int i = 1; i < run.nwrites; i++) {
                DPCHK(run.writes[i].us / 1000 - run.writes[i - 1].us / 1000 >= DIP_WRITE_MS, "%s: writes %d ms apart", nm,
                      (int)(run.writes[i].us / 1000 - run.writes[i - 1].us / 1000));
            }
            printf("dip %-26s: %s\n", nm, cls);
        }
        /* a style change cut in on by a screen change that leaves the watch */
        const dp_case_t *s = &DP_CASES[3];
        dp_transition(s, JR_KNOB_PANEL_DIP, 100, 40, 2, MP_PREV, 0, false, &run);
        {
            static uint16_t fb[466 * 466];
            dp_start(s->scene, 0u, 100, DP_T0_US - 4000000LL);
            int64_t t = DP_T0_US - 4000000LL;
            dp_apply(s->op, s->arg);
            for (int k = 0; k < 40; k++) { t += 1000000LL / MP_FPS; mp_frame(t, fb); }
            dp_apply(MP_PREV, 0);
            for (int k = 0; k < 40; k++) { t += 1000000LL / MP_FPS; mp_frame(t, fb); }
            for (int i = 0; i < run.n; i++) {
                s_fade_prev_ms = (uint32_t)(run.t_us[i] / 1000) - 42u;
                s_space_hold_ms = (uint32_t)(DP_T0_US / 1000) + 80U + (uint32_t)JR_DISPLAY_SPACE_HOLD_MS;   /* the rail lit by the screen change, in frame 2 */
                mp_entry_override = (int64_t)run.entry[i];
                mp_frame(run.t_us[i], fb);
                h_new2[i] = fc_hash(fb);
            }
            mp_entry_override = -1;
            dp_start(s->scene, 0u, 100, DP_T0_US);
            for (int i = 0; i < run.n; i++) {
                s_fade_prev_ms = (uint32_t)(run.t_us[i] / 1000) - 42u;
                s_space_hold_ms = i >= 2 ? (uint32_t)(DP_T0_US / 1000) + 80U + (uint32_t)JR_DISPLAY_SPACE_HOLD_MS : 0U;
                mp_frame(run.t_us[i], fb);
                h_old[i] = fc_hash(fb);
            }
        }
        dp_classify(&run, h_old, h_new2, NULL, cls);
        int nn = 0;
        const int q = dp_lit_odd(&run, cls);
        for (int i = 0; i < run.n; i++) nn += cls[i] == 'N';
        DPCHK(nn >= 3, "style cut by a screen change: the screen never arrived: %s", cls);
        DPCHK(q == 0, "style cut by a screen change: %d lit odd frames: %s", q, cls);
        DPCHK(run.dip == 256 && run.phase == DIP_IDLE && run.panel == 100 && run.mode == DIP_MODE_NONE,
              "style cut by a screen change: not at rest (dip %d, panel %d)", run.dip, run.panel);
        printf("dip %-26s: %s\n", "style cut by a screen", cls);
    }

    /* knob 0: nothing of this runs, and the fade is still in the pixels */
    {
        const dp_case_t *c = &DP_CASES[0];
        dp_transition(c, 0u, 100, 40, -1, MP_WAIT, 0, false, &run);
        dp_refs(c, 0, MP_WAIT, 0, &run, h_old);
        dp_refs(c, 1, MP_WAIT, 0, &run, h_new);
        dp_classify(&run, h_old, h_new, NULL, cls);
        const int q = dp_lit_odd(&run, cls);
        DPCHK(q >= 3, "knob 0: the slide has only %d part-strength frames, the fade is gone: %s", q, cls);
        DPCHK(run.nwrites == 0 && run.calls == 0U && run.dip_writes == 0U,
              "knob 0 wrote the brightness %d times", run.nwrites);
        bool still = true;
        for (int i = 0; i < run.nsamples; i++) still = still && run.samples[i].dip == 256 && run.samples[i].phase == DIP_IDLE;
        DPCHK(still && run.mode == DIP_MODE_NONE && run.nsamples > 0, "knob 0 ran a dip");
        printf("dip %-26s: %s\n", "knob 0 (the fade, in pixels)", cls);
    }
#undef DPCHK
    fc_knob_extra = 0;
    mp_frame_ms = 0;
    s_space_hold_ms = 0U;                /* the refs lit the rail; the next suite starts with it off */
    s_space_serial_seen = 0;             /* fc_setup builds a nav word with serial 0: the next suite must meet no edge */
    return bad;
}
