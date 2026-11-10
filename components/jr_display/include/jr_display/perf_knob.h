/* The dev-only perf knob: one word, one bit per cost candidate.
 *
 * A bit changes what a frame COSTS. A bit of the first round changed no pixel;
 * a fade bit may change the pixels of a frame in motion within a
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

/* 2026-11-10, the fade round: one candidate, FADE_SCREEN (the ring data screens
 * drawn at full strength and dimmed once instead of blended per primitive), lost
 * on the device (240 MHz, lease held) and is deleted. Mean ms per frame in the
 * 0.9 s after the trigger, knob 0 against the bit: WEATHER > STATUS 63 / 72,
 * STATUS > DESK 59 / 55 (65, 45), DESK > ACTIVITY 38 / 41, WATCH > WEATHER
 * 52 / 59. Blending at partial strength is not what makes a transition frame cost
 * two to three times a settled one. The bit numbers are free again.
 */
/* 2026-11-10, the replay round (docs/reference/frame-cost.md): a transition frame
 * is slow because a record is bypassed or a flat colour is blended one pixel at a
 * time. Each bit keeps the output of the shipped path (settled scenes identical, a
 * frame in motion inside the identity suite's tolerance) and does less work.
 *   bit 2 DISC_MEMO   aa_disc at part strength remembers the last pixel it blended
 *                     (a flat disc over a flat base is one blend per run).
 * Bits 0, 1 and 3 are reserved for the other candidates of the round. */
#define JR_KNOB_DISC_MEMO       (1u << 2)

#define JR_KNOB_KNOWN_MASK      (JR_KNOB_DISC_MEMO)
