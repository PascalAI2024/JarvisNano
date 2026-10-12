/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_display/companion.h"

#include <stddef.h>
#include <string.h>

static bool time_valid(const jr_companion_input_t *input)
{
    return input->time_valid && input->hour < 24U && input->minute < 60U;
}

static bool night_interval(const jr_companion_input_t *input)
{
    if (!time_valid(input) || input->night_start_hour >= 24U ||
        input->night_end_hour >= 24U ||
        input->night_start_hour == input->night_end_hour) {
        return false;
    }
    if (input->night_start_hour < input->night_end_hour) {
        return input->hour >= input->night_start_hour &&
               input->hour < input->night_end_hour;
    }
    return input->hour >= input->night_start_hour ||
           input->hour < input->night_end_hour;
}

static void line_set(jr_companion_line_t *line, const char *text,
                     int y, unsigned scale)
{
    size_t n = 0;
    while (n < JR_COMPANION_ITEM_CHARS && text[n] != '\0') {
        line->text[n] = text[n];
        ++n;
    }
    line->text[n] = '\0';
    if (n != 0U) {
        line->x = (int16_t)(232 - (int)(6U * scale * n) / 2);
        line->y = (int16_t)y;
        line->scale = (uint8_t)scale;
    }
}

/* Exactly the existing surface font's display-safe alphabet. Unsupported
 * bytes are explicit question marks, rather than silently invisible glyphs. */
static char display_char(unsigned char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (unsigned char)(ch - 'a' + 'A');
    }
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        (ch != 0U && strchr(" -_.:/%?!+,'()&*", (int)ch) != NULL)) {
        return (char)ch;
    }
    return '?';
}

static void next_text(const jr_companion_input_t *input, char text[25])
{
    switch (input->next_state) {
    case JR_COMPANION_NEXT_NONE:
        memcpy(text, "NO NEXT ITEM", sizeof "NO NEXT ITEM");
        return;
    case JR_COMPANION_NEXT_STALE:
        memcpy(text, "NEXT STALE", sizeof "NEXT STALE");
        return;
    case JR_COMPANION_NEXT_VALID: {
        size_t n = 0;
        bool visible = false;
        while (n < JR_COMPANION_ITEM_CHARS && input->next_item[n] != '\0') {
            text[n] = display_char((unsigned char)input->next_item[n]);
            visible = visible || text[n] != ' ';
            ++n;
        }
        text[n] = '\0';
        if (visible) {
            return;
        }
        break;
    }
    default:
        break;
    }
    memcpy(text, "NEXT UNAVAILABLE", sizeof "NEXT UNAVAILABLE");
}

static void battery_text(uint8_t percent, char text[25])
{
    memcpy(text, "BAT ", 4U);
    size_t n = 4U;
    if (percent > 100U) {
        memcpy(text + n, "--", sizeof "--");
        return;
    }
    if (percent >= 100U) {
        text[n++] = '1';
    }
    if (percent >= 10U) {
        text[n++] = (char)('0' + (percent / 10U) % 10U);
    }
    text[n++] = (char)('0' + percent % 10U);
    text[n++] = '%';
    text[n] = '\0';
}

bool jr_companion_build(const jr_companion_input_t *input,
                        jr_companion_view_t *out_view)
{
    if (out_view == NULL) {
        return false;
    }
    memset(out_view, 0, sizeof *out_view);
    if (input == NULL ||
        (input->requested != JR_COMPANION_NIGHTSTAND &&
         input->requested != JR_COMPANION_GLANCE) ||
        !input->idle || input->foreground || input->operator_active ||
        input->ota_active || input->probation ||
        (input->requested == JR_COMPANION_NIGHTSTAND && !input->usb_present)) {
        return false;
    }

    out_view->mode = input->requested;
    out_view->night = input->requested == JR_COMPANION_NIGHTSTAND &&
                      night_interval(input);
    out_view->privacy_muted = input->privacy_muted;
    out_view->charging = input->charging;
    out_view->battery_percent = input->battery_percent <= 100U
                                ? input->battery_percent : UINT8_MAX;
    out_view->background_rgb565 = 0x0000U;
    out_view->ink_rgb565 = 0x2104U;

    char text[JR_COMPANION_ITEM_CHARS + 1] = "--:--";
    if (time_valid(input)) {
        text[0] = (char)('0' + input->hour / 10U);
        text[1] = (char)('0' + input->hour % 10U);
        text[3] = (char)('0' + input->minute / 10U);
        text[4] = (char)('0' + input->minute % 10U);
    }
    line_set(&out_view->lines[0], text,
             input->requested == JR_COMPANION_GLANCE ? 180 : 222, 3U);
    if (input->requested == JR_COMPANION_GLANCE) {
        next_text(input, text);
        line_set(&out_view->lines[1], text, 224, 1U);
    }
    battery_text(out_view->battery_percent, text);
    line_set(&out_view->lines[2], text, 258, 1U);
    line_set(&out_view->lines[3], input->charging ? "CHARGING" :
             input->usb_present ? "USB" : "", 278, 1U);
    return true;
}

bool jr_companion_view_equal(const jr_companion_view_t *a,
                             const jr_companion_view_t *b)
{
    if (a == NULL || b == NULL || a->mode != b->mode ||
        a->night != b->night || a->privacy_muted != b->privacy_muted ||
        a->charging != b->charging || a->battery_percent != b->battery_percent ||
        a->background_rgb565 != b->background_rgb565 ||
        a->ink_rgb565 != b->ink_rgb565) {
        return false;
    }
    for (size_t i = 0; i < JR_COMPANION_LINE_COUNT; ++i) {
        if (a->lines[i].x != b->lines[i].x || a->lines[i].y != b->lines[i].y ||
            a->lines[i].scale != b->lines[i].scale ||
            strncmp(a->lines[i].text, b->lines[i].text,
                    JR_COMPANION_ITEM_CHARS + 1U) != 0) {
            return false;
        }
    }
    return true;
}
