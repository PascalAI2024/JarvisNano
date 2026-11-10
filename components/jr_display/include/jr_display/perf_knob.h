/* The dev-only perf knob: one word, one bit per cost candidate.
 *
 * A bit changes what a frame COSTS. A bit of the first round changed no pixel;
 * the four fade bits below change the pixels of a frame in motion within a
 * measured tolerance and leave every settled scene identical (the frame-identity
 * suite renders every scene with each bit and compares hashes, and plays the
 * transitions with and without each bit and measures the difference). The
 * device is the only judge of which bit is faster, because host instruction counts have predicted the ESP32-S3
 * wrong twice (more stores, slower, even with fewer instructions), so every
 * candidate ships off and is measured from one flash:
 *
 *     POST /api/debug/display?knob=<n>       GET /api/display  ("knob")
 *
 * The render task reads the word once a frame (knob_apply in jr_display.c) and
 * copies it to the two words the drawing files read, hud_perf_knob and
 * glass_perf_knob. Knob 0 is the shipped path. The losing paths are deleted
 * after the device has spoken, not before.
 *
 * A new candidate takes the next free bit, is ORed into JR_KNOB_KNOWN_MASK, and
 * is read from the two words (or, for the fade bits, from hud_perf_knob).
 *
 * 2026-11-10, the seven candidates of the first round (their bit numbers are
 * free again; the fade round below reuses them), measured on the device
 * (240 MHz, lease held) and settled:
 *   bit 0 record the MINIMAL dial disc: helped only the slow path; deleted.
 *   bit 1 hud_render.c fill16, one 16-bit store a pixel in place of two to a
 *         word: -12 ms on MINIMAL; the per-pixel loop is the only path.
 *   bit 2 glass_draw.c fill16, the same: neutral; the per-pixel loop is the
 *         only path.
 *   bit 3 aa_disc before 91b44ec: -12.7 ms on MINIMAL; it is the only aa_disc.
 *   bit 4 SKELETON balance wheel cell classes: no gain; deleted.
 *   bit 5 dial copy of the columns inside the round glass: about -1.1 ms on
 *         every baked dial; the only path (the corners hold stale strip data).
 *   bit 6 aa_fill interior written once: neutral on PILOT, worse on FUTURE and
 *         DIVER; deleted.
 * docs/reference/frame-cost.md, "The knob round, 2026-11-10", has the numbers. */
#pragma once

/* 2026-11-10, the fade round: a screen at partial strength used to blend each
 * primitive it drew; with a bit set the content is drawn at full strength and the
 * finished rows are dimmed once (sp_dim_disc in jr_display.c).
 *   bit 0 FADE_SCREEN       the ring data screens (STATUS, WEATHER, DESK,
 *                           ACTIVITY) sliding in and out, or under the sheet.
 *   bit 1 FADE_WATCH        the watch hands, complications and a drawn dial
 *                           entering, leaving and dipping on a style change;
 *                           a baked dial is dimmed with them, not before.
 *   bit 2 FADE_WATCH_SHADE  the watch under the opening or closing shade.
 *   bit 3 FADE_SHADE        the shade itself opening and closing. */
#define JR_KNOB_FADE_SCREEN       (1u << 0)
#define JR_KNOB_FADE_WATCH        (1u << 1)
#define JR_KNOB_FADE_WATCH_SHADE  (1u << 2)
#define JR_KNOB_FADE_SHADE        (1u << 3)

#define JR_KNOB_KNOWN_MASK      (JR_KNOB_FADE_SCREEN)
