#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *gfx_handle_t;
typedef struct gfx_disp_t gfx_disp_t;
typedef struct gfx_obj_t gfx_obj_t;
typedef struct {
    int fps;
    struct { int task_priority; uint32_t task_stack; int task_affinity; unsigned task_stack_caps; } task;
} gfx_core_config_t;
typedef void (*gfx_flush_cb_t)(gfx_disp_t *disp, int x1, int y1, int x2, int y2, const void *pixels);
typedef struct {
    uint16_t h_res, v_res;
    gfx_flush_cb_t flush_cb;
    void *update_cb;
    void *user_data;
    struct { bool swap; bool buff_dma; bool buff_spiram; bool double_buffer; } flags;
    struct { void *buf1; void *buf2; size_t buf_pixels; } buffers;
} gfx_disp_config_t;
typedef enum { GFX_ALIGN_CENTER } gfx_align_t;
gfx_handle_t gfx_emote_init(const gfx_core_config_t *cfg);
gfx_disp_t *gfx_disp_add(gfx_handle_t h, const gfx_disp_config_t *cfg);
void *gfx_disp_get_user_data(gfx_disp_t *d);
esp_err_t gfx_disp_flush_ready(gfx_disp_t *d, bool ok);
void gfx_disp_refresh_all(gfx_disp_t *d);
esp_err_t gfx_emote_lock(gfx_handle_t h);
esp_err_t gfx_emote_unlock(gfx_handle_t h);
gfx_obj_t *gfx_anim_create(gfx_disp_t *d);
esp_err_t gfx_obj_align(gfx_obj_t *o, gfx_align_t a, int x, int y);
esp_err_t gfx_obj_set_visible(gfx_obj_t *o, bool v);
esp_err_t gfx_anim_set_src(gfx_obj_t *o, const void *data, size_t size);
esp_err_t gfx_anim_set_segment(gfx_obj_t *o, uint32_t start, uint32_t end, uint32_t fps, bool repeat);
esp_err_t gfx_anim_start(gfx_obj_t *o);
esp_err_t gfx_anim_stop(gfx_obj_t *o);
uint32_t gfx_timer_get_actual_fps(gfx_handle_t h);
