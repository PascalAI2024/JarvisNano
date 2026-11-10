/* The dev-only perf knob: one word, one bit per cost candidate.
 *
 * A bit changes what a frame COSTS, never what it shows: with any bit set the
 * pixels are identical to knob 0 (the frame-identity suite renders every scene
 * with each bit and compares hashes). The device is the only judge of which
 * bit is faster, because host instruction counts have predicted the ESP32-S3
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
 * There are no candidates at present: JR_KNOB_KNOWN_MASK is 0, so the setter
 * keeps no bit and the identity suite's per-bit loop has nothing to run. A new
 * candidate takes bit 0, is ORed into the mask, and is read from the two words.
 *
 * 2026-11-10, the seven candidates of the first round, measured on the device
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

#define JR_KNOB_KNOWN_MASK      0u
