/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reactive "Siri-style" waveform face — baked-EAF implementation.
 *
 * Drop-in replacement for waveform.c. Same public symbols (emote_face_init,
 * emote_face_set_state, emote_face_set_synthetic_amplitude,
 * emote_face_set_amplitude_source) so emote.c (calls emote_face_init at
 * emote.c:281) and app_claw_face_bridge.c need NO changes.
 *
 * WHY THIS, NOT waveform.c: this engine (espressif2022/esp_emote_gfx, vendored
 * in the build clone) has NO canvas widget. A runtime CPU-drawn RGB565A8 buffer
 * pushed via gfx_img_set_src renders as a skewed spiral / white on this CO5300
 * panel (proven empirically — even a 64x64 solid square spiraled). The only
 * sanctioned reactive lever is the BAKED gfx_anim widget. gfx_motion is NOT
 * shipped in this engine version (no include/widget/gfx_motion.h), so affine
 * transforms are off the table too.
 *
 * APPROACH: one custom gfx_anim object ("rwave"), four baked looping EAFs (idle/
 * listen/think/speak). High-quality overrides may live on SD at
 * /sdcard/emote/rwave_*.eaf and are copied into PSRAM on first load. If SD is
 * absent or invalid, the small boot-safe partition assets remain the fallback.
 * The reactive states map the live 0..1000 amplitude to a quantized loop END
 * inside a pre-baked "amplitude ramp". The loop always starts at frame 0; moving
 * the start into the ramp makes a narrow bucket of near-identical frames look
 * frozen on the panel. Quantizing only the end preserves motion without resetting
 * the animation every driver tick.
 *
 * CRITICAL — single-frame pinning does NOT work on this engine. The anim timer
 * (gfx_anim.c:824) only DRAWS a frame in the `current_frame < end_frame` branch;
 * with start==end it resets without drawing. So we always loop [0..end] with
 * end>=1 (it traverses intermediate frames and keeps drawing). See plan §2.
 *
 * Engine API used (all verified in the clone, file:line):
 *   emote_create_obj_by_type(h, "anim", name)  emote_api.h  (-> gfx_anim_create, emote_setup.c:245)
 *   emote_get_asset_data_by_name(h, name, &d, &n)  emote_assets.h:89
 *   gfx_anim_set_src(obj, data, len)            gfx_anim.h:45
 *   gfx_anim_set_segment(obj, start, end, fps, repeat)  gfx_anim.h:56  (clamps end, gfx_anim.c:1005)
 *   gfx_anim_start(obj) / gfx_anim_stop(obj)    gfx_anim.h:63 / :70
 *   gfx_obj_align(obj, GFX_ALIGN_CENTER, 0, 0)  core/gfx_obj.h
 *   emote_set_obj_visible / emote_set_anim_visible / emote_lock / emote_unlock  emote_api.h
 *
 * See docs/REACTIVE_FACE_PLAN.md for the full design + open questions.
 */

#include "emote.h"   /* public face API: emote_face_state_t + emote_face_* (in include/) */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "soc/soc.h"        /* SOC_DROM_LOW / SOC_DROM_HIGH — the mmap cache window */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx.h"   /* pulls in widget/gfx_anim.h + core/gfx_obj.h */
#include "expression_emote.h"
#include "display_arbiter.h"

static const char *TAG = "app_rwave";

/* Custom gfx object name (must not collide with predefined emote elem names). */
#define RWAVE_OBJ_NAME    "rwave"

/* The flash partition the emote assets are mounted from (mirrors emote.c:22).
 * Used to esp_partition_read clips when assets are NOT memory-mapped (XIP build). */
#define EMOTE_ASSETS_PARTITION  "emote"

/* Baked-asset file names — HARD CONTRACT with gen_reactive_face.py's STATE_ORDER
 * and with the `emote` partition manifest the orchestrator must populate. */
#define RWAVE_ASSET_IDLE    "rwave_idle.eaf"
#define RWAVE_ASSET_LISTEN  "rwave_listen.eaf"
#define RWAVE_ASSET_THINK   "rwave_think.eaf"
#define RWAVE_ASSET_SPEAK   "rwave_speak.eaf"
#define RWAVE_SD_DIR        "/sdcard/emote"
#define RWAVE_SD_CLIP_MAX   (8U * 1024U * 1024U)

/* Per-state loop rate, applied via gfx_anim_set_segment() below. NOTE: the
 * loop/fps fields in assets_local/284_240/emote.json are NOT load-bearing for
 * THIS custom anim object — those are only read by emote_set_anim_emoji (the eye
 * flow, emote_op.c:287). This object sets its own fps here. The manifest fps
 * values mirror these for documentation, but THESE constants are authoritative.
 * Reactive states run at the engine target rate; the baked clips are deliberately
 * calm, so this reads as fluid motion rather than sparkle-noise. */
#define RWAVE_FPS_IDLE      24
#define RWAVE_FPS_REACTIVE  30
#define RWAVE_FPS_THINK     30

/* Amplitude driver task cadence (Hz) and lerp for fluid attack/decay. */
#define RWAVE_DRIVER_HZ     30
#define RWAVE_DRIVER_MS     (1000 / RWAVE_DRIVER_HZ)
#define RWAVE_LERP          0.35f   /* eased displayed level toward target. The driver task only
                                     * achieves ~6 Hz effective cadence (emote_lock contention with
                                     * the 30 fps render loop), so 0.18 measured as visibly laggy
                                     * voice tracking; 0.35 at ~6 Hz ≈ the intended 0.18 at 30 Hz. */
#define RWAVE_AMP_BUCKETS   8U      /* coarse end buckets reduce segment resets when RMS jitters */
#define RWAVE_MIN_END_DIV   3U      /* quiet-voice floor: never loop fewer than last/3 frames.
                                     * Without this, low amplitude maps to end=2..4 → a ~130 ms
                                     * micro-loop that strobes on the panel instead of animating. */

/* One baked EAF held resident in memory for its object's lifetime. The decoder
 * keeps the pointer (it does NOT copy — cf. eye's emote_acquire_data,
 * emote_op.c:283), so these blobs must stay valid while shown. They are mmap'd
 * read-only out of the asset partition; we just cache the pointer + frame count. */
typedef struct {
    const uint8_t *data;     /* EAF blob usable for gfx_anim_set_src + frame_count.
                              * In mmap mode this aliases the flash cache window; in
                              * XIP/non-mmap mode it points at `buf` (a heap copy). */
    uint8_t       *buf;      /* heap copy when assets are NOT memory-mapped (XIP);
                              * NULL when `data` aliases the mmap window. Freed in
                              * deinit. The gfx anim decoder keeps a pointer into the
                              * src for the object's lifetime, so this must persist. */
    size_t         size;
    uint32_t       frames;   /* total frames INCLUDING the trailing _C sentinel */
    uint32_t       last;     /* max `end` for set_segment: the loop draws [0..end-1],
                              * so end == frames-1 is needed to display the highest
                              * real frame (index frames-2). The _C sentinel sits at
                              * frames-1 and is never prepared (timer resets without
                              * drawing on cf >= end_frame), so this cap is safe. */
    bool           loaded;
} rwave_clip_t;

typedef struct {
    emote_handle_t      emote;
    gfx_obj_t          *obj;          /* the custom "rwave" gfx_anim object */
    rwave_clip_t        idle, listen, think, speak;

    TaskHandle_t        task;
    volatile bool       run;
    volatile emote_face_state_t state;
    volatile int        synth_amp;    /* -1 = use live cb; else 0..1000 */
    emote_face_amp_cb_t amp_cb;

    float               disp_amp;     /* eased displayed amplitude 0..1 */
    int                 last_start;   /* last segment start issued (avoid churn) */
    int                 last_end;     /* last segment end issued (avoid churn) */
    const rwave_clip_t *cur_clip;     /* clip whose src is currently set */
    emote_face_state_t  applied_state; /* last state for which visibility was applied
                                        * (only written by rwave_task, no volatile needed) */
    uint32_t            driver_ticks;
    uint32_t            segment_sets;
    uint32_t            keepalive_kicks;
    uint32_t            state_changes;
} rwave_ctx_t;

static rwave_ctx_t s_rw;

/* ---- asset loading -------------------------------------------------------- */

/* Read the EAF header's frame-count field (offset 4, int32 LE) per the format in
 * gfx_eaf_dec.h:34 — the count INCLUDES the trailing _C sentinel frame, so the
 * last drawable frame index is (count - 2). We avoid eaf_init() here (internal
 * decoder API) and read the count straight from the header, which is stable. */
static uint32_t rwave_eaf_frame_count(const uint8_t *d, size_t n)
{
    /* Mirror eaf_init's validation (gfx_eaf_dec.c:724): byte 0 = 0x89,
     * bytes 1..3 = "EAF" or "AAF", frame count = int32 LE at offset 4. The count
     * INCLUDES the trailing _C sentinel frame. */
    if (!d || n < 8 || d[0] != 0x89) {
        return 0;
    }
    if (memcmp(d + 1, "EAF", 3) != 0 && memcmp(d + 1, "AAF", 3) != 0) {
        return 0;
    }
    int32_t total;
    memcpy(&total, d + 4, sizeof(total));    /* little-endian int32 */
    return total > 0 ? (uint32_t)total : 0;
}

/* ESP32-S3 flash/PSRAM mmap cache window. A pointer in this range is a real,
 * dereferenceable mapped address; anything below it (handed back by
 * mmap_assets_get_mem when assets are NOT memory-mapped) is a raw PARTITION BYTE
 * OFFSET that must be read with esp_partition_read, not dereferenced. */
#ifndef SOC_DROM_LOW
#define SOC_DROM_LOW   0x3C000000UL
#endif
#ifndef SOC_DROM_HIGH
#define SOC_DROM_HIGH  0x40000000UL
#endif

static inline bool rwave_ptr_is_mapped(const void *p)
{
    return (uintptr_t)p >= SOC_DROM_LOW && (uintptr_t)p < SOC_DROM_HIGH;
}

static esp_err_t rwave_acquire_sd(const char *name, const uint8_t **out,
                                  uint8_t **owned, size_t *out_size)
{
    char path[96];
    int written = snprintf(path, sizeof(path), "%s/%s", RWAVE_SD_DIR, name);
    if (written < 0 || written >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long file_size = ftell(f);
    if (file_size <= 0 || (uint32_t)file_size > RWAVE_SD_CLIP_MAX) {
        fclose(f);
        ESP_LOGW(TAG, "SD asset '%s' size %ld rejected (cap=%u)",
                 path, file_size, (unsigned)RWAVE_SD_CLIP_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    size_t size = (size_t)file_size;
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free < size + (256U * 1024U)) {
        fclose(f);
        ESP_LOGW(TAG, "SD asset '%s' needs %u B PSRAM; free=%u B",
                 path, (unsigned)size, (unsigned)psram_free);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        ESP_LOGW(TAG, "no PSRAM (%u B) to preload SD asset '%s'",
                 (unsigned)size, path);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(buf, 1, size, f);
    bool read_ok = (n == size && !ferror(f));
    fclose(f);
    if (!read_ok) {
        ESP_LOGW(TAG, "short read for SD asset '%s' (%u/%u B)",
                 path, (unsigned)n, (unsigned)size);
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    if (rwave_eaf_frame_count(buf, size) < 2) {
        ESP_LOGW(TAG, "SD asset '%s' is not a valid EAF/AAF clip", path);
        heap_caps_free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = buf;
    *owned = buf;
    *out_size = size;
    ESP_LOGI(TAG, "acquire '%s': %u B SD->PSRAM @%p", path, (unsigned)size, (void *)buf);
    return ESP_OK;
}

/* ROOT CAUSE (boot loop): this build sets CONFIG_SPIRAM_XIP_FROM_PSRAM, so emote
 * mounts the `emote` partition with mmap_enable=false (emote.c:252). In that mode
 * mmap_assets_get_mem (and thus emote_get_asset_data_by_name) returns a raw
 * PARTITION OFFSET — NOT a mapped pointer (observed 0x001f97be). The eye works
 * because emote_set_anim_emoji copies the blob out of flash via emote_acquire_data
 * (-> esp_partition_read) before use; that helper is private to the expression
 * component, so we replicate it here with the public esp_partition API.
 *
 * Acquire a dereferenceable EAF buffer for `name`: if the asset pointer is in the
 * mmap window, alias it (zero copy); otherwise treat it as a `emote`-partition
 * offset and esp_partition_read `size` bytes into a heap buffer. Returns the
 * usable buffer in *out (and, when copied, the owning allocation in *owned). */
static esp_err_t rwave_acquire(const char *name, const uint8_t **out,
                               uint8_t **owned, size_t *out_size)
{
    *out = NULL;
    *owned = NULL;
    *out_size = 0;

    esp_err_t sd_err = rwave_acquire_sd(name, out, owned, out_size);
    if (sd_err == ESP_OK) {
        return ESP_OK;
    }
    if (sd_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "SD override for '%s' unavailable (%s); falling back to partition",
                 name, esp_err_to_name(sd_err));
    }

    const uint8_t *data = NULL;
    size_t size = 0;
    esp_err_t err = emote_get_asset_data_by_name(s_rw.emote, name, &data, &size);
    if (err != ESP_OK || !data || size < 16) {
        ESP_LOGW(TAG, "asset '%s' not found (%s)", name, esp_err_to_name(err));
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    if (rwave_ptr_is_mapped(data)) {
        *out = data;                 /* memory-mapped: use in place, no copy */
        *out_size = size;
        ESP_LOGI(TAG, "acquire '%s': %zu B mmap @%p [%02x %02x %02x %02x %02x %02x %02x %02x...]",
                 name, size, (void *)data,
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
        return ESP_OK;
    }

    /* Non-mapped: `data` is a byte offset into the `emote` partition (it already
     * points past the 2-byte asset wrapper magic; `size` is the table asset_size,
     * which INCLUDES those 2 magic bytes — reading `size` bytes from the post-magic
     * offset reads 2 bytes of trailing slack, exactly as the eye path does, and the
     * EAF self-describes its own length so the slack is harmless). */
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                 EMOTE_ASSETS_PARTITION);
    if (!part) {
        ESP_LOGW(TAG, "'%s' is a partition offset but '%s' partition not found",
                 name, EMOTE_ASSETS_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }
    size_t off = (size_t)data;
    if (off + size > part->size) {
        ESP_LOGW(TAG, "asset '%s' offset+size (%zu) exceeds partition (%u)",
                 name, off + size, (unsigned)part->size);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);   /* internal RAM fallback */
    }
    if (!buf) {
        ESP_LOGW(TAG, "no mem (%u B) to copy asset '%s'", (unsigned)size, name);
        return ESP_ERR_NO_MEM;
    }
    err = esp_partition_read(part, off, buf, size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_partition_read('%s' @0x%zx) failed: %s",
                 name, off, esp_err_to_name(err));
        free(buf);
        return err;
    }
    *out = buf;
    *owned = buf;
    *out_size = size;
    ESP_LOGI(TAG, "acquire '%s': %zu B flash-copied (off=0x%zx) @%p [%02x %02x %02x %02x %02x %02x %02x %02x...]",
             name, size, off, (void *)buf,
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    return ESP_OK;
}

static esp_err_t rwave_load_clip(const char *name, rwave_clip_t *clip)
{
    const uint8_t *data = NULL;
    uint8_t *owned = NULL;
    size_t size = 0;
    esp_err_t err = rwave_acquire(name, &data, &owned, &size);
    if (err != ESP_OK) {
        clip->loaded = false;
        return err;
    }
    /* Defense-in-depth: after acquire, `data` is guaranteed dereferenceable (it is
     * either the mmap window or a heap copy). Validate the EAF header before use. */
    uint32_t total = rwave_eaf_frame_count(data, size);
    if (total < 2) {
        ESP_LOGW(TAG, "asset '%s' has too few/invalid frames (%u)", name, (unsigned)total);
        free(owned);
        clip->loaded = false;
        return ESP_ERR_INVALID_SIZE;
    }
    clip->data = data;
    clip->buf = owned;               /* NULL when mmap-aliased; freed in deinit */
    clip->size = size;
    clip->frames = total;
    clip->last = total - 1;          /* max set_segment end (exclusive draw window) */
    clip->loaded = true;
    ESP_LOGI(TAG, "loaded '%s' (%u B, %u real frames, %s)", name,
             (unsigned)size, (unsigned)(total - 1),
             owned ? "flash-copied" : "mmap");   /* real frames = total - 1 (_C) */
    return ESP_OK;
}

static const rwave_clip_t *rwave_clip_for(emote_face_state_t st)
{
    switch (st) {
    case EMOTE_FACE_IDLE:      return &s_rw.idle;
    case EMOTE_FACE_LISTENING: return &s_rw.listen;
    case EMOTE_FACE_THINKING:  return &s_rw.think;
    case EMOTE_FACE_SPEAKING:  return &s_rw.speak;
    default:                   return NULL;
    }
}

/* ---- amplitude source ----------------------------------------------------- */

static float rwave_target_amp(emote_face_state_t st)
{
    int milli;
    /* Snapshot the callback pointer once. emote_face_set_amplitude_source()
     * may swap or clear amp_cb from another task, so a check-then-call on the
     * live field could dereference a pointer cleared between the two reads. */
    emote_face_amp_cb_t cb = s_rw.amp_cb;
    if (s_rw.synth_amp >= 0) {
        milli = s_rw.synth_amp;          /* CLI synthetic 0..1000 */
    } else if (cb) {
        milli = cb(st);                  /* live mic/out RMS, 0..1000 */
    } else {
        milli = 0;                       /* no source → flat (full loop still moves) */
    }
    if (milli < 0)    milli = 0;
    if (milli > 1000) milli = 1000;
    return milli / 1000.0f;
}

/* ---- segment programming -------------------------------------------------- */

/* Point the object's src at `clip` (only when it changes) then program the loop.
 * Must be called under emote_lock. */
static void rwave_apply_segment(const rwave_clip_t *clip, uint32_t start, uint32_t end, uint32_t fps)
{
    if (!clip || !clip->loaded || !s_rw.obj) {
        return;
    }
    if (s_rw.cur_clip != clip) {
        esp_err_t set_ret = gfx_anim_set_src(s_rw.obj, clip->data, clip->size);
        if (set_ret != ESP_OK) {
            ESP_LOGE(TAG, "gfx_anim_set_src FAILED: %s", esp_err_to_name(set_ret));
            return;
        }
        ESP_LOGI(TAG, "rwave_apply_segment: new clip set (%u frames, %zu B)",
                 (unsigned)clip->frames, clip->size);
        s_rw.cur_clip = clip;
        s_rw.last_start = -1;            /* force a fresh set_segment */
        s_rw.last_end = -1;              /* force a fresh set_segment */
    }
    if (start >= clip->last) start = clip->last - 1;
    if (end <= start)       end = start + 1;     /* end>start so the loop draws */
    if (end > clip->last)   end = clip->last;
    if ((int)start == s_rw.last_start && (int)end == s_rw.last_end) {
        /* The gfx engine can silently stop a custom anim after a long HTTP/SD
         * diagnostic read. A cheap keepalive keeps the object invalidated and
         * restarts it if is_playing was dropped. */
        gfx_anim_start(s_rw.obj);
        gfx_obj_set_visible(s_rw.obj, true);
        s_rw.keepalive_kicks++;
        return;                          /* no change → don't thrash the engine */
    }
    s_rw.last_start = (int)start;
    s_rw.last_end = (int)end;
    gfx_anim_set_segment(s_rw.obj, start, end, fps, /*repeat=*/true);
    gfx_anim_start(s_rw.obj);
    gfx_obj_set_visible(s_rw.obj, true);
    s_rw.segment_sets++;
}

/* ---- driver task ---------------------------------------------------------- */

static void rwave_task(void *arg)
{
    (void)arg;
    while (s_rw.run) {
        s_rw.driver_ticks++;
        emote_face_state_t st = s_rw.state;

        /* Apply visibility changes whenever state transitions. Called WITHOUT the
         * emote lock — emote_set_obj_visible/emote_set_anim_visible acquire their
         * own internal lock; wrapping them in emote_lock() causes a recursive
         * deadlock. The rwave_task is the only writer of applied_state. */
        if (st != s_rw.applied_state && s_rw.obj) {
            bool show = (st != EMOTE_FACE_OFF);
            emote_set_obj_visible(s_rw.emote, RWAVE_OBJ_NAME, show);
            emote_set_anim_visible(s_rw.emote, !show);
            if (st == EMOTE_FACE_IDLE || st == EMOTE_FACE_THINKING) {
                s_rw.disp_amp = 0.0f;
            }
            s_rw.applied_state = st;
            s_rw.state_changes++;
            ESP_LOGI(TAG, "rwave state applied=%d", (int)st);
        }

        const rwave_clip_t *clip = rwave_clip_for(st);

        if (st == EMOTE_FACE_OFF || !s_rw.obj || !clip || !clip->loaded) {
            /* Log why we're skipping, but only once per state transition */
            static emote_face_state_t last_skip_state = EMOTE_FACE_OFF;
            static int last_skip_reason = 0;
            int reason = (st == EMOTE_FACE_OFF) ? 1 : (!s_rw.obj) ? 2 : (!clip) ? 3 : (!clip->loaded) ? 4 : 0;
            if (st != last_skip_state || reason != last_skip_reason) {
                ESP_LOGW(TAG, "rwave_task skip: state=%d reason=%d (1=OFF 2=no_obj 3=no_clip 4=not_loaded)",
                         (int)st, reason);
                last_skip_state = st;
                last_skip_reason = reason;
            }
            vTaskDelay(pdMS_TO_TICKS(RWAVE_DRIVER_MS));
            continue;
        }

        /* ease displayed amplitude toward target for fluid motion */
        float target = (st == EMOTE_FACE_LISTENING || st == EMOTE_FACE_SPEAKING)
                           ? rwave_target_amp(st) : 0.0f;
        s_rw.disp_amp += (target - s_rw.disp_amp) * RWAVE_LERP;

        uint32_t start = 0, end, fps;
        if (st == EMOTE_FACE_LISTENING || st == EMOTE_FACE_SPEAKING) {
            /* Quantize amplitude into loop ends. Calling gfx_anim_set_segment()
             * resets current_frame, so changing the end every 30 Hz tick makes
             * the animation snap back to frame 0. Keeping start at 0 is
             * important: listen/speak are monotonic ramps, and a tiny bucket
             * window around a steady level can be visually indistinguishable
             * from a frozen frame. */
            float a = s_rw.disp_amp;
            if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
            uint32_t bucket = (uint32_t)(a * (float)RWAVE_AMP_BUCKETS + 0.5f);
            if (bucket > RWAVE_AMP_BUCKETS) {
                bucket = RWAVE_AMP_BUCKETS;
            }
            start = 0;
            end = 1 + (bucket * (clip->last - 1)) / RWAVE_AMP_BUCKETS;
            /* Quiet-voice floor: a near-silent level would otherwise loop only
             * frames [0..2-4] — at 30 fps that 100-150 ms micro-loop reads as
             * strobing, not motion. The ramp's early frames carry subtle
             * baked motion, so a window of at least last/RWAVE_MIN_END_DIV
             * always animates while still reading as "quiet". */
            uint32_t min_end = clip->last / RWAVE_MIN_END_DIV;
            if (min_end < 2) {
                min_end = 2;
            }
            if (end < min_end) {
                end = min_end;
            }
            fps = RWAVE_FPS_REACTIVE;
        } else if (st == EMOTE_FACE_THINKING) {
            end = clip->last;            /* full self-contained sweep loop */
            fps = RWAVE_FPS_THINK;
        } else { /* IDLE */
            end = clip->last;            /* full breathing loop */
            fps = RWAVE_FPS_IDLE;
        }

        if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
            emote_lock(s_rw.emote);
            rwave_apply_segment(clip, start, end, fps);
            emote_unlock(s_rw.emote);
        } else {
            static bool logged_no_owner = false;
            if (!logged_no_owner) {
                ESP_LOGW(TAG, "rwave_task: display arbiter NOT owned by EMOTE — waiting");
                logged_no_owner = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RWAVE_DRIVER_MS));
    }
    s_rw.task = NULL;
    vTaskDelete(NULL);
}

/* ---- public API (matches emote.h declarations) ---------------------------- */

esp_err_t emote_face_init(emote_handle_t emote)
{
    ESP_RETURN_ON_FALSE(emote, ESP_ERR_INVALID_ARG, TAG, "null emote handle");
    if (s_rw.obj) {
        return ESP_OK;   /* already initialised */
    }
    s_rw.emote = emote;
    s_rw.synth_amp = -1;
    s_rw.last_end = -1;
    s_rw.cur_clip = NULL;

    /* Load the four baked clips from the `emote` asset partition. A missing clip
     * is non-fatal: that state just won't show (best-effort, like emote.c's
     * face-init guard). At least one must load for the face to be useful. */
    int ok = 0;
    ok += (rwave_load_clip(RWAVE_ASSET_IDLE,   &s_rw.idle)   == ESP_OK);
    ok += (rwave_load_clip(RWAVE_ASSET_LISTEN, &s_rw.listen) == ESP_OK);
    ok += (rwave_load_clip(RWAVE_ASSET_THINK,  &s_rw.think)  == ESP_OK);
    ok += (rwave_load_clip(RWAVE_ASSET_SPEAK,  &s_rw.speak)  == ESP_OK);
    if (ok == 0) {
        ESP_LOGW(TAG, "no reactive face assets found in '%s' partition — face "
                      "disabled, baked emote eye stays up", "emote");
        return ESP_OK;   /* don't fail emote_start; the eye still works */
    }

    /* Create the custom anim object on the emote display, centred, hidden. */
    s_rw.obj = emote_create_obj_by_type(emote, EMOTE_OBJ_TYPE_ANIM, RWAVE_OBJ_NAME);
    ESP_RETURN_ON_FALSE(s_rw.obj, ESP_FAIL, TAG, "failed to create rwave anim obj");
    gfx_obj_set_size(s_rw.obj, 466, 466);
    gfx_obj_align(s_rw.obj, GFX_ALIGN_CENTER, 0, 0);
    emote_set_obj_visible(emote, RWAVE_OBJ_NAME, false);

    s_rw.run = true;
    /* Boot directly into the IDLE breathing face so the device shows the
     * waveform identity from the moment the display arbiter hands EMOTE
     * ownership — no blank panel waiting for a tap. */
    s_rw.state = EMOTE_FACE_IDLE;
    BaseType_t started = xTaskCreate(rwave_task, "rwave", 4096, NULL, 3, &s_rw.task);
    ESP_RETURN_ON_FALSE(started == pdPASS, ESP_FAIL, TAG, "failed to start rwave task");

    ESP_LOGI(TAG, "reactive waveform face ready (baked EAF, %d clips loaded)", ok);
    return ESP_OK;
}

esp_err_t emote_face_set_state(emote_face_state_t state)
{
    /* Pure state write — intentionally no emote engine calls here.
     * emote_set_obj_visible / emote_set_anim_visible acquire the emote lock,
     * which the render task holds mid-flush. Calling them from arbitrary tasks
     * (e.g. the Gemini session task) causes a permanent deadlock when the render
     * task is busy. The rwave_task detects applied_state != state on its next tick
     * and applies visibility within its own emote_lock region. */
    s_rw.state = state;
    return ESP_OK;
}

void emote_face_set_synthetic_amplitude(int amp_milli)
{
    s_rw.synth_amp = amp_milli;   /* <0 restores the live cb */
}

void emote_face_set_amplitude_source(emote_face_amp_cb_t cb)
{
    s_rw.amp_cb = cb;
}

esp_err_t emote_face_get_debug(emote_face_debug_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "face debug is NULL");
    memset(out, 0, sizeof(*out));
    emote_face_state_t st = s_rw.state;
    const rwave_clip_t *clip = rwave_clip_for(st);
    out->state = (int)st;
    out->applied_state = (int)s_rw.applied_state;
    out->synth_amp = s_rw.synth_amp;
    out->displayed_amp = s_rw.disp_amp;
    out->last_start = s_rw.last_start;
    out->last_end = s_rw.last_end;
    out->driver_ticks = s_rw.driver_ticks;
    out->segment_sets = s_rw.segment_sets;
    out->keepalive_kicks = s_rw.keepalive_kicks;
    out->state_changes = s_rw.state_changes;
    out->running = s_rw.run;
    out->object_ready = s_rw.obj != NULL;
    out->display_owned = display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE);
    out->current_clip_loaded = clip && clip->loaded;
    return ESP_OK;
}
