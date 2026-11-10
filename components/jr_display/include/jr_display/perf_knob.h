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
 * after the device has spoken, not before. */
#pragma once

/* The MINIMAL dial's disc, recorded the first time it is drawn and replayed
 * (hud_dial_cache_set). Off on the device since 2026-11-10; the 12 KB of PSRAM
 * is taken only when this bit is first set. */
#define JR_KNOB_DIAL_REC        (1u << 0)
/* hud_render.c fill16 as the store loop it was before 91b44ec (one 16-bit
 * store per pixel) in place of two pixels to a word. */
#define JR_KNOB_HUD_FILL_OLD    (1u << 1)
/* glass_draw.c fill16, the same. */
#define JR_KNOB_GLASS_FILL_OLD  (1u << 2)
/* aa_disc as it was before 91b44ec, verbatim (aa_disc_v0): no record branches,
 * the interior stored by a plain loop. */
#define JR_KNOB_DISC_ROW_OLD    (1u << 3)

#define JR_KNOB_KNOWN_MASK      (JR_KNOB_DIAL_REC | JR_KNOB_HUD_FILL_OLD | \
                                 JR_KNOB_GLASS_FILL_OLD | JR_KNOB_DISC_ROW_OLD)
