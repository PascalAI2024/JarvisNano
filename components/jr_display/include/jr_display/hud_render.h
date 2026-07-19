/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * hud_render.h — pure procedural "arc reactor" HUD renderer (466x466 RGB565).
 *
 * This is the visual identity of JarvisRobot v5 (docs/VISION.md): a breathing
 * arc-reactor ring, an amplitude-reactive waveform, orbiting thinking comets,
 * and a boot bloom — all computed, no baked assets, no full framebuffer.
 * Rendering is strip-oriented: the caller asks for N rows at a time and blits
 * them, so peak RAM is one strip, never a 434 KB frame.
 *
 * Pure C, no ESP-IDF / FreeRTOS includes — host-compilable for tests, exactly
 * like jr_core/jr_dsp. All math is integer (LUT sine, incremental r², integer
 * sqrt for scanline band intervals; angular elements are plotted polar→
 * cartesian so no atan is ever needed).
 *
 * Concurrency contract (matches the presenter split in jr_display.c):
 *   - hud_set() may be called from ANY task; it only stores one aligned
 *     32-bit word (face|amp) — atomic on Xtensa/RISC-V by alignment.
 *   - hud_tick() + hud_render_rows() must be called from ONE render task;
 *     they own every other field.
 */
#ifndef JR_DISPLAY_HUD_RENDER_H
#define JR_DISPLAY_HUD_RENDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUD_W 466
#define HUD_H 466

/* Faces mirror jr_face_t plus the boot intro. The presenter maps 1:1. */
typedef enum {
    HUD_FACE_BOOT = 0,   /* bloom intro; auto-promotes to the pending face */
    HUD_FACE_IDLE,
    HUD_FACE_LISTEN,
    HUD_FACE_THINK,
    HUD_FACE_SPEAK,
    HUD_FACE_ERROR,
    HUD_FACE_COUNT
} hud_face_t;

#define HUD_BARS      48   /* waveform bars around the ring */
#define HUD_RAMP_LEVELS 33 /* 0..32 inclusive brightness ramp per palette entry */

typedef enum {
    HUD_COL_CYAN = 0,
    HUD_COL_GOLD,
    HUD_COL_RED,
    HUD_COL_TICK,
    HUD_COL_COUNT
} hud_color_t;

typedef struct {
    /* ---- cross-task mailbox (the ONLY field other tasks may touch) ---- */
    volatile uint32_t req;        /* (face << 8) | amp — single aligned write */

    /* ---- render-task-owned animation state ---- */
    hud_face_t cur, prev;         /* crossfade endpoints */
    uint32_t   trans_start_ms;    /* crossfade start; k=1 after HUD_TRANS_MS */
    bool       boot_done;         /* bloom finished; face requests now honored */
    uint32_t   boot_start_ms;
    uint32_t   now_ms;            /* last hud_tick time */

    int        amp;               /* smoothed amplitude 0..255 (attack/decay) */
    uint8_t    amp_hist[HUD_BARS];/* per-frame history feeding the bars */
    int        hist_head;
    int        comet_a;           /* comet head angle, 0..255 wrap (Q8) */
    int        bar_rot;           /* slow waveform rotation, Q8 angle units */
    int        breath_phase;      /* free-running phase accumulator */

    /* ---- palette (built once; byte order baked in) ---- */
    uint16_t   ramp[HUD_COL_COUNT][HUD_RAMP_LEVELS];
    bool       swap_bytes;
} hud_t;

/* Crossfade duration between faces. */
#define HUD_TRANS_MS 250u
/* Boot bloom duration before the first real face is honored. */
#define HUD_BOOT_MS  1500u

/* Initialize state + palette. swap_bytes: true if the panel wants big-endian
 * RGB565 in memory (QSPI CO5300 path — confirmed against the v4 blit). */
void hud_init(hud_t *h, uint32_t now_ms, bool swap_bytes);

/* Post the desired face + amplitude (0..255). Callable from any task. */
void hud_set(hud_t *h, hud_face_t face, uint8_t amp);

/* Re-arm the boot bloom to start at now_ms — called by the presenter the
 * moment the panel actually lights, so the intro plays from its true start
 * (panel bring-up takes ~1.4 s of vendor-mandated delays). Render-task only.
 * Face requests posted via hud_set() meanwhile are preserved and honored
 * once the bloom completes. */
void hud_restart_boot(hud_t *h, uint32_t now_ms);

/* Advance animation state one frame. Render-task only. */
void hud_tick(hud_t *h, uint32_t now_ms);

/* Render rows [y0, y0+nrows) of the current frame into dst
 * (nrows * HUD_W uint16 pixels). Render-task only. */
void hud_render_rows(hud_t *h, uint16_t *dst, int y0, int nrows);

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_HUD_RENDER_H */
