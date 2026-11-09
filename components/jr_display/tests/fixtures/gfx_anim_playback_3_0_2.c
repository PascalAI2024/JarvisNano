/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unmodified playback-function excerpts from esp_emote_gfx3.0.2,
 * repository commit c78aa9bec705ac67b494dffab0f8ae3c5e27a843,
 * src/widget/gfx_anim.c. This licensed fixture lets host tests apply the
 * production managed patch without downloading ESP-IDF components.
 * When managed source exists the runner verifies these exact bodies agree.
 * See LICENSE.esp_emote_gfx in this directory.
 */

static void gfx_anim_timer_callback(void *arg)
{
    gfx_obj_t *obj = (gfx_obj_t *)arg;
    gfx_anim_t *anim = (gfx_anim_t *)obj->src;

    if (!anim || !anim->is_playing || obj->state.is_visible == false) {
        return;
    }

    if (anim->current_frame >= anim->end_frame) {
        if (anim->repeat) {
            ESP_LOGD(TAG, "Repeat");
            if (obj->disp && obj->disp->update_cb) {
                obj->disp->update_cb(obj->disp, GFX_DISP_EVENT_ALL_FRAME_DONE, obj);
            }
            anim->current_frame = anim->start_frame;
        } else {
            ESP_LOGD(TAG, "Done");
            anim->is_playing = false;
            if (obj->disp && obj->disp->update_cb) {
                obj->disp->update_cb(obj->disp, GFX_DISP_EVENT_ALL_FRAME_DONE, obj);
            }
            return;
        }
    } else {
        gfx_anim_prepare_frame(obj);
        anim->current_frame++;
        if (obj->disp && obj->disp->update_cb) {
            obj->disp->update_cb(obj->disp, GFX_DISP_EVENT_ONE_FRAME_DONE, obj);
        }
        ESP_LOGD(TAG, "Frame %" PRIu32 "/%" PRIu32, anim->current_frame, anim->end_frame);
    }

    gfx_obj_invalidate(obj);
}

esp_err_t gfx_anim_set_segment(gfx_obj_t *obj, uint32_t start, uint32_t end, uint32_t fps, bool repeat)
{
    CHECK_OBJ_TYPE_ANIMATION(obj);

    gfx_anim_t *anim = (gfx_anim_t *)obj->src;
    if (anim == NULL) {
        ESP_LOGE(TAG, "Animation property is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    int total_frames = eaf_get_total_frames(anim->file_desc);

    anim->start_frame = start;
    anim->end_frame = (end > total_frames - 1) ? (total_frames - 1) : end;
    anim->current_frame = start;

    if (anim->fps != fps) {
        anim->fps = fps;

        if (anim->timer != NULL) {
            uint32_t new_period_ms = 1000 / fps;
            gfx_timer_set_period(anim->timer, new_period_ms);
            ESP_LOGD(TAG, "FPS %" PRIu32 "->%" PRIu32, anim->fps, fps);
        }
    }

    anim->repeat = repeat;

    ESP_LOGD(TAG, "Segment [%" PRIu32 "-%" PRIu32 "] fps:%" PRIu32 " repeat:%d", anim->start_frame, anim->end_frame, fps, repeat);
    return ESP_OK;
}

esp_err_t gfx_anim_start(gfx_obj_t *obj)
{
    CHECK_OBJ_TYPE_ANIMATION(obj);

    gfx_anim_t *anim = (gfx_anim_t *)obj->src;
    if (anim == NULL) {
        ESP_LOGE(TAG, "Animation property is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    if (anim->file_desc == NULL) {
        ESP_LOGE(TAG, "Animation source not set");
        return ESP_ERR_INVALID_STATE;
    }

    if (anim->is_playing) {
        return ESP_OK;
    }

    anim->is_playing = true;
    anim->current_frame = anim->start_frame;

    ESP_LOGD(TAG, "Start");
    return ESP_OK;
}

esp_err_t gfx_anim_stop(gfx_obj_t *obj)
{
    CHECK_OBJ_TYPE_ANIMATION(obj);

    gfx_anim_t *anim = (gfx_anim_t *)obj->src;
    if (anim == NULL) {
        ESP_LOGE(TAG, "Animation property is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    if (!anim->is_playing) {
        return ESP_OK;
    }

    anim->is_playing = false;

    ESP_LOGD(TAG, "Stop");
    return ESP_OK;
}
