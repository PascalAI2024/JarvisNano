/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_jitter.c — the playback feeder's reply / hole / pre-roll walk,
 * driven on the host with no ring, no DAC and no clock.
 *
 * The stepper is fed the way the feeder feeds it: once per pass, with the
 * ring level it saw, whether the last DAC write is still audible, and whether
 * an earcon owns the ring. A pass is one TICK here; the feeder's own cadence
 * is ~32 ms while writing and 10 ms while dry, and nothing below depends on
 * which — only on the 80 ms tail and the 2.5 s reply window.
 */
#include "unity.h"
#include "jr_dsp/dsp.h"
#include <string.h>

#define TICK_MS 10u

typedef struct {
    jr_jitter_t j;
    uint32_t now;
    uint32_t tail_until;      /* the harness's DAC tail, like the feeder's */
    unsigned holes, gaps_opened, gaps_ended, replies_ended, quiet, changes;
    uint32_t last_hole_ms;
    uint32_t last_prev_ms;
} sim_t;

static void sim_init(sim_t *s, uint32_t preroll_ms)
{
    memset(s, 0, sizeof *s);
    jr_jitter_init(&s->j, preroll_ms);
    s->now = 1000u;
}

static void tick(sim_t *s, uint32_t avail, bool earcon)
{
    const bool tail = (int32_t)(s->tail_until - s->now) > 0;
    jr_jitter_out_t o;
    jr_jitter_feed(&s->j, s->now, avail, tail, earcon, &o);
    s->holes += o.hole_booked;
    s->gaps_opened += o.gap_opened;
    s->gaps_ended += o.gap_ended;
    s->replies_ended += o.reply_ended;
    s->quiet += o.quiet;
    s->changes += o.preroll_changed;
    if (o.hole_booked) {
        s->last_hole_ms = o.hole_ms;
    }
    if (o.preroll_changed) {
        s->last_prev_ms = o.preroll_prev_ms;
    }
    if (avail > 0u) {
        /* A write happened this pass; it is audible 80 ms past its end. */
        s->tail_until = s->now + TICK_MS + JR_JITTER_TAIL_MS;
    }
    s->now += TICK_MS;
}

static void play(sim_t *s, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) tick(s, 768u, false);
}

static void dry(sim_t *s, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) tick(s, 0u, false);
}

/* A reply that plays cleanly, then the silence that ends it. */
static void clean_reply(sim_t *s)
{
    play(s, 400u);
    dry(s, 3000u);
}

/* A reply with one 300 ms network hole in the middle. */
static void holey_reply(sim_t *s)
{
    play(s, 500u);
    dry(s, 300u);
    play(s, 500u);
    dry(s, 3000u);
}

/* The measured defect: a 100 ms tone fed 256 samples at a time by the app
 * task while the prio-19 feeder drains 768 — the ring is dry every other
 * pass with the tone's own tail still audible. */
static void starved_earcon(sim_t *s, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        tick(s, (t / TICK_MS) % 2u == 0u ? 256u : 0u, true);
    }
}

static void test_jitter_reply_with_one_hole_steps_600_to_900(void)
{
    sim_t s;
    sim_init(&s, 600u);
    holey_reply(&s);
    TEST_ASSERT_EQUAL_UINT(1u, s.holes);
    TEST_ASSERT_UINT32_WITHIN(TICK_MS, 300u, s.last_hole_ms);
    TEST_ASSERT_EQUAL_UINT(1u, s.replies_ended);
    TEST_ASSERT_EQUAL_UINT(1u, s.gaps_ended);
    TEST_ASSERT_EQUAL_UINT(1u, s.changes);
    TEST_ASSERT_EQUAL_UINT32(600u, s.last_prev_ms);
    TEST_ASSERT_EQUAL_UINT32(900u, s.j.preroll_ms);
}

static void test_jitter_three_clean_replies_step_900_to_700(void)
{
    sim_t s;
    sim_init(&s, 900u);
    clean_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(900u, s.j.preroll_ms);
    clean_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(900u, s.j.preroll_ms);
    clean_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(700u, s.j.preroll_ms);
    TEST_ASSERT_EQUAL_UINT(0u, s.holes);
    TEST_ASSERT_EQUAL_UINT(3u, s.replies_ended);
    TEST_ASSERT_EQUAL_UINT(1u, s.changes);
}

static void test_jitter_earcon_is_no_reply_no_hole_no_step(void)
{
    sim_t s;
    sim_init(&s, 600u);
    starved_earcon(&s, 100u);
    dry(&s, 3000u);
    TEST_ASSERT_GREATER_THAN_UINT(0u, s.quiet);
    TEST_ASSERT_EQUAL_UINT(0u, s.holes);
    TEST_ASSERT_EQUAL_UINT(0u, s.gaps_opened);
    TEST_ASSERT_EQUAL_UINT(0u, s.gaps_ended);
    TEST_ASSERT_EQUAL_UINT(0u, s.replies_ended);
    TEST_ASSERT_EQUAL_UINT(0u, s.changes);
    TEST_ASSERT_EQUAL_UINT32(600u, s.j.preroll_ms);
    TEST_ASSERT_FALSE(s.j.reply_open);
}

static void test_jitter_hole_inside_earcon_tail_is_ignored(void)
{
    sim_t s;
    sim_init(&s, 600u);
    /* Chunked enqueue: the outstanding count itself drops to zero between the
     * app task's chunks, so the starvation hole lands in the earcon's TAIL,
     * not under an outstanding sample. The DAC tail is active throughout. */
    tick(&s, 256u, true);
    tick(&s, 0u, false);
    tick(&s, 256u, true);
    tick(&s, 0u, false);
    tick(&s, 256u, true);
    dry(&s, 3000u);
    TEST_ASSERT_EQUAL_UINT(0u, s.holes);
    TEST_ASSERT_EQUAL_UINT(0u, s.gaps_opened);
    TEST_ASSERT_EQUAL_UINT(0u, s.replies_ended);
    TEST_ASSERT_EQUAL_UINT32(600u, s.j.preroll_ms);

    /* And the stepper is fully awake again for the real reply that follows. */
    clean_reply(&s);
    TEST_ASSERT_EQUAL_UINT(1u, s.replies_ended);
    TEST_ASSERT_EQUAL_UINT(0u, s.holes);
}

static void test_jitter_earcon_after_reply_leaves_it_clean(void)
{
    sim_t s;
    sim_init(&s, 900u);
    /* A reply ends, its tail passes, the owner long-presses mute: the sweep
     * plays inside the 2.5 s window. The reply must still count as clean. */
    play(&s, 400u);
    dry(&s, 200u);
    starved_earcon(&s, 100u);
    dry(&s, 3000u);
    TEST_ASSERT_EQUAL_UINT(0u, s.holes);
    TEST_ASSERT_EQUAL_UINT(1u, s.replies_ended);
    TEST_ASSERT_EQUAL_UINT32(1u, s.j.clean_replies);
    TEST_ASSERT_EQUAL_UINT32(900u, s.j.preroll_ms);
}

static void test_jitter_ceiling_holds_at_1500(void)
{
    sim_t s;
    sim_init(&s, 1400u);
    holey_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(1500u, s.j.preroll_ms);
    holey_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(1500u, s.j.preroll_ms);
    TEST_ASSERT_EQUAL_UINT(2u, s.holes);
    TEST_ASSERT_EQUAL_UINT(1u, s.changes);
}

static void test_jitter_floor_holds_at_600(void)
{
    sim_t s;
    sim_init(&s, 700u);
    clean_reply(&s);
    clean_reply(&s);
    clean_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(600u, s.j.preroll_ms);
    clean_reply(&s);
    clean_reply(&s);
    clean_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(600u, s.j.preroll_ms);
    TEST_ASSERT_EQUAL_UINT(6u, s.replies_ended);
    TEST_ASSERT_EQUAL_UINT(1u, s.changes);
}

static void test_jitter_pinned_preroll_never_moves(void)
{
    sim_t s;
    sim_init(&s, 600u);
    jr_jitter_pin(&s.j, 1000u);
    holey_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(1000u, s.j.preroll_ms);
    clean_reply(&s);
    clean_reply(&s);
    clean_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(1000u, s.j.preroll_ms);
    TEST_ASSERT_EQUAL_UINT(1u, s.holes);          /* still counted */
    TEST_ASSERT_EQUAL_UINT(4u, s.replies_ended);  /* still delimited */
    TEST_ASSERT_EQUAL_UINT(0u, s.changes);

    /* preroll=0 resets to the floor and re-enables the walk. */
    jr_jitter_pin(&s.j, 0u);
    TEST_ASSERT_EQUAL_UINT32(600u, s.j.preroll_ms);
    holey_reply(&s);
    TEST_ASSERT_EQUAL_UINT32(900u, s.j.preroll_ms);
    TEST_ASSERT_EQUAL_UINT(1u, s.changes);

    jr_jitter_pin(&s.j, 5000u);
    TEST_ASSERT_EQUAL_UINT32(3000u, s.j.preroll_ms);
}

/* ---- registration (called from test_core.c main, inside UNITY_BEGIN/END) ---- */
void jitter_tests_run(void)
{
    RUN_TEST(test_jitter_reply_with_one_hole_steps_600_to_900);
    RUN_TEST(test_jitter_three_clean_replies_step_900_to_700);
    RUN_TEST(test_jitter_earcon_is_no_reply_no_hole_no_step);
    RUN_TEST(test_jitter_hole_inside_earcon_tail_is_ignored);
    RUN_TEST(test_jitter_earcon_after_reply_leaves_it_clean);
    RUN_TEST(test_jitter_ceiling_holds_at_1500);
    RUN_TEST(test_jitter_floor_holds_at_600);
    RUN_TEST(test_jitter_pinned_preroll_never_moves);
}
